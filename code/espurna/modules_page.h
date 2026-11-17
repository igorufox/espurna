/*

MODULES: the head's own page, every chip (modules.cpp arch::page, `/` while there is no webui module, and
/modules.html). Small on purpose: what the board runs, the memory, and the install of the WebUI (html/src/install.mjs
installImage, the same steps): tick, Apply: the head of the install image .ota first when the board runs another head
build (ESP32: /safeboot/switch, the head to the safeboot's /upload; the others: /modules/off, the head to /upgrade),
then /modules/off and /modules/apply with the ticked names and the modules file. The full WebUI is the webui data
module; this page installs it. On top, the setup form (head_setup.cpp, W71): the admin password (required while the
default one is on) and the Wi-Fi network, with a scan, saved with a restart

*/

#pragma once

#include <Arduino.h>

namespace espurna {
namespace modules {

alignas(4) static constexpr char HeadPage[] PROGMEM = R"~(<!doctype html><meta charset=utf-8>
<meta name=viewport content="width=device-width,initial-scale=1"><title>ESPurna modules</title>
<style>body{font:15px system-ui,sans-serif;margin:0 auto;padding:16px;max-width:760px;color:#222;background:#fff}
h2{margin:.2em 0}.sub{color:#666;font-size:13px;margin-bottom:12px}
.bar{display:flex;height:22px;border-radius:4px;overflow:hidden;background:#e8f4e8;margin:4px 0 2px}
.bar i{display:block;height:100%}.h{background:#8090a8}.m{background:#1f8dd6}.d{background:#8cc6ee}
.lg{font-size:13px;color:#555;margin-bottom:12px}.lg b{color:#222}
table{border-collapse:collapse;width:100%}td,th{padding:6px 4px;border-bottom:1px solid #eee;text-align:left;font-size:14px}
.r{text-align:right}.st{font-size:12px;color:#666}.bad{color:#c00}
button{font-size:14px;padding:6px 12px;margin:4px 4px 4px 0}input[type=checkbox]{width:20px;height:20px}
#ap{background:#c00000;color:#fff;border:0;border-radius:4px}#ap:disabled{background:#ccc}
#msg{white-space:pre-wrap;background:#f4f4f4;padding:8px;min-height:1em;font-size:13px}</style>
<h2>Modules</h2><div class=sub id=ver></div>
<form id=su style="display:none;border:1px solid #ccc;padding:8px;margin-bottom:12px"><b>Setup</b>
<div id=pr class=bad>The default password is still on: set a new one first</div>
<p>Admin password <input type=password id=p1 autocomplete=new-password maxlength=63> repeat <input type=password id=p2
autocomplete=new-password maxlength=63></p><p>Wi-Fi <input id=ss maxlength=32 placeholder=SSID> <select id=nets
style="display:none"></select> <button type=button id=sc>Scan</button> password <input type=password id=wp maxlength=64></p>
<button>Save and restart</button> <span id=sm class=st></span></form>
<div id=off style="display:none;background:#fde2e1;border:1px solid #d9534f;padding:8px;margin-bottom:12px">
<b>Modules are off</b>: <span id=why></span><br><button id=on>Start the modules again</button></div><div id=bars></div>
<table><thead><tr><th></th><th>Module</th><th class=r>Flash</th><th class=r>RAM</th><th>State</th></tr></thead>
<tbody id=list></tbody></table>
<p>Install image (.ota): <input type=file id=file accept=.ota,.bin> <button id=gh>From GitHub</button></p>
<p><button id=ap disabled>Apply</button> <button id=rs>Restart</button> <a href="./">WebUI</a></p>
<p class=st>Tick the modules, then Apply: a head of another build in the file goes on first, then the board restarts
without modules, writes the ticked ones from the file and restarts with them. Switching off only needs no file. The full
WebUI is the module webui.</p><div id=msg></div>
<script>
var S,A={},F=null,H=null,T=null,busy=false,$=function(i){return document.getElementById(i)};
function kb(v){return (v/1024).toFixed(1)+' KB'}
function sec(v){return Math.ceil(v/4096)*4096}
function msg(t){$('msg').textContent=t}
function bar(t,total,parts,first,lg){var h='<b>'+t+'</b><div class=bar>';parts.forEach(function(p){if(p[1]>0&&total>0)
h+='<i class='+p[0]+' title="'+p[2]+'" style="width:'+(100*p[1]/total)+'%"></i>'});
return h+'</div><div class=lg><b>'+first+'</b><br>'+lg+'</div>'}
function find(n){return S.modules.filter(function(x){return x.name==n})[0]}
function running(){var o={};S.modules.forEach(function(m){if(m.enabled!==0)o[m.name]=1});return o}
function hn(){return S.image.indexOf('beken-')==0?S.image.slice(6):S.image.split('-')[0]}
function headId(){var k=Object.keys(A)[0],d,s='',i;if(!k)return '';d=A[k].data;if(d.length<40)return '';
for(i=24;i<40;i++)s+=(d[i]<16?'0':'')+d[i].toString(16);return s}
function changes(){var r=running(),out=[];Object.keys(T).forEach(function(n){if(T[n]&&!r[n])out.push('+'+n);
if(!T[n]&&r[n])out.push('-'+n)});return out}
function load(){return fetch('modules',{cache:'no-store'}).then(function(r){return r.json()}).then(function(j){S=j;
if(!T){T={};S.modules.forEach(function(m){T[m.name]=m.enabled!==0})}draw()})}
function draw(){var used=0,names={},h='',dyn=S.ram_dynamic||0,tot=S.ram_used+dyn+S.heap,ch=changes(),code=S.iram_code||0;
S.modules.forEach(function(m){used+=sec(m.size);names[m.name]=1});Object.keys(A).forEach(function(n){names[n]=1});
$('ver').textContent=S.version+', '+S.image;$('off').style.display=S.off?'block':'none';$('why').textContent=S.off;
$('bars').innerHTML=bar('Flash',S.area,[['h',S.head,'head'],['m',used,'modules']],'free '+kb(S.free)+' of '+kb(S.area),
'head '+kb(S.head)+', modules '+kb(used))+bar('RAM',tot,[['m',S.ram_used,'module statics'],['d',dyn,'heap at their start']],
'free '+kb(S.heap)+' of '+kb(tot),'module statics '+kb(S.ram_used)+', heap the modules took when they started '+kb(dyn)+
', free heap after boot '+kb(S.heap_boot))+(S.iram_data?bar(S.pool=='TCM'?'TCM':'IRAM tail',S.iram_data,
[['m',S.iram_data_used,'modules']],'free '+kb(S.iram_data-S.iram_data_used)+' of '+kb(S.iram_data),
(S.pool=='TCM'?'':'module code '+kb(code)+', ')+'module statics '+kb(S.iram_data_used-code)):'');
Object.keys(names).sort().forEach(function(n){var m=find(n),a=A[n],can=!!m||!!a;
h+='<tr><td><input type=checkbox data-n="'+n+'"'+(T[n]?' checked':'')+(can&&!busy?'':' disabled')+'></td><td>'+n+
'</td><td class=r>'+kb(m?m.size:a.image)+'</td><td class=r>'+(m?m.ram:a.ram)+'</td><td class=st>'+
(m?(m.enabled!==0?m.state:'off'):'in the file')+'</td></tr>'});
$('list').innerHTML=h;Array.prototype.forEach.call(document.querySelectorAll('input[data-n]'),function(e){
e.onchange=function(){tick(e.getAttribute('data-n'),e.checked,0);draw()}});
$('ap').disabled=busy||!ch.length;if(ch.length&&!busy)msg(ch.join(', ')+': Apply'+(F&&S.md5&&headId()&&headId()!=S.md5?
(H?' - the head of the file goes on first':' - the file is for another head build and has no head for this board'):''))}
function req(n){var a=A[n],m=find(n);if(a&&a.req)return a.req;return m&&m.requires?m.requires.split(','):[]}
function tick(n,on,d){if(d>4)return;T[n]=on;if(on)req(n).forEach(function(x){if(!T[x])tick(x,true,d+1)});
else Object.keys(T).forEach(function(o){if(T[o]&&req(o).indexOf(n)>=0)tick(o,false,d+1)})}
function post(u,d){return fetch(u,{method:'POST',body:d}).then(function(r){return r.text().then(function(t){
if(!r.ok)throw t||r.status;return t})})}
function back(c){var t0=Date.now();c=c||200;return new Promise(function(ok,no){function poll(){
fetch('modules',{cache:'no-store'}).then(function(r){if(r.status==c)ok();else throw 0}).catch(function(){
if(Date.now()-t0>120000)no('the board did not come back');else setTimeout(poll,2000)})}setTimeout(poll,4000)})}
function apply(){var r=running(),names=Object.keys(T).filter(function(n){return T[n]}),
off=Object.keys(r).filter(function(n){return !T[n]}),add=names.filter(function(n){return !r[n]});
if(add.length&&!F)return msg('adding a module needs the install image (.ota): pick it below');
if(F&&!H&&S.md5&&headId()&&headId()!=S.md5)return msg('the file is for another head build and has no head for this board');
busy=true;draw();var p;
if(!add.length&&off.length&&!F){p=off.reduce(function(q,n){return q.then(function(){var d=new FormData();d.append('name',n);
d.append('on','0');msg(n+': switching off...');return post('modules/enable',d)}).then(function(){var d=new FormData();
d.append('name',n);return post('modules/remove',d)})},Promise.resolve()).then(function(){msg('restarting...');
return post('modules/restart',new FormData())}).then(back)}
else{p=head().then(function(){return post('modules/off',new FormData())}).then(function(){msg('restarting without modules...');return back()})
.then(function(){var d=new FormData();d.append('names',names.join(','));d.append('modules',new Blob([F]),'modules.bin');
msg('writing '+(names.join(', ')||'no modules')+'...');return post('modules/apply',d)}).then(function(t){msg(t);return back()})}
p.then(function(){busy=false;T=null;msg('applied');return load()},function(x){busy=false;msg(''+x);draw()})}
function head(){var n=hn(),sb=n=='esp32',d;if(!H||(F&&S.md5&&headId()==S.md5))return Promise.resolve();
msg('restarting for the new head ('+n+')...');return post(sb?'safeboot/switch':'modules/off',new FormData()).then(function(){
return back(sb?404:200)}).then(function(){if(sb)d=new Blob([H]);else{d=new FormData();d.append('upgrade',new Blob([H]),'espurna-'+n)}
msg('writing the head ('+n+')...');return post(sb?'upload':'upgrade',d)}).then(function(){msg('restarting with the new head...');
return back()})}
function parse(b){var v=new DataView(b),c,i,k,j,nm,nr,ch,o={},z=0,h=null,m=null,n=hn();
if(b.byteLength>8&&v.getUint32(0,true)==0x31544f45){c=v.getUint32(4,true);for(i=0;i<c;i++){k=8+32*i;nm='';
for(j=0;j<16;j++){ch=v.getUint8(k+4+j);if(!ch)break;nm+=String.fromCharCode(ch)}z=v.getUint32(k+20,true);
if(v.getUint32(k,true)==1&&nm==n)h=b.slice(z,z+v.getUint32(k+24,true));
if(v.getUint32(k,true)==2)m=b.slice(z,z+v.getUint32(k+24,true))}if(!m)throw 'the install image has no modules file';
b=m;v=new DataView(b)}
if(b.byteLength<8||v.getUint32(0,true)!=0x32434d45)throw 'not an install image (.ota) nor a modules file';
c=v.getUint32(4,true);for(i=0;i<c;i++){k=8+72*i;nm='';for(j=0;j<16;j++){ch=v.getUint8(k+j);if(!ch)break;
nm+=String.fromCharCode(ch)}nr='';for(j=0;j<32;j++){ch=v.getUint8(k+36+j);if(!ch)break;nr+=String.fromCharCode(ch)}
o[nm]={image:v.getUint32(k+24,true),ram:v.getUint32(k+28,true),req:nr?nr.split(','):[],
data:new Uint8Array(b,v.getUint32(k+16,true),v.getUint32(k+20,true))}}
A=o;F=b;H=h;Object.keys(o).forEach(function(n){if(!(n in T))T[n]=false});msg('modules: '+Object.keys(o).join(', '));draw()}
$('file').onchange=function(){var f=this.files[0];if(f)f.arrayBuffer().then(parse).catch(function(x){msg(''+x)})};
$('gh').onclick=function(){var rev=(S.version.split('-git')[1]||'').slice(0,7);msg('looking for the install image of '+rev+'...');
fetch(S.list||'https://igorufox.github.io/espurna/firmware.json').then(function(r){return r.json()}).then(function(j){
var u=null,m=null;(j.releases||[]).forEach(function(r){if(String(r.commit||'').indexOf(rev)==0)((r.images||{})[S.image]||[])
.forEach(function(f){if(f.kind=='install')u=f.url;if(f.kind=='modules')m=f.url})});u=u||m;if(!u)throw 'no install image of '+rev+
' in the release list, pick the file from the disk';return fetch(u).then(function(r){return r.arrayBuffer()}).then(parse)})
.catch(function(x){msg(''+x)})};
$('ap').onclick=apply;
$('on').onclick=function(){post('modules/on',new FormData()).then(function(t){msg(t);return back()}).then(function(){location.reload()})};
$('rs').onclick=function(){post('modules/restart',new FormData()).then(function(){msg('restarting...');return back()})
.then(function(){location.reload()})};
function sm(t){$('sm').textContent=t}
function gj(u){return fetch(u,{cache:'no-store'}).then(function(r){return r.json()})}
function scan(s){gj('setup/scan'+(s?'?start=1':'')).then(function(j){var e=$('nets');if(j.run)return setTimeout(scan,1500);
e.innerHTML='<option value="">'+j.list.length+' networks</option>';j.list.sort(function(a,b){return b[1]-a[1]}).forEach(function(n){
var o=document.createElement('option');o.value=n[0];o.textContent=n[0]+' '+n[1]+' dBm'+(n[2]?' open':'');e.appendChild(o)});
e.style.display=j.list.length?'':'none';sm(j.error)}).catch(function(x){sm(''+x)})}
$('nets').onchange=function(){if(this.value)$('ss').value=this.value};
$('sc').onclick=function(){sm('scanning...');scan(1)};
$('su').onsubmit=function(e){e.preventDefault();var p=$('p1').value,d=new FormData();if(p||$('p2').value){
if(p!=$('p2').value)return sm('the passwords differ');
if(!/^(?=.*[A-Z\d])(?=.*[a-z])[\w~!@#$%^&*()<>,.?;:{}[\]\\|]{8,63}/.test(p))
return sm('the password: 8..63 characters, a lowercase letter and an uppercase letter or a digit');d.append('pass',p)}
d.append('ssid',$('ss').value);d.append('wpass',$('wp').value);sm('saving...');post('setup',d).then(function(){
sm('saved, the board restarts'+($('ss').value?' and joins '+$('ss').value:''))}).catch(function(x){sm(''+x)})};
gj('setup').then(function(j){$('su').style.display='block';$('pr').style.display=j.pass?'':'none';$('ss').value=j.ssid})
.catch(function(){});
load().catch(function(x){msg(''+x)});
</script>
)~";

} // namespace modules
} // namespace espurna
