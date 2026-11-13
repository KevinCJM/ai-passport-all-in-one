// The phone setup page in English, in an isolated headless Chrome (no badge): each tab's text, the language switch,
// English city and currency names. Screenshots and the Chinese still shown go to releases/i18n-english-test/portal.
//   node scripts/test_passport_portal_english.mjs
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import http from 'node:http';
import {spawn} from 'node:child_process';
import {fileURLToPath} from 'node:url';
const root=path.resolve(path.dirname(fileURLToPath(import.meta.url)),'..');
const out=path.join(root,'releases/i18n-english-test/portal');fs.mkdirSync(out,{recursive:true});
const html=fs.readFileSync(path.join(root,'firmware/passport_device/main/wifi_portal_page.hpp'),'utf8').split('R"PORTAL(')[1].split(')PORTAL"')[0];
let language=1;const posts=[];
const replies={'/wifi/status':()=>({phase:'idle',language,bluetooth_enabled:true,bluetooth_name:'AI-Passport-1234'}),'/wifi/scan':()=>({scanning:false,networks:[{ssid_hex:'486f6d65',rssi:-50,secure:true},{ssid_hex:'436166e9',rssi:-80,secure:false}]}),
 '/media/prefs':()=>({city:'杭州',country:'CN',province:'浙江',latitude:30.27,longitude:120.16,radios:18}),'/rates/prefs':()=>({base:'CNY',targets:['USD','JPY']}),
 '/podcast/list':()=>({items:[{id:1,n:'声动早咖啡'}]}),'/stock/list':()=>({items:[{id:'sh600519',n:'贵州茅台'}]}),'/wheel/list':()=>({modes:[{n:'吃饭',o:['面条','米饭']}]})};
const server=http.createServer(async(req,res)=>{
 if(req.url==='/'){res.setHeader('Content-Type','text/html; charset=utf-8');res.end(html);return;}
 if(req.url==='/menu-editor.js'){res.setHeader('Content-Type','text/javascript');res.end(fs.readFileSync(path.join(root,'scripts/passport_menu.js')));return;}
 res.setHeader('Content-Type','application/json; charset=utf-8');
 if(req.method==='POST'){let body='';for await(const chunk of req)body+=chunk;posts.push([req.url,JSON.parse(body||'{}')]);if(req.url==='/language')language=JSON.parse(body).language;res.end('{"saved":true}');return;}
 if(req.url==='/identity/data'){res.statusCode=404;res.end('{}');return;}
 const reply=replies[req.url.split('?')[0]];res.end(JSON.stringify(reply?reply():{}));
});
await new Promise(resolve=>server.listen(0,'127.0.0.1',resolve));
const base=`http://127.0.0.1:${server.address().port}`;
const profile=fs.mkdtempSync(path.join(os.tmpdir(),'passport-portal-en-'));
const chrome=spawn('/Applications/Google Chrome.app/Contents/MacOS/Google Chrome',['--headless=new','--no-first-run','--no-default-browser-check','--disable-background-networking','--remote-debugging-port=0',`--user-data-dir=${profile}`,'about:blank'],{stdio:'ignore'});
const pause=ms=>new Promise(resolve=>setTimeout(resolve,ms));
let socket;
try{
 const portFile=path.join(profile,'DevToolsActivePort');
 for(let i=0;!fs.existsSync(portFile)&&i<150;i++)await pause(100);
 const debugPort=Number(fs.readFileSync(portFile,'utf8').split('\n')[0]);
 const target=(await(await fetch(`http://127.0.0.1:${debugPort}/json/list`)).json()).find(x=>x.type==='page');
 socket=new WebSocket(target.webSocketDebuggerUrl);await new Promise((resolve,reject)=>{socket.onopen=resolve;socket.onerror=reject;});
 let sequence=0;const waiting=new Map(),exceptions=[];
 socket.onmessage=event=>{const v=JSON.parse(event.data);if(v.id){const p=waiting.get(v.id);if(p){waiting.delete(v.id);v.error?p.reject(Error(JSON.stringify(v.error))):p.resolve(v.result);}}else if(v.method==='Runtime.exceptionThrown')exceptions.push(v.params.exceptionDetails);};
 const call=(method,params={})=>new Promise((resolve,reject)=>{const id=++sequence;waiting.set(id,{resolve,reject});socket.send(JSON.stringify({id,method,params}));});
 const evaluate=async expression=>{const r=await call('Runtime.evaluate',{expression,awaitPromise:true,returnByValue:true});if(r.exceptionDetails)throw Error(JSON.stringify(r.exceptionDetails));return r.result.value;};
 const assert=(yes,message)=>{if(!yes)throw Error(message);};
 const until=async expression=>{for(let i=0;i<100;i++){if(await evaluate(expression))return;await pause(50);}throw Error('timeout: '+expression);};
 // Visible text and attributes still in Chinese (user data such as names is expected).
 const chinese=()=>evaluate(`(()=>{const re=/[\\u3400-\\u9fff\\u3000-\\u303f\\uff00-\\uffef]/,found=new Set();const w=document.createTreeWalker(document.body,NodeFilter.SHOW_TEXT);for(let n;(n=w.nextNode());){const e=n.parentElement;if(e&&e.closest('[hidden]')||!re.test(n.nodeValue)||e.closest('script'))continue;found.add(n.nodeValue.trim());}for(const e of document.querySelectorAll('[placeholder],[aria-label],[title],optgroup'))for(const a of ['placeholder','aria-label','title','label']){const v=e.getAttribute(a);if(v&&re.test(v))found.add(a+': '+v);}return [...found];})()`);
 await call('Runtime.enable');await call('Page.enable');await call('Emulation.setDeviceMetricsOverride',{width:390,height:1400,deviceScaleFactor:1,mobile:true});
 await call('Page.navigate',{url:base});await until(`document.documentElement.lang==='en'`);await pause(1500);
 const left={};
 for(const tab of ['wifi','card','bt','media','rate','wheel','menu']){
  await evaluate(`document.getElementById('tab-${tab}').click()`);await pause(1200);
  left[tab]=await chinese();
  const shot=await call('Page.captureScreenshot',{format:'png',captureBeyondViewport:true});fs.writeFileSync(path.join(out,`portal-${tab}.png`),Buffer.from(shot.data,'base64'));
 }
 assert(await evaluate(`document.getElementById('tab-card').textContent==='ID Card'`),'tab not translated');
 await evaluate(`document.getElementById('tab-media').click()`);await pause(300);
 assert(await evaluate(`[...document.getElementById('city').options].some(o=>o.textContent==='Hangzhou · Zhejiang')`),'Chinese city not in English');
 assert(await evaluate(`[...document.getElementById('city').options].some(o=>o.textContent.startsWith('Tokyo · Japan'))`),'overseas city not in English');
 assert(await evaluate(`document.getElementById('media-message').textContent==='18 local and provincial stations.'`),'pattern text not translated');
 await evaluate(`document.getElementById('tab-rate').click()`);await pause(300);
 assert(await evaluate(`[...document.getElementById('base').options].some(o=>o.textContent.startsWith('USD · US Dollar'))`),'currency not in English');
 // The switch posts the setting and reloads in Chinese.
 await evaluate(`document.getElementById('lang').click()`);await until(`document.documentElement.lang==='zh-CN'&&document.getElementById('lang').textContent==='English'`);
 assert(posts.some(([u,b])=>u==='/language'&&b.language===0),'language not posted');
 assert(await evaluate(`document.getElementById('tab-card').textContent==='身份卡'`),'Chinese not restored');
 assert(exceptions.length===0,'browser exceptions: '+JSON.stringify(exceptions));
 fs.writeFileSync(path.join(out,'chinese-left.json'),JSON.stringify(left,null,1)+'\n');
 console.log(JSON.stringify({status:'PASS',chinese_left:left},null,1));
 await call('Browser.close');
}finally{socket?.close();chrome.kill('SIGTERM');await new Promise(resolve=>server.close(resolve));await pause(300);fs.rmSync(profile,{recursive:true,force:true});}
