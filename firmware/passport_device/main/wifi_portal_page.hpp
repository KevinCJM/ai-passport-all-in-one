#pragma once
namespace passport::portal {
inline constexpr char page[]=R"PORTAL(<!doctype html>
<html lang="zh-CN"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><meta name="color-scheme" content="dark"><title>AI Passport · 手机配置</title>
<style>
*{box-sizing:border-box}body{margin:0;background:#10191f;color:#e4ecee;font:16px/1.55 -apple-system,BlinkMacSystemFont,"Segoe UI",sans-serif}main{max-width:460px;margin:auto;padding:30px 20px 36px}header{display:flex;gap:12px;align-items:center;margin-bottom:28px}.mark{height:42px;width:42px;border:1px solid #39584b;border-radius:12px;display:grid;place-items:center;color:#91e0b0;font-size:22px}header strong{display:block;font-size:19px}small,.muted{color:#94a8b0}h1{font-size:27px;line-height:1.25;margin:0 0 10px}p{margin:8px 0 20px}.card{padding:20px;background:#1c2a32;border:1px solid #2c3e46;border-radius:18px;margin:20px 0}label{display:block;margin:14px 0 7px;font-size:14px}select,input,button{font:inherit;min-height:48px;border-radius:10px}select,input{width:100%;color:#e4ecee;background:#111c22;border:1px solid #49606c;padding:11px 12px}select:focus,input:focus,button:focus-visible{outline:2px solid #91e0b0;outline-offset:3px}button{cursor:pointer;border:0;padding:11px 16px;background:#91e0b0;color:#10231a;font-weight:650}.primary{width:100%;margin-top:22px}.text-button{background:none;padding:7px 0;color:#91e0b0;min-height:42px;font-size:14px}.row{display:flex;justify-content:space-between;align-items:center;gap:12px}.row label{margin:0}.password{position:relative}.password input{padding-right:65px}.password button{position:absolute;right:6px;top:4px;min-height:40px;padding:6px 8px;background:none;color:#94a8b0;font-size:14px}.check{display:flex;align-items:center;gap:9px;font-size:14px}.check input{width:18px;min-height:18px;margin:0}.feedback{padding:16px;border-radius:12px;background:#16282b;display:flex;gap:12px;align-items:flex-start;margin-top:16px}.feedback.error{background:#3a2424;color:#ffb5ab}.feedback.success{background:#19342a;color:#b4efc8}.spinner{flex:0 0 18px;height:18px;border:2px solid #42645d;border-top-color:#91e0b0;border-radius:50%;animation:spin .85s linear infinite;margin-top:3px}button:disabled{opacity:.5;cursor:wait}[hidden]{display:none!important}.note{font-size:13px;color:#94a8b0;margin-top:20px}.tabs{display:flex;gap:6px;background:#1c2a32;padding:4px;border-radius:12px;margin:-6px 0 24px}.tabs button{flex:1;min-height:42px;background:none;color:#94a8b0;font-weight:600}.tabs button[aria-selected=true]{background:#91e0b0;color:#10231a}.previews{display:flex;gap:18px;align-items:flex-end;flex-wrap:wrap;margin-bottom:6px}.previews figure{margin:0;display:flex;flex-direction:column;gap:8px;align-items:flex-start}canvas{background:#111c22;border:1px solid #49606c;border-radius:8px;display:block}#avatar-canvas{width:96px;height:156px}#photo-canvas{width:150px;height:200px}figcaption{font-size:13px;color:#94a8b0}.file{position:relative;display:inline-block;margin:0;padding:9px 14px;border:1px solid #49606c;border-radius:10px;color:#91e0b0;cursor:pointer;font-size:14px}.file:focus-within{outline:2px solid #91e0b0;outline-offset:3px}.file input{position:absolute;inset:0;opacity:0;width:100%;min-height:0;cursor:pointer}.switch{display:flex;align-items:center;gap:9px;margin:0}.switch input{width:22px;min-height:22px;margin:0}.pods{list-style:none;padding:0;margin:0}.pods li{display:flex;justify-content:space-between;align-items:center;gap:12px;padding:10px 0;border-top:1px solid #2c3e46}.pods li:first-child{border-top:0}.pods small{display:block}.pods button{min-height:38px;padding:6px 14px;font-size:14px;flex:none}.search{display:flex;gap:8px}.search button{flex:none}@keyframes spin{to{transform:rotate(360deg)}}@media(prefers-reduced-motion:reduce){.spinner{animation:none}}
</style></head><body><main>
<header><div class="mark" aria-hidden="true">◈</div><div><strong>AI Passport</strong><small>手机配置</small></div></header>
<nav class="tabs" role="tablist" aria-label="配置项目"><button type="button" role="tab" id="tab-wifi" aria-selected="true" aria-controls="wifi-panel">WiFi</button><button type="button" role="tab" id="tab-card" aria-selected="false" aria-controls="card-panel">身份卡</button><button type="button" role="tab" id="tab-bt" aria-selected="false" aria-controls="bt-panel">蓝牙</button><button type="button" role="tab" id="tab-media" aria-selected="false" aria-controls="media-panel">收听</button><button type="button" role="tab" id="tab-rate" aria-selected="false" aria-controls="rate-panel">行情</button><button type="button" role="tab" id="tab-wheel" aria-selected="false" aria-controls="wheel-panel">转盘</button></nav>
<div id="wifi-panel" role="tabpanel" aria-labelledby="tab-wifi">
<section id="setup"><h1>让 Passport 连上 WiFi</h1><p class="muted">选择附近的 2.4 GHz 网络。设置保存后，所有联网功能共用这一个连接。</p>
<form id="form" class="card"><div class="row"><label for="network">附近的 WiFi</label><button class="text-button" type="button" id="refresh">重新扫描</button></div>
<select id="network" required aria-describedby="scan-note"><option value="">正在扫描附近网络…</option></select><small id="scan-note">正在扫描，请稍候</small>
<label class="check"><input type="checkbox" id="manual">手动输入隐藏网络</label>
<div id="manual-wrap" hidden><label for="ssid">WiFi 名称</label><input id="ssid" maxlength="32" autocomplete="off" autocapitalize="none" spellcheck="false"></div>
<label for="password">WiFi 密码</label><div class="password"><input id="password" type="password" maxlength="64" autocomplete="new-password" aria-describedby="password-note"><button type="button" id="show" aria-label="显示密码">显示</button></div><small id="password-note">开放网络可留空。</small>
<button class="primary" id="connect" type="submit">连接 WiFi</button></form>
<div id="feedback" class="feedback" role="status" aria-live="polite" hidden><span class="spinner" id="spinner" hidden aria-hidden="true"></span><span id="message"></span></div>
<p class="note">没有自动弹出此页时，请在手机浏览器打开 <strong>http://192.168.8.1</strong>。密码只提交到这台 Passport；连接成功后才保存。</p>
</section><section id="success" class="card" hidden tabindex="-1" role="status" aria-labelledby="success-title" style="text-align:center;padding:36px 20px">
<div aria-hidden="true" style="font-size:52px;color:#91e0b0;margin-bottom:14px">✓</div><h1 id="success-title">连接成功</h1><p id="success-network" class="muted"></p><p>WiFi 设置已保存。</p><p class="note">热点将在 30 秒内关闭。可以关闭此页，手机切回原来的网络。</p></section>
</div>
<div id="card-panel" role="tabpanel" aria-labelledby="tab-card" hidden>
<h1>身份卡与照片</h1><p class="muted">在设备主页按上下键，可切换到身份卡或全屏照片。图片在手机上裁剪后再上传。</p>
<form id="card-form" class="card" novalidate><div class="previews">
<figure><canvas id="avatar-canvas" width="96" height="156" role="img" aria-label="头像预览"></canvas><figcaption>头像</figcaption><label class="file">选择头像<input type="file" id="avatar-file" accept="image/*"></label></figure>
<figure><canvas id="photo-canvas" width="240" height="320" role="img" aria-label="全屏照片预览"></canvas><figcaption>全屏照片</figcaption><label class="file">选择照片<input type="file" id="photo-file" accept="image/*"></label></figure></div>
<label for="name">姓名</label><input id="name" maxlength="63" autocomplete="name" required>
<label for="title">头衔</label><input id="title" maxlength="63" autocomplete="organization-title">
<label for="motto">签名</label><input id="motto" maxlength="159">
<button class="primary" id="save-card" type="submit">保存到 Passport</button></form>
<div id="card-feedback" class="feedback" role="status" aria-live="polite" hidden><span class="spinner" id="card-spinner" hidden aria-hidden="true"></span><span id="card-message"></span></div>
<p class="note">图片和文字只保存在这台 Passport，不上传到其他服务器。</p>
</div>
<div id="bt-panel" role="tabpanel" aria-labelledby="tab-bt" hidden>
<h1>蓝牙</h1><p class="muted">拔线后，语音输入、AI 额度和电脑状态通过蓝牙连接电脑助手。首次配对请在电脑助手中完成。</p>
<div class="card"><div class="row"><div><strong id="bt-name">AI Passport</strong><br><small id="bt-state">正在读取…</small></div><label class="switch"><input type="checkbox" id="bt-enabled" disabled>启用蓝牙</label></div></div>
<div id="bt-feedback" class="feedback" role="status" aria-live="polite" hidden><span class="spinner" id="bt-spinner" hidden aria-hidden="true"></span><span id="bt-message"></span></div>
</div>
<div id="media-panel" role="tabpanel" aria-labelledby="tab-media" hidden>
<h1>收音机</h1><p class="muted">选择所在城市，「收音机」会列出本地和省内电台。</p>
<form id="media-form" class="card" novalidate><label for="city">所在城市</label><select id="city" aria-describedby="city-note"></select><small id="city-note">用于选择本地和省内电台，以及主页天气。不使用 IP 或手机定位。</small>
<button class="primary" id="save-media" type="submit" disabled>保存</button></form>
<div id="media-feedback" class="feedback" role="status" aria-live="polite" hidden><span class="spinner" id="media-spinner" hidden aria-hidden="true"></span><span id="media-message"></span></div>
<p class="note">电台来自内置的蜻蜓 FM 电台表，只收录设备能播放的 MP3 台。</p>
<h1 style="margin-top:36px">播客</h1><p class="muted">「播客」页面列出这里的节目（最多 12 个），记住每个节目听到的位置。节目来自喜马拉雅的免费专辑。</p>
<div class="card"><label>已添加的节目</label><ul id="pod-list" class="pods"><li><small>正在读取…</small></li></ul></div>
<form id="pod-form" class="card" novalidate><label for="pod-kw">搜索节目</label><div class="search"><input id="pod-kw" maxlength="16" placeholder="例如：英语、历史、商业" enterkeyhint="search"><button type="submit" id="pod-go">搜索</button></div>
<ul id="pod-results" class="pods"></ul></form>
<div id="pod-feedback" class="feedback" role="status" aria-live="polite" hidden><span class="spinner" id="pod-spinner" hidden aria-hidden="true"></span><span id="pod-message"></span></div>
<p class="note">搜索由 Passport 通过它连接的 WiFi 完成，需要先连上能上网的 WiFi。付费专辑无法播放，不能添加。</p>
</div>
<div id="rate-panel" role="tabpanel" aria-labelledby="tab-rate" hidden>
<h1>汇率</h1><p class="muted">「汇率」页面用主要货币给其他货币标价，例如 1 美元 = 6.67 人民币。</p>
<form id="rate-form" class="card" novalidate><label for="base">主要货币</label><select id="base"></select>
<label>要查看的货币（最多 6 种）</label><div id="targets"></div>
<button class="primary" id="save-rate" type="submit" disabled>保存</button></form>
<div id="rate-feedback" class="feedback" role="status" aria-live="polite" hidden><span class="spinner" id="rate-spinner" hidden aria-hidden="true"></span><span id="rate-message"></span></div>
<p class="note">数据来自 open.er-api.com，每天更新一次，仅供参考。</p>
<h1 style="margin-top:36px">自选股</h1><p class="muted">「自选股」页面显示这里的股票、ETF 和指数（最多 12 个），支持 A 股、港股、美股，以及黄金、白银、原油和美元指数等外汇。</p>
<div class="card"><label>已添加</label><ul id="stock-list" class="pods"><li><small>正在读取…</small></li></ul></div>
<form id="stock-form" class="card" novalidate><label for="stock-kw">搜索</label><div class="search"><input id="stock-kw" maxlength="16" placeholder="名称、代码或拼音，例如 茅台、510300、黄金" enterkeyhint="search"><button type="submit" id="stock-go">搜索</button></div>
<ul id="stock-results" class="pods"></ul></form>
<div id="stock-feedback" class="feedback" role="status" aria-live="polite" hidden><span class="spinner" id="stock-spinner" hidden aria-hidden="true"></span><span id="stock-message"></span></div>
<p class="note">行情来自腾讯财经，搜索和黄金、原油、外汇的走势来自东方财富，均为公开接口，可能有延迟，仅供参考。搜索需要 Passport 先连上能上网的 WiFi。</p>
</div>
<div id="wheel-panel" role="tabpanel" aria-labelledby="tab-wheel" hidden>
<h1>随机转盘</h1><p class="muted">「随机转盘」页面按确认随机选一个。最多 5 个模式，每个模式 2–8 个选项，每项最多 8 个字。也可以在胸牌上长按上键，说「吃饭有烧烤、汉堡、炸鸡」添加。</p>
<form id="wheel-form" novalidate><div id="wheel-modes"></div>
<button type="button" class="text-button" id="wheel-add">＋ 添加模式</button>
<button class="primary" id="save-wheel" type="submit" disabled>保存</button></form>
<div id="wheel-feedback" class="feedback" role="status" aria-live="polite" hidden><span class="spinner" id="wheel-spinner" hidden aria-hidden="true"></span><span id="wheel-message"></span></div>
<p class="note">保存后，下次打开「随机转盘」生效。</p>
</div>
</main><script>
const $=id=>document.getElementById(id);let connecting=false,complete=false,scanTimer=null,statusTimer=null;
function feedback(message,kind=''){ $('feedback').hidden=false;$('feedback').className='feedback '+kind;$('message').textContent=message;$('spinner').hidden=kind!=='loading';}
function label(hex){try{return new TextDecoder().decode(Uint8Array.from(hex.match(/../g)||[],x=>parseInt(x,16)));}catch(_){return '未命名网络';}}
async function api(path,body){const result=await fetch(path,{cache:'no-store',...(body===undefined?{}:{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(body)})});let data={};try{data=await result.json();}catch(_){if(result.ok)throw Error('设备暂时没有响应，请重试');}if(!result.ok)throw Error(data.message||'设备暂时没有响应，请重试');return data;}
async function scan(force=false){if(connecting||complete)return;clearTimeout(scanTimer);$('refresh').disabled=true;$('scan-note').textContent='正在扫描附近网络…';try{const result=await api('/wifi/scan'+(force?'?refresh=1':''));if(complete)return;if(result.scanning){scanTimer=setTimeout(()=>scan(false),1200);return;}if(result.error)throw new Error('扫描暂不可用，请稍后重新扫描。');const selected=$('network').value;$('network').replaceChildren(new Option('请选择 WiFi',''));for(const item of result.networks||[]){const level=item.rssi>-60?'强':item.rssi>-75?'中':'弱';$('network').add(new Option(label(item.ssid_hex)+' · '+(item.secure?'加密':'开放')+' · '+level,item.ssid_hex));}if([...$('network').options].some(x=>x.value===selected))$('network').value=selected;$('scan-note').textContent=result.networks?.length?'仅显示 2.4 GHz 网络':'未找到网络，可重新扫描或手动输入。';}catch(error){$('scan-note').textContent=error.message;}finally{$('refresh').disabled=false;}}
function busy(value){connecting=value;$('connect').disabled=value;$('refresh').disabled=value;$('network').disabled=value||$('manual').checked;$('manual').disabled=value;$('show').disabled=value;$('ssid').disabled=value;$('password').disabled=value;}
function connected(result){if(complete)return;complete=true;clearTimeout(scanTimer);clearInterval(statusTimer);$('password').value='';$('setup').hidden=true;$('success-network').textContent=result.ssid_hex?'已连接 '+label(result.ssid_hex):'WiFi 已连接';$('success').hidden=false;document.title='AI Passport · 连接成功';$('success').focus({preventScroll:true});}
async function status(){try{const result=await api('/wifi/status');if(result.phase==='connected'){connected(result);return result.phase;}if(result.phase==='failed'){busy(false);feedback(result.message||'连接失败，请检查密码后重试。','error');clearInterval(statusTimer);}else if(result.phase==='connecting'){feedback('正在连接 '+label(result.ssid_hex)+'，请保持手机连接 Passport 热点。','loading');}return result.phase;}catch(error){if(!complete){feedback('暂时无法连接 Passport。请检查手机仍连接设备热点，再刷新本页。','error');busy(false);clearInterval(statusTimer);}return 'error';}}
$('manual').onchange=()=>{$('manual-wrap').hidden=!$('manual').checked;$('network').required=!$('manual').checked;$('network').disabled=$('manual').checked;$('ssid').required=$('manual').checked;};
$('show').onclick=()=>{const show=$('password').type==='password';$('password').type=show?'text':'password';$('show').textContent=show?'隐藏':'显示';$('show').setAttribute('aria-label',show?'隐藏密码':'显示密码');};
$('refresh').onclick=()=>scan(true);
$('form').onsubmit=async event=>{event.preventDefault();if(complete)return;const manual=$('manual').checked,ssid=$('ssid').value,password=$('password').value;if(manual&&(!ssid||new TextEncoder().encode(ssid).length>32)){feedback('WiFi 名称需要 1–32 字节，请缩短后重试。','error');return;}const bytes=new TextEncoder().encode(password).length;if(bytes&&((bytes<8||bytes>63)&&!(/^[a-fA-F0-9]{64}$/.test(password)))){feedback('密码需要 8–63 字节；开放网络请留空。','error');return;}const body=manual?{ssid,password}:{ssid_hex:$('network').value,password};busy(true);feedback('正在提交 WiFi 设置…','loading');clearTimeout(scanTimer);try{await api('/wifi',body);$('password').value='';clearInterval(statusTimer);statusTimer=setInterval(status,1000);await status();}catch(error){busy(false);feedback(error.message,'error');}};
function show(prefix,message,kind=''){const box=$(prefix+'-feedback');box.hidden=false;box.className='feedback '+kind;$(prefix+'-message').textContent=message;$(prefix+'-spinner').hidden=kind!=='loading';}
const tabs=[['tab-wifi','wifi-panel'],['tab-card','card-panel'],['tab-bt','bt-panel'],['tab-media','media-panel'],['tab-rate','rate-panel'],['tab-wheel','wheel-panel']];
for(const [tab,panel] of tabs)$(tab).onclick=()=>{for(const [other,area] of tabs){$(other).setAttribute('aria-selected',String(other===tab));$(area).hidden=other!==tab;}if(panel==='card-panel')loadCard();if(panel==='bt-panel')loadBluetooth();if(panel==='media-panel'){loadMedia();loadPods();}if(panel==='rate-panel'){loadRates();loadStocks();}if(panel==='wheel-panel')loadWheel();};
// Same layout as scripts/pack_passport_identity.py and firmware identity.hpp.
const AW=96,AH=156,PW=240,PH=320,AVATAR=0x1000,PHOTO=0x10000,TOTAL=PHOTO+PW*PH*2;let image=null,cardLoaded=false,cardBusy=false;
function crc32(bytes){let crc=-1;for(let i=0;i<bytes.length;i++){crc^=bytes[i];for(let k=0;k<8;k++)crc=crc&1?(crc>>>1)^0xedb88320:crc>>>1;}return (crc^-1)>>>0;}
function text(offset,size){const raw=image.subarray(offset,offset+size);const end=raw.indexOf(0);return new TextDecoder().decode(raw.subarray(0,end<0?size:end));}
function packIdentity(base,fields,flags){const out=new Uint8Array(base),enc=new TextEncoder();out.fill(0,16,304);
 for(const [value,offset,size,name] of [[fields.name,16,64,'姓名'],[fields.title,80,64,'头衔'],[fields.motto,144,160,'签名']]){const bytes=enc.encode(value);if(bytes.length>=size)throw Error(name+'太长，请缩短后重试');out.set(bytes,offset);}
 out.set([80,80,73,68],0);const view=new DataView(out.buffer);view.setUint32(4,1,true);view.setUint32(12,flags,true);view.setUint32(8,crc32(out.subarray(16)),true);return out;}
function encode(pixels,offset,alpha){const n=pixels.width*pixels.height,d=pixels.data;for(let i=0;i<n;i++){const v=(d[i*4]>>3)<<11|(d[i*4+1]>>2)<<5|d[i*4+2]>>3;image[offset+i*2]=v&255;image[offset+i*2+1]=v>>8;if(alpha)image[offset+n*2+i]=d[i*4+3];}}
function paint(canvas,offset,alpha){const w=canvas.width,h=canvas.height,n=w*h,pixels=new ImageData(w,h);for(let i=0;i<n;i++){const v=image[offset+i*2]|image[offset+i*2+1]<<8;pixels.data[i*4]=Math.round((v>>11&31)*255/31);pixels.data[i*4+1]=Math.round((v>>5&63)*255/63);pixels.data[i*4+2]=Math.round((v&31)*255/31);pixels.data[i*4+3]=alpha?image[offset+n*2+i]:255;}canvas.getContext('2d').putImageData(pixels,0,0);}
function flags(){return new DataView(image.buffer).getUint32(12,true);}
function setFlag(bit){new DataView(image.buffer).setUint32(12,flags()|bit,true);}
function crop(file,w,h){return new Promise((resolve,reject)=>{const picture=new Image();picture.onload=()=>{const canvas=document.createElement('canvas');canvas.width=w;canvas.height=h;const g=canvas.getContext('2d');const scale=Math.max(w/picture.width,h/picture.height),dw=picture.width*scale,dh=picture.height*scale;g.drawImage(picture,(w-dw)/2,(h-dh)/2,dw,dh);URL.revokeObjectURL(picture.src);resolve(g.getImageData(0,0,w,h));};picture.onerror=()=>{URL.revokeObjectURL(picture.src);reject(Error('无法读取这张图片，请换一张 JPG 或 PNG'));};picture.src=URL.createObjectURL(file);});}
async function choose(input,w,h,offset,alpha,bit,canvas){const file=input.files[0];if(!file||!image)return;try{encode(await crop(file,w,h),offset,alpha);setFlag(bit);paint(canvas,offset,alpha);$('card-feedback').hidden=true;}catch(error){show('card',error.message,'error');}finally{input.value='';}}
async function loadCard(){if(cardLoaded||cardBusy)return;cardBusy=true;$('save-card').disabled=true;show('card','正在读取设备上的身份卡…','loading');
 try{const result=await fetch('/identity/data',{cache:'no-store'});if(result.status===404){image=new Uint8Array(TOTAL).fill(255);image.fill(0,0,16);$('card-feedback').hidden=true;}
  else{if(!result.ok)throw Error('暂时无法读取身份卡，请稍后重试');const raw=new Uint8Array(await result.arrayBuffer());if(raw.length!==TOTAL)throw Error('读取不完整，请重新打开此页');image=raw;$('name').value=text(16,64);$('title').value=text(80,64);$('motto').value=text(144,160);$('card-feedback').hidden=true;}
  if(flags()&1)paint($('avatar-canvas'),AVATAR,true);if(flags()&2)paint($('photo-canvas'),PHOTO,false);cardLoaded=true;}
 catch(error){show('card',error.message,'error');}finally{cardBusy=false;$('save-card').disabled=!cardLoaded;}}
$('avatar-file').onchange=()=>choose($('avatar-file'),AW,AH,AVATAR,true,1,$('avatar-canvas'));
$('photo-file').onchange=()=>choose($('photo-file'),PW,PH,PHOTO,false,2,$('photo-canvas'));
$('card-form').onsubmit=event=>{event.preventDefault();if(!image||cardBusy)return;const name=$('name').value.trim();if(!name){show('card','请填写姓名','error');$('name').focus();return;}
 let body;try{body=packIdentity(image,{name,title:$('title').value.trim(),motto:$('motto').value.trim()},flags()&3);}catch(error){show('card',error.message,'error');return;}
 cardBusy=true;$('save-card').disabled=true;const request=new XMLHttpRequest();request.open('POST','/identity');request.setRequestHeader('Content-Type','application/octet-stream');
 request.upload.onprogress=event=>{if(event.lengthComputable)show('card',event.loaded<event.total?'正在上传 '+Math.round(event.loaded*100/event.total)+'%':'正在写入设备…','loading');};
 const done=(message,kind)=>{cardBusy=false;$('save-card').disabled=false;show('card',message,kind);};
 request.onload=()=>{let data={};try{data=JSON.parse(request.responseText);}catch(_){}if(request.status===200){image=body;done('已保存。在设备主页按上下键查看身份卡和照片。','success');}else done(data.message||'保存失败，请重新上传','error');};
 request.onerror=request.ontimeout=()=>done('上传中断。请确认手机仍连接 Passport 热点后重试。','error');request.timeout=60000;show('card','正在上传 0%','loading');request.send(body);};
async function loadBluetooth(){try{const result=await api('/wifi/status');$('bt-name').textContent=result.bluetooth_name||'AI Passport';$('bt-enabled').checked=Boolean(result.bluetooth_enabled);$('bt-enabled').disabled=false;$('bt-state').textContent=result.bluetooth_enabled?'已开启 · 拔线后自动连接电脑助手':'已关闭';}catch(error){show('bt',error.message,'error');}}
$('bt-enabled').onchange=async()=>{const enabled=$('bt-enabled').checked;$('bt-enabled').disabled=true;show('bt',enabled?'正在开启蓝牙…':'正在关闭蓝牙…','loading');try{await api('/bluetooth',{enabled});await new Promise(r=>setTimeout(r,800));await loadBluetooth();show('bt',enabled?'蓝牙已开启':'蓝牙已关闭','success');}catch(error){$('bt-enabled').checked=!enabled;$('bt-enabled').disabled=false;show('bt',error.message,'error');}};
// China only, coordinates included: the phone has no internet while it is on the Passport hotspot.
// [city, latitude, longitude, province]; the device lists built-in stations named after the city, then the rest of the province.
const CITIES=[['直辖市','CN',[['北京',39.90,116.41,''],['上海',31.23,121.47,''],['天津',39.13,117.20,''],['重庆',29.56,106.55,'']]],
 ['华北 · 东北','CN',[['石家庄',38.04,114.51,'河北'],['太原',37.87,112.55,'山西'],['呼和浩特',40.84,111.75,'内蒙古'],['沈阳',41.81,123.43,'辽宁'],['大连',38.91,121.61,'辽宁'],['长春',43.82,125.32,'吉林'],['哈尔滨',45.80,126.53,'黑龙江']]],
 ['华东','CN',[['南京',32.06,118.80,'江苏'],['苏州',31.30,120.59,'江苏'],['无锡',31.49,120.31,'江苏'],['杭州',30.27,120.16,'浙江'],['宁波',29.87,121.54,'浙江'],['合肥',31.82,117.23,'安徽'],['福州',26.07,119.30,'福建'],['厦门',24.48,118.09,'福建'],['南昌',28.68,115.86,'江西'],['济南',36.65,117.12,'山东'],['青岛',36.07,120.38,'山东']]],
 ['华中 · 华南','CN',[['郑州',34.75,113.62,'河南'],['武汉',30.59,114.31,'湖北'],['长沙',28.23,112.94,'湖南'],['广州',23.13,113.26,'广东'],['深圳',22.54,114.06,'广东'],['东莞',23.02,113.75,'广东'],['佛山',23.02,113.12,'广东'],['珠海',22.27,113.58,'广东'],['南宁',22.82,108.37,'广西'],['海口',20.04,110.32,'海南'],['三亚',18.25,109.51,'海南']]],
 ['西南 · 西北','CN',[['成都',30.57,104.07,'四川'],['贵阳',26.65,106.63,'贵州'],['昆明',25.04,102.71,'云南'],['拉萨',29.65,91.13,'西藏'],['西安',34.34,108.94,'陕西'],['兰州',36.06,103.83,'甘肃'],['西宁',36.62,101.78,'青海'],['银川',38.49,106.23,'宁夏'],['乌鲁木齐',43.83,87.62,'新疆']]],
 ['港澳台','',[['香港',22.32,114.17,'','HK'],['澳门',22.20,113.54,'','MO'],['台北',25.03,121.57,'','TW'],['高雄',22.63,120.30,'','TW']]]];
const CURRENCIES=[['CNY','人民币'],['USD','美元'],['EUR','欧元'],['JPY','日元'],['HKD','港币'],['GBP','英镑'],['KRW','韩元'],['TWD','新台币'],['MOP','澳门元'],['SGD','新加坡元'],['AUD','澳元'],['CAD','加元'],['NZD','新西兰元'],['CHF','瑞士法郎'],['THB','泰铢'],['MYR','林吉特'],['VND','越南盾'],['PHP','菲律宾比索'],['IDR','印尼盾'],['INR','印度卢比'],['RUB','卢布'],['AED','迪拉姆'],['SEK','瑞典克朗'],['TRY','土耳其里拉'],['BRL','巴西雷亚尔'],['MXN','墨西哥比索'],['ARS','阿根廷比索'],['CLP','智利比索'],['ZAR','南非兰特'],['EGP','埃及镑'],['SAR','沙特里亚尔'],['QAR','卡塔尔里亚尔'],['KWD','科威特第纳尔'],['ILS','以色列谢克尔'],['NOK','挪威克朗'],['DKK','丹麦克朗'],['PLN','波兰兹罗提'],['CZK','捷克克朗'],['HUF','匈牙利福林'],['UAH','乌克兰格里夫纳'],['KZT','哈萨克坚戈'],['MNT','蒙古图格里克'],['LAK','老挝基普'],['KHR','柬埔寨瑞尔'],['MMK','缅甸元'],['NPR','尼泊尔卢比'],['LKR','斯里兰卡卢比'],['PKR','巴基斯坦卢比'],['BDT','孟加拉塔卡']];
let wheelLoaded=false;const chars=s=>[...s].length;
function wheelCount(){$('wheel-add').hidden=$('wheel-modes').children.length>=5;}
function wheelCard(name='',options=[]){const card=document.createElement('div');card.className='card';card.innerHTML='<div class="row"><label>模式名称</label><button type="button" class="text-button">删除</button></div><input class="wheel-name" maxlength="8" placeholder="例如 吃饭"><label>选项（用逗号、顿号或空格分开，2–8 个）</label><input class="wheel-options" placeholder="烧烤、汉堡、炸鸡、面条、快餐">';card.querySelector('.wheel-name').value=name;card.querySelector('.wheel-options').value=options.join('、');card.querySelector('button').onclick=()=>{card.remove();wheelCount();};$('wheel-modes').append(card);wheelCount();}
async function loadWheel(){if(wheelLoaded)return;show('wheel','正在读取…','loading');try{const data=await api('/wheel/list');$('wheel-modes').replaceChildren();for(const m of data.modes||[])wheelCard(m.n,m.o);wheelLoaded=true;$('save-wheel').disabled=false;$('wheel-feedback').hidden=true;}catch(error){show('wheel',error.message,'error');}}
$('wheel-add').onclick=()=>{wheelCard();$('wheel-modes').lastChild.querySelector('input').focus();};
$('wheel-form').onsubmit=async event=>{event.preventDefault();const modes=[];
for(const card of $('wheel-modes').children){const n=card.querySelector('.wheel-name').value.trim(),o=[...new Set(card.querySelector('.wheel-options').value.split(/[\s,，、;；。.]+/).filter(Boolean))];if(!n&&!o.length)continue;
if(!n||chars(n)>8){show('wheel','模式名称需要 1–8 个字','error');return;}if(modes.some(m=>m.n===n)){show('wheel','模式「'+n+'」重复了','error');return;}
if(o.length<2||o.length>8){show('wheel','「'+n+'」需要 2–8 个选项','error');return;}const long=o.find(x=>chars(x)>8);if(long){show('wheel','选项「'+long+'」超过 8 个字','error');return;}modes.push({n,o});}
$('save-wheel').disabled=true;show('wheel','正在保存…','loading');try{await api('/wheel',{modes});show('wheel','已保存，下次打开「随机转盘」生效。','success');}catch(error){show('wheel',error.message,'error');}finally{$('save-wheel').disabled=false;}};
let ratesLoaded=false;
async function loadRates(){if(ratesLoaded)return;show('rate','正在读取…','loading');try{const prefs=await api('/rates/prefs');
 $('base').replaceChildren(...CURRENCIES.map(([code,name])=>new Option(name+' '+code,code)));$('base').value=prefs.base;
 $('targets').replaceChildren(...CURRENCIES.map(([code,name])=>{const label=document.createElement('label');label.className='check';const box=document.createElement('input');box.type='checkbox';box.value=code;box.checked=prefs.targets.includes(code);label.append(box,name+' '+code);return label;}));
 $('save-rate').disabled=false;ratesLoaded=true;$('rate-feedback').hidden=true;}catch(error){show('rate',error.message,'error');}}
$('rate-form').onsubmit=async event=>{event.preventDefault();const base=$('base').value,targets=[...$('targets').querySelectorAll('input:checked')].map(box=>box.value).filter(code=>code!==base);
 if(!targets.length||targets.length>6){show('rate','请选择 1–6 种与主要货币不同的货币','error');return;}
 $('save-rate').disabled=true;show('rate','正在保存…','loading');
 try{await api('/rates',{base,targets});show('rate','已保存，下次打开「汇率」页面生效。','success');}catch(error){show('rate',error.message,'error');}finally{$('save-rate').disabled=false;}};
let mediaLoaded=false;
function cityOptions(selected){const select=$('city');select.replaceChildren(new Option('请选择城市',''));for(const [group,code,list] of CITIES){const box=document.createElement('optgroup');box.label=group;for(const [name,lat,lon,province,cc] of list){const option=new Option(name,JSON.stringify({city:name,latitude:lat,longitude:lon,country:cc||code,province}));option.selected=name===selected;box.append(option);}select.append(box);}}
async function loadMedia(){if(mediaLoaded)return;show('media','正在读取…','loading');try{const prefs=await api('/media/prefs');cityOptions(prefs.city);
 if(prefs.city&&!$('city').value){const option=new Option(prefs.city,JSON.stringify({city:prefs.city,latitude:prefs.latitude,longitude:prefs.longitude,country:prefs.country,province:prefs.province||''}));option.selected=true;$('city').add(option,1);}
 $('save-media').disabled=false;mediaLoaded=true;
 if(prefs.radios!==undefined)show('media','本地和省内电台 '+prefs.radios+' 个。');else $('media-feedback').hidden=true;}
 catch(error){show('media',error.message,'error');}}
$('media-form').onsubmit=async event=>{event.preventDefault();const place=$('city').value?JSON.parse($('city').value):{city:'',latitude:0,longitude:0,country:'',province:''};
 if(!place.city){show('media','请选择所在城市','error');return;}
 $('save-media').disabled=true;show('media','正在保存…','loading');
 try{await api('/media',place);show('media','已保存，收音机立即生效。','success');}
 catch(error){show('media',error.message,'error');}finally{$('save-media').disabled=false;}};
let stocks=[],stocksLoaded=false;
function drawStocks(){$('stock-list').replaceChildren(...stocks.map((p,i)=>podRow(p,p.id,'移除',()=>saveStocks(stocks.filter((_,j)=>j!==i),'已移除「'+p.n+'」'),stocks.length<=1)));}
async function loadStocks(){if(stocksLoaded)return;try{stocks=(await api('/stock/list')).items;stocksLoaded=true;drawStocks();}catch(error){show('stock',error.message,'error');}}
async function saveStocks(next,message){show('stock','正在保存…','loading');try{await api('/stocks',{items:next.map(p=>({id:p.id,n:p.n}))});stocks=next;drawStocks();show('stock',message+'，下次打开「自选股」生效。','success');}catch(error){show('stock',error.message,'error');}}
function addStock(p){if(stocks.some(x=>x.id===p.id)){show('stock','「'+p.n+'」已经添加过了','error');return;}if(stocks.length>=12){show('stock','最多 12 个，请先移除一些','error');return;}saveStocks([...stocks,{id:p.id,n:p.n}],'已添加「'+p.n+'」');}
$('stock-form').onsubmit=async event=>{event.preventDefault();const kw=$('stock-kw').value.trim();if(!kw){show('stock','请输入名称或代码','error');return;}
 $('stock-go').disabled=true;show('stock','正在搜索…','loading');
 try{const result=await api('/stock/search?kw='+encodeURIComponent(kw));
  $('stock-results').replaceChildren(...result.items.map(p=>podRow(p,p.t+' · '+p.c,'添加',()=>addStock(p))));
  if(result.items.length)$('stock-feedback').hidden=true;else show('stock','没有找到，换个关键词试试');}
 catch(error){show('stock',error.message,'error');}finally{$('stock-go').disabled=false;}};
let pods=[],podsLoaded=false;
function podRow(item,note,label,action,disabled){const li=document.createElement('li'),text=document.createElement('div');text.textContent=item.n;if(note){const small=document.createElement('small');small.textContent=note;text.append(small);}
 const button=document.createElement('button');button.type='button';button.textContent=label;button.disabled=Boolean(disabled);button.onclick=action;li.append(text,button);return li;}
function drawPods(){$('pod-list').replaceChildren(...pods.map((p,i)=>podRow(p,'','移除',()=>savePods(pods.filter((_,j)=>j!==i),'已移除「'+p.n+'」'),pods.length<=1)));}
async function loadPods(){if(podsLoaded)return;try{pods=(await api('/podcast/list')).items;podsLoaded=true;drawPods();}catch(error){show('pod',error.message,'error');}}
async function savePods(next,message){show('pod','正在保存…','loading');try{await api('/podcasts',{items:next.map(p=>({id:p.id,n:p.n}))});pods=next;drawPods();show('pod',message+'，下次打开「播客」生效。','success');}catch(error){show('pod',error.message,'error');}}
function addPod(p){if(pods.some(x=>x.id===p.id)){show('pod','「'+p.n+'」已经添加过了','error');return;}if(pods.length>=12){show('pod','最多 12 个节目，请先移除一些','error');return;}savePods([...pods,{id:p.id,n:p.n}],'已添加「'+p.n+'」');}
$('pod-form').onsubmit=async event=>{event.preventDefault();const kw=$('pod-kw').value.trim();if(!kw){show('pod','请输入节目名称','error');return;}
 $('pod-go').disabled=true;show('pod','正在搜索…','loading');
 try{const result=await api('/podcast/search?kw='+encodeURIComponent(kw));
  $('pod-results').replaceChildren(...result.items.map(p=>podRow(p,(p.by?p.by+' · ':'')+p.tracks+' 期'+(p.paid?' · 付费':''),'添加',()=>addPod(p),p.paid)));
  if(result.items.length)$('pod-feedback').hidden=true;else show('pod','没有找到相关节目，换个关键词试试');}
 catch(error){show('pod',error.message,'error');}finally{$('pod-go').disabled=false;}};
async function start(){const phase=await status();if(complete)return;if(phase==='connecting'){busy(true);statusTimer=setInterval(status,1000);}else scan();}start();
</script></body></html>)PORTAL";
}
