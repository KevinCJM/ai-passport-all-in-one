// Exercise the actual offline phone portal in an isolated Chrome profile; no badge is contacted.
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import http from 'node:http';
import {spawn} from 'node:child_process';
import {fileURLToPath} from 'node:url';
const root=path.resolve(path.dirname(fileURLToPath(import.meta.url)),'..');
const out=path.join(root,'releases/city-support-test');fs.mkdirSync(out,{recursive:true});
const html=fs.readFileSync(path.join(root,'firmware/passport_device/main/wifi_portal_page.hpp'),'utf8').split('R"PORTAL(')[1].split(')PORTAL"')[0];
let stored={city:'Kelowna',country:'CA',province:'加拿大',latitude:49.88,longitude:-119.49},posts=[];
const server=http.createServer(async(req,res)=>{
 if(req.url==='/'){res.setHeader('Content-Type','text/html; charset=utf-8');res.end(html);return;}
 if(req.url==='/menu-editor.js'){res.setHeader('Content-Type','text/javascript');res.end(fs.readFileSync(path.join(root,'scripts/passport_menu.js')));return;}
 res.setHeader('Content-Type','application/json; charset=utf-8');
 if(req.url==='/media/prefs'){res.end(JSON.stringify(stored));return;}
 if(req.url==='/media'&&req.method==='POST'){
  let body='';for await(const chunk of req)body+=chunk;
  stored=JSON.parse(body);posts.push(stored);res.end('{"saved":true}');return;
 }
 res.end(JSON.stringify(req.url.startsWith('/wifi/scan')?{networks:[],scanning:false}:req.url==='/wifi/status'?{phase:'idle'}:req.url==='/podcast/list'?{items:[]}:{}));
});
await new Promise(resolve=>server.listen(0,'127.0.0.1',resolve));
const base=`http://127.0.0.1:${server.address().port}`;
const profile=fs.mkdtempSync(path.join(os.tmpdir(),'passport-city-chrome-'));
const chrome=spawn('/Applications/Google Chrome.app/Contents/MacOS/Google Chrome',[
 '--headless=new','--no-first-run','--no-default-browser-check','--disable-background-networking',
 '--remote-debugging-port=0',`--user-data-dir=${profile}`,'about:blank'],{stdio:'ignore'});
const pause=ms=>new Promise(resolve=>setTimeout(resolve,ms));
let socket;
try{
 const portFile=path.join(profile,'DevToolsActivePort');
 for(let i=0;!fs.existsSync(portFile)&&i<150;i++)await pause(100);
 if(!fs.existsSync(portFile))throw Error('isolated Chrome did not start');
 const debugPort=Number(fs.readFileSync(portFile,'utf8').split('\n')[0]);
 const target=(await(await fetch(`http://127.0.0.1:${debugPort}/json/list`)).json()).find(x=>x.type==='page');
 socket=new WebSocket(target.webSocketDebuggerUrl);
 await new Promise((resolve,reject)=>{socket.onopen=resolve;socket.onerror=reject;});
 let sequence=0;const waiting=new Map(),exceptions=[];
 socket.onmessage=event=>{const value=JSON.parse(event.data);if(value.id){const pending=waiting.get(value.id);if(pending){waiting.delete(value.id);value.error?pending.reject(Error(JSON.stringify(value.error))):pending.resolve(value.result);}}else if(value.method==='Runtime.exceptionThrown')exceptions.push(value.params.exceptionDetails);};
 const call=(method,params={})=>new Promise((resolve,reject)=>{const id=++sequence;waiting.set(id,{resolve,reject});socket.send(JSON.stringify({id,method,params}));});
 const evaluate=async expression=>{const r=await call('Runtime.evaluate',{expression,awaitPromise:true,returnByValue:true});if(r.exceptionDetails)throw Error(JSON.stringify(r.exceptionDetails));return r.result.value;};
 const assert=(yes,message)=>{if(!yes)throw Error(message);};
 const until=async expression=>{for(let i=0;i<100;i++){if(await evaluate(expression))return;await pause(50);}throw Error('portal timeout: '+expression);};
 const search=async word=>evaluate(`(()=>{const input=document.getElementById('city-search');input.value=${JSON.stringify(word)};input.dispatchEvent(new Event('input'));return [...document.getElementById('city').options].filter(x=>x.value).map(x=>JSON.parse(x.value));})()`);
 const select=async(name,cc)=>evaluate(`(()=>{const s=document.getElementById('city');const o=[...s.options].find(x=>x.value&&JSON.parse(x.value).city===${JSON.stringify(name)}&&JSON.parse(x.value).country===${JSON.stringify(cc)});if(!o)return false;s.value=o.value;s.dispatchEvent(new Event('change'));return true;})()`);
 const save=async()=>{await evaluate(`document.getElementById('save-media').click()`);await until(`document.getElementById('media-message').textContent.includes('已保存')`);};
 await call('Runtime.enable');await call('Page.enable');
 await call('Emulation.setDeviceMetricsOverride',{width:390,height:1000,deviceScaleFactor:1,mobile:true});
 await call('Page.navigate',{url:base});await until(`!!document.getElementById('tab-media')`);
 await evaluate(`document.getElementById('tab-media').click()`);await until(`!document.getElementById('save-media').disabled`);
 assert(await evaluate(`JSON.parse(document.getElementById('city').value).city==='Kelowna'`),'unlisted IP city lost');
 // Portland, Maine shares the catalog name/country of Portland, Oregon.
 // Opening and saving the page must retain the IP location's actual coordinates.
 stored={city:'波特兰',country:'US',province:'美国',latitude:43.66,longitude:-70.26};
 await call('Page.reload');await until(`!!document.getElementById('tab-media')`);
 await evaluate(`document.getElementById('tab-media').click()`);await until(`!document.getElementById('save-media').disabled`);
 assert(await evaluate(`JSON.parse(document.getElementById('city').value).latitude===43.66`),'same-name IP coordinates lost on load');
 await search('Portland');await save();assert(stored.latitude===43.66&&stored.longitude===-70.26,'same-name IP coordinates overwritten on save');
 const count=await evaluate(`CITIES.reduce((n,g)=>n+g[2].length,0)`);assert(count>=200,'overseas catalog missing from portal');
 const regions=[['New York','纽约','US'],['Canada','温哥华','CA'],['Tokyo','东京','JP'],['Seoul','首尔','KR'],['Australia','悉尼','AU'],['Paris','巴黎','FR'],['Berlin','柏林','DE']];
 for(const [word,name,cc] of regions){const options=await search(word);assert(options.some(x=>x.city===name&&x.country===cc),'missing '+word);assert(await select(name,cc),'selection failed');await save();assert(stored.city===name&&stored.country===cc,'wrong save payload');}
 const london=await search('London');assert(london.some(x=>x.country==='CA')&&london.some(x=>x.country==='GB'),'same-name cities collapsed');
 for(const cc of ['CA','GB']){assert(await select('伦敦',cc),'London '+cc+' unavailable');await save();assert(stored.country===cc,'wrong London country');}
 const before=posts.length;await search('not-a-real-city-123');await evaluate(`document.getElementById('save-media').click()`);await pause(100);assert(posts.length===before,'empty search overwrote city');
 await search('温哥华');assert(await select('温哥华','CA'),'Chinese search failed');await save();
 await evaluate(`document.getElementById('media-panel').scrollIntoView()`);
 const shot=await call('Page.captureScreenshot',{format:'png',fromSurface:true});fs.writeFileSync(path.join(out,'city-portal-mobile.png'),Buffer.from(shot.data,'base64'));
 assert(await evaluate(`document.documentElement.scrollWidth<=window.innerWidth`),'mobile page overflows');
 assert(exceptions.length===0,'browser exceptions: '+JSON.stringify(exceptions));
 const report={status:'PASS',real_chrome:true,physical_device_accessed:false,portal_catalog_cities:count,regions:regions.map(x=>x[2]),unlisted_ip_city_retained:true,same_name_ip_coordinates_retained:true,london_disambiguated:true,english_country_search:true,empty_search_did_not_save:true,posts:posts.length,exceptions};
 fs.writeFileSync(path.join(out,'city-browser-verification.json'),JSON.stringify(report,null,2)+'\n');console.log(JSON.stringify(report));
 await call('Browser.close');
}finally{
 socket?.close();chrome.kill('SIGTERM');await new Promise(resolve=>server.close(resolve));
 // Remove only this test's temporary browser profile.
 await pause(300);fs.rmSync(profile,{recursive:true,force:true});
}
