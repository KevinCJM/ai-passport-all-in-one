/* Shared, offline menu editor: served by the badge and by the Mac assistant. */
(function (scope) {
  'use strict';
  const APPS = 64, PAGES = 8, NAME = 25, BYTES = 481;
  const clone = value => JSON.parse(JSON.stringify(value));
  function decode(encoded) {
    const b = Uint8Array.from(atob(encoded), c => c.charCodeAt(0));
    if (b.length !== BYTES || b[0] !== 1) throw Error('配置版本不兼容，请更新固件和小助手');
    const d = {mode:b[1], count:b[2], pageCount:b[3], hidden:[], categories:[...b.slice(12,17)], orders:[], assignment:[...b.slice(209,273)], pages:[...b.slice(273,281)], names:[]};
    for (let id=0; id<APPS; id++) if (b[4+(id>>3)] & (1<<(id%8))) d.hidden.push(id);
    for (let m=0; m<3; m++) d.orders.push([...b.slice(17+m*APPS,17+m*APPS+d.count)]);
    const utf8 = new TextDecoder('utf-8', {fatal:true});
    for (let p=0; p<PAGES; p++) {const name=b.slice(281+p*NAME,281+(p+1)*NAME);const end=name.indexOf(0);d.names.push(utf8.decode(name.slice(0,end<0?NAME:end)));}
    return d;
  }
  function encode(d) {
    const b = new Uint8Array(BYTES);b[0]=1;b[1]=d.mode;b[2]=d.count;b[3]=d.pageCount;
    for (const id of d.hidden) b[4+(id>>3)] |= 1<<(id%8);
    b.set(d.categories,12);
    for (let m=0;m<3;m++){b.fill(255,17+m*APPS,17+(m+1)*APPS);b.set(d.orders[m],17+m*APPS);}
    b.set(d.assignment,209);b.set(d.pages,273);
    for(let p=0;p<d.pageCount;p++){
      const text=d.names[p], raw=new TextEncoder().encode(text);
      if(!text.trim()||[...text].length>8||raw.length>=NAME||/[\x00-\x1f\x7f]/.test(text))throw Error('页面名称请用1～8个字');
      b.set(raw,281+p*NAME);
    }
    return btoa(String.fromCharCode(...b));
  }
  function defaults(catalog) {
    const ids=catalog.flatMap(g=>g.apps.map(a=>a.id));
    const d={mode:0,count:ids.length,pageCount:5,hidden:[],categories:[0,1,2,3,4],orders:[ids.slice(),ids.slice(),ids.slice()],assignment:Array(APPS).fill(0),pages:[0,1,2,3,4,5,6,7],names:Array(PAGES).fill('')};
    catalog.forEach((g,p)=>{d.names[p]=g.name.replace(/\s*·\s*/g,'').slice(0,8);g.apps.forEach(a=>d.assignment[a.id]=p);});
    return d;
  }
  function mount(root, prefix='') {
    if(root.dataset.menuMounted)return;root.dataset.menuMounted='1';
    if(!document.querySelector('#passport-menu-style')){
      const css=document.createElement('style');css.id='passport-menu-style';css.textContent=`
      .pm{max-width:900px}.pm button,.pm input,.pm select{font:inherit;box-sizing:border-box}.pm button{min-height:36px;padding:6px 10px;margin:2px;border-radius:9px;border:1px solid #49606c;background:#20343d;color:#e4ecee;cursor:pointer}.pm button:disabled{opacity:.45;cursor:default}.pm select,.pm input[type=text]{background:#111c22;color:#e4ecee;border:1px solid #49606c;border-radius:8px;padding:9px;min-height:40px;max-width:100%}.pm select{width:auto}.pm input[type=checkbox]{width:19px;min-height:19px;height:19px;flex:none;accent-color:#91e0b0}.pm input[type=text]{width:100%}.pm fieldset{border:0;padding:0;margin:0;min-width:0}.pm small{color:#94a8b0}.pm-toolbar{display:flex;gap:6px;align-items:center;flex-wrap:wrap;margin:10px 0}.pm-choice{display:flex;align-items:center;gap:8px;margin:0;padding:7px 3px}.pm-picks{max-height:280px;overflow:auto;display:grid;grid-template-columns:repeat(2,minmax(0,1fr));margin:8px 0}.pm-group{background:#1c2a32;border:1px solid #39505b;border-radius:12px;padding:12px;margin:12px 0}.pm-group header{display:flex;align-items:center;gap:5px;margin:0 0 8px;flex-wrap:wrap}.pm-group header input{max-width:180px}.pm-item{display:flex;gap:5px;align-items:center;flex-wrap:wrap;padding:7px 0;border-top:1px solid #39505b}.pm-item span{flex:1;min-width:90px}.pm-item select{max-width:135px}.pm-drag{cursor:grab;color:#94a8b0}.pm-preview{width:240px;box-sizing:border-box;background:#021018;border:2px solid #39505b;border-radius:14px;padding:10px;color:#eaf4ff}.pm-grid{display:grid;grid-template-columns:repeat(3,1fr);gap:5px;min-height:218px;align-content:start}.pm-tile{height:68px;border-radius:10px;background:#0a2230;text-align:center;display:flex;flex-direction:column;justify-content:center;gap:5px;font-size:13px}.pm-tile b{color:#4bffc4;font-size:22px}.pm-result{white-space:pre-wrap;margin:10px 0;min-height:24px}.pm-save{background:#91e0b0!important;color:#10231a!important;font-weight:650}.pm button:focus-visible,.pm input:focus-visible,.pm select:focus-visible{outline:2px solid #91e0b0;outline-offset:2px}.pm .pm-muted{color:#94a8b0;font-size:13px}.pm details{margin:14px 0}.pm-list{display:grid;gap:4px;min-height:218px;align-content:start}.pm-row{height:27px;border-radius:7px;background:#0a2230;display:flex;align-items:center;gap:8px;padding:0 8px;font-size:13px}.pm-row b{color:#4bffc4}.pm-mode{margin:-4px 0 8px}`;
      document.head.append(css);
    }
    root.classList.add('pm');
    const el=(tag,text)=>{const e=document.createElement(tag);if(text!==undefined)e.textContent=text;return e;};
    const toolbar=el('div');toolbar.className='pm-toolbar';const reload=el('button','重新读取胸牌');reload.type='button';toolbar.append(reload);
    const status=el('div');status.className='pm-result';status.setAttribute('role','status');
    const form=el('fieldset');form.disabled=true;const controls=el('div'),picks=el('div'),arrangement=el('div'),preview=el('div'),actions=el('div');actions.className='pm-toolbar';
    form.append(controls,picks,arrangement,preview,actions);root.append(toolbar,status,form);
    let catalog=[],apps=[],byId=new Map(),draft=null,revision=0,busy=false,dirty=false,previewPage=0;
    const api=async(path,body)=>{const response=await fetch(prefix+path,{method:body===undefined?'GET':'POST',headers:body===undefined?{}:{'Content-Type':'application/json'},body:body===undefined?undefined:JSON.stringify(body),cache:'no-store'});const text=await response.text();let data;try{data=JSON.parse(text);}catch(_){throw Error('设备未返回配置，请重新连接后读取');}if(!response.ok)throw Error(data.error||'设备拒绝了配置（'+response.status+'）');return data;};
    const shown=id=>!draft.hidden.includes(id);
    // Modes 1 (nine a screen) and 3 (compact list) have no pages and share one order.
    const flat=()=>draft.mode===1||draft.mode===3,order=()=>draft.orders[draft.mode===3?1:draft.mode];
    const category=id=>byId.get(id).group;
    const list=(group,includeHidden=false)=>order().filter(id=>(includeHidden||shown(id))&&(flat()|| (draft.mode===0?category(id):draft.assignment[id])===group));
    const groupIds=()=>draft.mode===0?draft.categories.slice():flat()?[0]:draft.pages.slice(0,draft.pageCount);
    const title=g=>draft.mode===0?catalog[g].name:flat()?'全部功能':draft.names[g];
    function change(fn,redraw=true){const before=clone(draft);try{fn();encode(draft);dirty=true;status.textContent='有未保存的修改';if(redraw)render();}catch(e){draft=before;status.textContent=e.message;if(redraw)render();}}
    function customCount(group,except=-1){return draft.orders[2].filter(id=>id!==except&&shown(id)&&draft.assignment[id]===group).length;}
    function enable(id,yes){
      if(!yes){if(id===10)throw Error('系统设置必须保留');draft.hidden=[...new Set([...draft.hidden,id])];return;}
      let group=draft.assignment[id];
      if(customCount(group,id)>=9){
        group=draft.pages.slice(0,draft.pageCount).find(g=>customCount(g,id)<9);
        if(group===undefined){
          if(draft.pageCount>=PAGES)throw Error('自定义页面已满，请先调整页面');
          group=draft.pageCount++;draft.pages[group]=group;draft.names[group]='新增功能'+(group+1);
        }
        draft.assignment[id]=group;
      }
      draft.hidden=draft.hidden.filter(x=>x!==id);
    }
    function move(id,group,before){
      if(before===id)return;
      if(draft.mode===2){if(customCount(group,id)>=9)throw Error('本页已有9项，请选择其他页面');draft.assignment[id]=group;}
      else if(draft.mode===0&&category(id)!==group)throw Error('跨分类移动请使用“自定义分页”');
      const ids=order();ids.splice(ids.indexOf(id),1);
      const at=before===undefined?-1:ids.indexOf(before);ids.splice(at<0?ids.length:at,0,id);
    }
    function shiftPage(group,delta){const order=draft.mode===0?draft.categories:draft.pages;const i=order.indexOf(group),j=i+delta,limit=draft.mode===0?5:draft.pageCount;if(j<0||j>=limit)return;[order[i],order[j]]=[order[j],order[i]];}
    function drop(event,group,before){event.preventDefault();try{const payload=JSON.parse(event.dataTransfer.getData('text/plain'));change(()=>{if(payload.kind==='app')move(payload.id,group,before);else if(payload.kind==='page'&&!flat()){const order=draft.mode===0?draft.categories:draft.pages;const i=order.indexOf(payload.id),j=order.indexOf(group);order.splice(i,1);order.splice(j,0,payload.id);}});}catch(_){status.textContent='无法移动，请使用上移／下移按钮';}}
    function button(text,fn,parent,disabled=false){const b=el('button',text);b.type='button';b.disabled=disabled;b.onclick=fn;parent.append(b);return b;}
    function renderPreview(){
      preview.replaceChildren(el('h4','排列预览'));
      const screens=[],per=draft.mode===3?7:9;
      if(flat()){const ids=list(0);for(let i=0;i<ids.length;i+=per)screens.push({title:'全部功能',ids:ids.slice(i,i+per),first:i,total:ids.length});}
      else for(const g of groupIds()){const ids=list(g);if(ids.length)screens.push({title:title(g),ids});}
      previewPage=Math.max(0,Math.min(previewPage,screens.length-1));const shown=screens[previewPage];
      const nav=el('div');nav.className='pm-toolbar';
      button('上一屏',()=>{previewPage=Math.max(0,previewPage-1);renderPreview();},nav,previewPage===0);
      button('下一屏',()=>{previewPage=Math.min(screens.length-1,previewPage+1);renderPreview();},nav,previewPage>=screens.length-1);
      const screen=el('div');screen.className='pm-preview';
      const heading=el('div',shown.title+' · '+(draft.mode===3?(shown.first+1)+'/'+shown.total:(previewPage+1)+'/'+screens.length));heading.style.marginBottom='10px';screen.append(heading);
      const body=el('div');body.className=draft.mode===3?'pm-list':'pm-grid';
      for(const id of shown.ids){const cell=el('div');cell.className=draft.mode===3?'pm-row':'pm-tile';cell.append(el('b','◇'),el('span',byId.get(id).name));body.append(cell);}
      screen.append(body);preview.append(nav,screen,el('small','预览展示排列；图标以胸牌实际显示为准。'));
    }
    function render(){
      controls.replaceChildren();const top=el('div');top.className='pm-toolbar';top.append(el('strong','显示方式'));
      const modes=el('select');modes.setAttribute('aria-label','菜单显示方式');[[0,'原有分类'],[2,'自定义分页'],[1,'不分页 · 九宫格'],[3,'不分页 · 紧凑列表']].forEach(([i,name])=>{const o=el('option',name);o.value=i;modes.append(o);});modes.value=draft.mode;modes.onchange=()=>change(()=>{draft.mode=Number(modes.value);previewPage=0;});top.append(modes,el('small','已显示 '+apps.filter(a=>shown(a.id)).length+' / '+apps.length+' 项'));
      const about=el('small',['按原来的 5 个分类分页，可调整分类顺序和分类里的顺序。','不分页：选中的功能按顺序排，每屏 9 个九宫格，长按上下翻屏。','自己建页、起页名（1～8 个字），用每项右边的下拉框决定放在哪页；每页最多 9 个，最多 8 页。','不分页：每屏 7 行的紧凑列表（图标＋名称），长按上下快移 7 项。'][draft.mode]);about.className='pm-mode';controls.append(top,about);
      const wasOpen=picks.querySelector('details')?.open??true;picks.replaceChildren();const details=el('details');details.open=wasOpen;details.append(el('summary','选择要显示的功能'));
      const search=el('input');search.type='text';search.placeholder='搜索功能';search.setAttribute('aria-label','搜索功能');const choices=el('div');choices.className='pm-picks';
      function drawChoices(){choices.replaceChildren();for(const a of apps.filter(a=>a.name.includes(search.value.trim()))){const label=el('label');label.className='pm-choice';const check=el('input');check.type='checkbox';check.checked=shown(a.id);check.disabled=a.id===10;check.onchange=()=>{change(()=>enable(a.id,check.checked));};label.append(check,el('span',a.name+(a.id===10?'（保留）':'')));choices.append(label);}}
      search.oninput=drawChoices;const bulk=el('div');bulk.className='pm-toolbar';button('全部显示',()=>change(()=>{for(const a of apps)enable(a.id,true);}),bulk);button('只留系统设置',()=>change(()=>{for(const a of apps)enable(a.id,a.id===10);}),bulk);details.append(search,bulk,choices);drawChoices();picks.append(details);
      arrangement.replaceChildren(el('p','拖动排序，也可使用上移／下移；设置保留入口，隐藏功能仍可语音打开。'),el('small','恢复显示时，自定义页满后会自动补充新页。'));
      groupIds().forEach((group,pageIndex)=>{
        const section=el('section');section.className='pm-group';const header=el('header');
        if(draft.mode===2){const name=el('input');name.type='text';name.value=draft.names[group];name.maxLength=8;name.setAttribute('aria-label','页面名称');name.onchange=()=>change(()=>draft.names[group]=name.value.trim());header.append(name);}else header.append(el('strong',title(group)));
        if(!flat()){const handle=el('span','⋮⋮');handle.className='pm-drag';handle.draggable=true;handle.title='拖动页面';handle.ondragstart=e=>e.dataTransfer.setData('text/plain',JSON.stringify({kind:'page',id:group}));header.prepend(handle);button('页↑',()=>change(()=>shiftPage(group,-1)),header,pageIndex===0);button('页↓',()=>change(()=>shiftPage(group,1)),header,pageIndex===groupIds().length-1);}
        if(draft.mode===2)button('删除页',()=>change(()=>{if(draft.pageCount===1)throw Error('至少保留一页');if(draft.assignment[10]===group)throw Error('请先将系统设置移动到其他页面');for(const id of draft.orders[2]){if(draft.assignment[id]===group){draft.hidden=[...new Set([...draft.hidden,id])];draft.assignment[id]=0;}else if(draft.assignment[id]>group)--draft.assignment[id];}draft.names.splice(group,1);draft.names.push('');draft.pages=draft.pages.slice(0,draft.pageCount).filter(p=>p!==group).map(p=>p>group?p-1:p);--draft.pageCount;while(draft.pages.length<PAGES)draft.pages.push(draft.pages.length);}),header);
        section.append(header);const ids=list(group);section.ondragover=e=>e.preventDefault();section.ondrop=e=>drop(e,group);
        if(!ids.length)section.append(el('small','此页为空，胸牌会自动跳过。'));
        ids.forEach((id,index)=>{const row=el('div');row.className='pm-item';row.draggable=true;row.ondragstart=e=>{e.stopPropagation();e.dataTransfer.setData('text/plain',JSON.stringify({kind:'app',id}));};row.ondragover=e=>e.preventDefault();row.ondrop=e=>{e.stopPropagation();drop(e,group,id);};row.append(el('span','⋮⋮ '+byId.get(id).name));button('↑',()=>change(()=>move(id,group,ids[index-1])),row,index===0);button('↓',()=>change(()=>{const all=order(),a=all.indexOf(id),b=all.indexOf(ids[index+1]);[all[a],all[b]]=[all[b],all[a]];}),row,index===ids.length-1);
          if(draft.mode===2){const target=el('select');target.setAttribute('aria-label',byId.get(id).name+'所在页面');draft.pages.slice(0,draft.pageCount).forEach(p=>{const option=el('option',draft.names[p]);option.value=p;target.append(option);});target.value=group;target.onchange=()=>change(()=>move(id,Number(target.value)));row.append(target);}
          button('隐藏',()=>change(()=>enable(id,false)),row,id===10);section.append(row);
        });arrangement.append(section);
      });
      if(draft.mode===2)button('＋ 新增页面',()=>change(()=>{if(draft.pageCount>=PAGES)throw Error('最多8页');const p=draft.pageCount++;draft.pages[p]=p;draft.names[p]='新页面';}),arrangement,draft.pageCount>=PAGES);
      renderPreview();actions.replaceChildren();button('恢复默认',()=>change(()=>{draft=defaults(catalog);previewPage=0;}),actions);
      const save=button('保存到胸牌',saveDraft,actions);save.className='pm-save';
    }
    async function read(){if(busy)return;busy=true;reload.disabled=true;form.disabled=true;status.textContent='正在读取胸牌…';try{const state=await api('/menu');const groups=[];for(let g=0;g<5;g++)groups.push(await api('/menu/catalog?group='+g));catalog=groups;apps=catalog.flatMap((g,group)=>g.apps.map(a=>({...a,group})));byId=new Map(apps.map(a=>[a.id,a]));const next=decode(state.layout);if(next.count!==apps.length)throw Error('功能目录与配置不一致，请重新读取');draft=next;revision=state.revision;dirty=false;previewPage=0;render();form.disabled=false;status.textContent='已读取胸牌当前配置';}catch(e){status.textContent='读取失败：'+e.message;}finally{busy=false;reload.disabled=false;}}
    async function saveDraft(){if(busy||!draft)return;busy=true;form.disabled=true;reload.disabled=true;status.textContent='正在保存到胸牌…';try{const layout=encode(draft),result=await api('/menu',{layout,revision});const until=Date.now()+20000;let saved=false;while(Date.now()<until){await new Promise(resolve=>setTimeout(resolve,300));const state=await api('/menu');if(state.done===result.ticket){if(state.result)throw Error('胸牌写入失败，原配置仍然保留');if(state.layout!==layout)throw Error('配置已被其他页面修改，请重新读取');revision=state.revision;saved=true;break;}if(!state.pending&&state.revision!==revision)throw Error('配置已变化，请重新读取');}if(!saved)throw Error('保存确认超时，请重新读取胸牌核对');dirty=false;status.textContent='已保存到胸牌，菜单已更新';}catch(e){status.textContent='保存失败：'+e.message;}finally{busy=false;form.disabled=false;reload.disabled=false;}}
    reload.onclick=read;scope.addEventListener('beforeunload',event=>{if(dirty){event.preventDefault();event.returnValue='';}});read();
  }
  scope.PassportMenu={mount,decode,encode,defaults};
})(typeof window==='undefined'?globalThis:window);
