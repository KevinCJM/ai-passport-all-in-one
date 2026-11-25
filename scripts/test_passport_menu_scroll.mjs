// Menu editor on a phone: tapping ↑/↓ far down the list must not throw the page back to the top.
// Isolated headless Chrome, mocked badge API.   node scripts/test_passport_menu_scroll.mjs
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import http from 'node:http';
import {spawn} from 'node:child_process';
import {fileURLToPath} from 'node:url';
const root=path.resolve(path.dirname(fileURLToPath(import.meta.url)),'..');
const js=fs.readFileSync(path.join(root,'scripts/passport_menu.js'));
const page=`<!doctype html><meta name=viewport content="width=device-width"><body style="background:#111;color:#eee"><h1>top</h1><div id=m></div><script>${js}</script><script>
const groups=[0,1,2,3,4,5].map(g=>({name:'分类'+g,apps:Array.from({length:8},(_,i)=>({id:g*8+i,name:'功能'+(g*8+i)}))}));
const layout=PassportMenu.encode(PassportMenu.defaults(groups));
window.fetch=async u=>{const g=/group=(\\d)/.exec(u);return new Response(JSON.stringify(g?groups[+g[1]]:{layout,revision:1}));};
PassportMenu.mount(document.getElementById('m'));</script>`;
const server=http.createServer((req,res)=>{res.setHeader('Content-Type','text/html; charset=utf-8');res.end(page);});
await new Promise(resolve=>server.listen(0,'127.0.0.1',resolve));
const profile=fs.mkdtempSync(path.join(os.tmpdir(),'passport-menu-scroll-'));
const chrome=spawn('/Applications/Google Chrome.app/Contents/MacOS/Google Chrome',['--headless=new','--no-first-run','--no-default-browser-check','--disable-background-networking','--remote-debugging-port=0',`--user-data-dir=${profile}`,'about:blank'],{stdio:'ignore'});
const pause=ms=>new Promise(resolve=>setTimeout(resolve,ms));
let socket,failed=false;
try{
 const portFile=path.join(profile,'DevToolsActivePort');
 for(let i=0;!fs.existsSync(portFile)&&i<150;i++)await pause(100);
 const port=Number(fs.readFileSync(portFile,'utf8').split('\n')[0]);
 const target=(await(await fetch(`http://127.0.0.1:${port}/json/list`)).json()).find(x=>x.type==='page');
 socket=new WebSocket(target.webSocketDebuggerUrl);await new Promise((resolve,reject)=>{socket.onopen=resolve;socket.onerror=reject;});
 let seq=0;const waiting=new Map();
 socket.onmessage=e=>{const v=JSON.parse(e.data);const p=v.id&&waiting.get(v.id);if(p){waiting.delete(v.id);v.error?p.reject(Error(JSON.stringify(v.error))):p.resolve(v.result);}};
 const call=(method,params={})=>new Promise((resolve,reject)=>{const id=++seq;waiting.set(id,{resolve,reject});socket.send(JSON.stringify({id,method,params}));});
 const evaluate=async expression=>(await call('Runtime.evaluate',{expression,awaitPromise:true,returnByValue:true})).result.value;
 await call('Page.enable');await call('Emulation.setDeviceMetricsOverride',{width:390,height:700,deviceScaleFactor:2,mobile:true});await call('Emulation.setTouchEmulationEnabled',{enabled:true});
 await call('Page.navigate',{url:`http://127.0.0.1:${server.address().port}/`});
 for(let i=0;i<50&&!await evaluate(`document.querySelectorAll('.pm-item').length>40`);i++)await pause(100);
 // A real tap on the 30th row's ↓ (focus moves to the button, as on a phone).
 const before=await evaluate(`(()=>{const b=[...document.querySelectorAll('.pm-item')][30].querySelectorAll('button')[1];b.scrollIntoView({block:'center'});const r=b.getBoundingClientRect();window._tap=[r.x+r.width/2,r.y+r.height/2];return scrollY;})()`);
 const [x,y]=await evaluate('window._tap');
 for(const type of ['touchStart','touchEnd'])await call('Input.dispatchTouchEvent',{type,touchPoints:type==='touchEnd'?[]:[{x,y}]});
 await pause(400);
 const after=await evaluate('scrollY'),moved=await evaluate(`[...document.querySelectorAll('.pm-item')][31].textContent.includes('功能30')`);
 // The preview shows the screen of the item just moved, not the first one.
 const preview=await evaluate(`document.querySelector('.pm-preview').textContent`);
 if(!preview.includes('分类3')){console.error('FAIL: preview stayed on',preview.slice(0,20));failed=true;}
 // Ticking a function far down the picker keeps the picker where it was.
 const picker=await evaluate(`(()=>{const l=document.querySelector('.pm-picks');l.scrollTop=150;l.querySelectorAll('input')[40].click();return document.querySelector('.pm-picks').scrollTop;})()`);
 if(picker<100){console.error('FAIL: picker scrolled back to',picker);failed=true;}
 console.log({before,after,moved,picker});
 if(!moved){console.error('FAIL: the tap did not move the item');failed=true;}
 if(Math.abs(after-before)>60){console.error('FAIL: page jumped from',before,'to',after);failed=true;}
}finally{socket?.close();chrome.kill('SIGTERM');server.close();await pause(300);fs.rmSync(profile,{recursive:true,force:true});}
if(failed)process.exit(1);console.log('PASS menu editor keeps its scroll position');
