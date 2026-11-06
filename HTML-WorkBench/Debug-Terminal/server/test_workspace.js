const assert=require('node:assert/strict'),fs=require('node:fs'),vm=require('node:vm');
const html=fs.readFileSync('server/index.html','utf8'),source=[...html.matchAll(/<script>([\s\S]*?)<\/script>/g)][1][1];
class El{constructor(){this.dataset={};this.textContent='正常';this.attributes={};this.children=[];this.classList={add(){},toggle:(k,v)=>this.attributes[k]=v};}appendChild(e){this.children.push(e);}closest(){return this.row;}querySelector(s){return s==='dt'?this.dt:this.badge||this.dt.children[0];}setAttribute(k,v){this.attributes[k]=v;}removeAttribute(k){delete this.attributes[k];}}
const ids=[...html.matchAll(/id="([^"]+)"/g)].map(x=>x[1]),els=Object.fromEntries(ids.map(id=>[id,new El()]));
for(const id of ['benchCamera','environment','inputs','outputs','servos','displayState','displayTe','screenTouch','audioState','peripheralDiagnostics']){els[id].row=new El();els[id].row.dt=new El();}
const pages=['overview','connect','camera','sound','servo','power','bus'],blocks=pages.map(p=>Object.assign(new El(),{dataset:{workspace:p}})),links=pages.map(p=>Object.assign(new El(),{dataset:{page:p}}));
const events={},location={hash:''},window={addEventListener:(n,f)=>events[n]=f,scrollTo(){}};
const document={getElementById:id=>els[id]||null,createElement:()=>new El(),querySelectorAll:q=>q==='[data-workspace]'?blocks:links,dispatchEvent(){}};
vm.runInNewContext(source,{document,window,location,CustomEvent:class{constructor(n,v){Object.assign(this,v);}}});
for(const page of pages){location.hash='#'+page;events.hashchange();assert.equal(blocks.filter(x=>x.dataset.viewHidden==='false').length,1);assert.equal(links.find(x=>x.attributes['aria-current']==='page').dataset.page,page);}
location.hash='#i2cScanner';events.hashchange();assert.equal(links[6].attributes['aria-current'],'page');
const data={connection:{active:'serial'},esp_peripherals:{audio:{state:'INIT_ERROR',error:259},touch:{state:'NOT_FOUND',error:261},display:{configured:true,te_signal_present:true}}};
window.robotUiUpdate(data,true,true,true);assert.equal(els.audioState.row.dataset.health,'error');assert.equal(els.screenTouch.row.dataset.health,'error');assert.equal(els.displayState.row.dataset.health,'warning');
window.robotUiUpdate(data,true,true,false);assert.equal(els.audioState.row.dataset.health,'unknown');
data.esp_peripherals.audio={state:'SAMPLING',valid:true,error:0};window.robotUiUpdate(data,true,true,true);assert.equal(els.audioState.row.dataset.health,'success');
els.speakerTestStatus.textContent='执行失败，错误 259';window.robotUiUpdate(data,true,true,true);assert.equal(els.speakerTestStatus.dataset.health,'error');
window.robotUiUpdate(data,false,false,false);assert.equal(els.audioState.row.dataset.health,'unknown');assert.equal(els.speakerTestStatus.dataset.health,'unknown');
console.log('PASS workspace: seven isolated views, legacy deep links, error/recovery/stale status, unverified display and offline controls');
