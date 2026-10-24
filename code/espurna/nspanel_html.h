/*

NSPANEL MODULE - /nspanel editor page

Self-contained, no external assets. Edits /nspanel.json and previews the grid cards

*/

#pragma once

namespace espurna {
namespace nspanel {
namespace web {

static const char EditorPage[] = R"html(<!doctype html>
<html><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>NSPanel config</title>
<style>
body{font-family:sans-serif;margin:0;padding:12px;background:#111;color:#ddd}
h1{font-size:18px;margin:0 0 8px}
.row{display:flex;flex-wrap:wrap;gap:12px}
.col{flex:1 1 420px;min-width:0}
textarea{width:100%;height:70vh;box-sizing:border-box;font:12px monospace;background:#1b1b1b;color:#eee;border:1px solid #444}
button,select{background:#333;color:#eee;border:1px solid #555;padding:6px 12px;margin:4px 4px 4px 0;cursor:pointer}
#st{font-size:13px;margin:4px 0;white-space:pre-wrap}
.ok{color:#7c7}.err{color:#f77}
.card{background:#181c18;border:1px solid #333;border-radius:6px;margin:0 0 12px;padding:8px;max-width:480px}
.hd{display:flex;justify-content:space-between;font-size:14px;margin-bottom:6px}
.grid{display:grid;gap:6px}
.tile{background:#222;border-radius:6px;padding:6px;text-align:center;font-size:12px;min-height:54px;overflow:hidden}
.ic{color:#fd8;font-size:10px;word-break:break-all}.nm{margin-top:4px}.ty{color:#8ab;font-size:10px}
.bad{outline:1px solid #f55}
</style></head><body>
<h1>NSPanel</h1>
<div class="row"><div class="col">
<button onclick="load()">Reload</button><button onclick="fmt()">Format</button><button onclick="save()">Save</button>
Grid: <select id="cap" onchange="prev()"><option value="8">8 (TFT &ge; 51)</option><option value="6">6</option></select>
<div id="st"></div>
<textarea id="ta" spellcheck="false" oninput="prev()"></textarea>
</div><div class="col" id="pv"></div></div>
<script>
var ta=document.getElementById('ta'),st=document.getElementById('st'),pv=document.getElementById('pv');
function msg(t,c){st.textContent=t;st.className=c||''}
function esc(s){return String(s==null?'':s).replace(/[&<>"]/g,function(c){return{'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;'}[c]})}
function load(){fetch('nspanel.json',{credentials:'same-origin'}).then(function(r){if(!r.ok)throw r.status;return r.text()})
.then(function(t){ta.value=t;msg('Loaded','ok');prev()}).catch(function(e){msg('Load failed: '+e,'err')})}
function fmt(){try{ta.value=JSON.stringify(JSON.parse(ta.value),null,2);prev()}catch(e){msg(e.message,'err')}}
function save(){var j;try{j=JSON.parse(ta.value)}catch(e){msg('Not saved: '+e.message,'err');return}
fetch('nspanel.json',{method:'POST',credentials:'same-origin',headers:{'Content-Type':'application/json'},body:ta.value})
.then(function(r){return r.text().then(function(t){if(!r.ok)throw t||r.status;msg('Saved, panel reloaded','ok')})})
.catch(function(e){msg('Save failed: '+e,'err')})}
function prev(){var c,cap=+document.getElementById('cap').value,h='';
try{c=JSON.parse(ta.value)}catch(e){pv.innerHTML='';msg(e.message,'err');return}
msg('JSON ok','ok');var groups=c.groups||{},cards=[];
(c.pages||[]).forEach(function(p){var it=p.items||[],n=Math.max(1,Math.ceil(it.length/cap));
for(var k=0;k<n;k++)cards.push({t:(p.title||'')+(n>1?' '+(k+1)+'/'+n:''),i:it.slice(k*cap,k*cap+cap)})});
cards.forEach(function(cd,ci){var cols=cd.i.length>6?4:3;
h+='<div class="card"><div class="hd"><span>'+(cards.length>1?'&#9664;':'')+'</span><b>'+esc(cd.t)+'</b><span>'+(cards.length>1?'&#9654;':'')+'</span></div>';
h+='<div class="grid" style="grid-template-columns:repeat('+cols+',1fr)">';
cd.i.forEach(function(it){var ty=it.type||'switch',bad='',tgt='';
if(ty=='scene'){tgt='group '+it.group+' '+(it.action||'toggle');if(!groups[it.group])bad=' bad'}
else if(it.relay!=null)tgt='relay '+it.relay;else{tgt=it.topic||'';if(!tgt)bad=' bad'}
h+='<div class="tile'+bad+'" title="'+esc(tgt)+'"><div class="ic">'+esc(it.icon||'')+'</div><div class="nm">'+esc(it.name)+'</div><div class="ty">'+esc(ty)+'</div></div>'});
h+='</div></div>'});pv.innerHTML=h}
load();
</script></body></html>
)html";

} // namespace web
} // namespace nspanel
} // namespace espurna
