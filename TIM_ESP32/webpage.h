// The web UI, served from flash. One page, plain JavaScript, no internet needed.
#pragma once

static const char kIndexHtml[] PROGMEM = R"HTML(<!doctype html>
<html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>TIM</title>
<style>
:root{--bg:#f4f5f7;--card:#fff;--ink:#1d2430;--mute:#6a7383;--line:#dfe3ea;--acc:#1f6feb;--ok:#1a7f37;--warn:#b35900;--bad:#c62828}
@media (prefers-color-scheme:dark){:root{--bg:#12161c;--card:#1b2129;--ink:#e6e9ee;--mute:#8d96a5;--line:#2c3440;--acc:#4c8dff;--ok:#3fb950;--warn:#e3a33b;--bad:#f0605d}}
*{box-sizing:border-box}body{margin:0;background:var(--bg);color:var(--ink);font:15px/1.4 system-ui,sans-serif}
header{position:sticky;top:0;z-index:2;background:var(--card);border-bottom:1px solid var(--line);padding:8px 12px;display:flex;flex-wrap:wrap;gap:6px 12px;align-items:center}
header b{font-size:17px}#eng{font-size:13px;padding:2px 8px;border-radius:10px;background:var(--line)}
nav{display:flex;gap:4px;overflow-x:auto;width:100%}nav button{flex:0 0 auto}
button{font:inherit;padding:6px 12px;border:1px solid var(--line);border-radius:8px;background:var(--card);color:var(--ink);cursor:pointer}
button.on{background:var(--acc);border-color:var(--acc);color:#fff}button.pri{background:var(--acc);color:#fff;border-color:var(--acc)}
main{padding:12px;max-width:1100px;margin:auto}section{display:none}section.on{display:block}
.grid{display:grid;grid-template-columns:repeat(auto-fill,minmax(160px,1fr));gap:10px}
.tile{background:var(--card);border:1px solid var(--line);border-radius:12px;padding:10px 12px}
.tile .n{font-size:12px;color:var(--mute)}.tile .v{font-size:28px;font-weight:600;font-variant-numeric:tabular-nums}
.tile .u{font-size:14px;color:var(--mute);margin-left:4px}.tile .s{font-size:12px}.tile .mm{font-size:12px;color:var(--mute);font-variant-numeric:tabular-nums}
.card{background:var(--card);border:1px solid var(--line);border-radius:12px;padding:12px;margin-bottom:12px}
h3{margin:0 0 8px;font-size:15px}table{border-collapse:collapse;width:100%;font-variant-numeric:tabular-nums}
td,th{padding:4px 6px;border-bottom:1px solid var(--line);text-align:left;font-size:13px}th{color:var(--mute);font-weight:500}
.scroll{overflow-x:auto}.ok{color:var(--ok)}.warn{color:var(--warn)}.bad{color:var(--bad)}.mute{color:var(--mute)}
input,select{font:inherit;padding:5px 8px;border:1px solid var(--line);border-radius:6px;background:var(--bg);color:var(--ink);max-width:100%}
label{display:block;margin:6px 0}.row{display:flex;flex-wrap:wrap;gap:8px;align-items:center}
#msg{position:fixed;left:50%;bottom:16px;transform:translateX(-50%);background:var(--ink);color:var(--bg);padding:8px 14px;border-radius:8px;display:none;max-width:92vw}
.sel li{list-style:none;padding:4px 0;border-bottom:1px solid var(--line);display:flex;gap:6px;align-items:center}.sel{padding:0;margin:0}
.sel li span{flex:1}code{font-size:12px}
</style></head><body>
<header><b>TIM</b><span id="eng">engine ?</span><span class="mute" id="hdr"></span>
<nav id="nav"><button data-s="dash" class="on">Dashboard</button><button data-s="sig">All signals</button><button data-s="j">J1587</button><button data-s="cal">Calibrate</button><button data-s="mod">Modules</button><button data-s="set">Settings</button></nav></header>
<main>
<section id="dash" class="on"><div class="row" style="margin-bottom:10px"><button onclick="post('/api/minmax/reset')">Clear min/max</button><span class="mute">Min/max since restart or the last clear; @ = uptime second it happened (now <span id="upnow">?</span> s). Not saved.</span></div><div class="grid" id="tiles"></div><p class="mute" id="dashnote"></p></section>

<section id="sig">
<div class="card"><h3>Engineering values</h3><div class="scroll"><table id="engtab"></table></div></div>
<div class="card"><h3>ADC channels <button onclick="post('/api/minmax/reset')">Reset min/max</button></h3>
<p class="mute" id="adcsum"></p><div class="scroll"><table id="rawtab"></table></div>
<p class="mute">"Input" is the value at the C55 pin before calibration: FE-A volts, FE-B sensor volts, or battery amps (+ = discharge). "Ratio" is sensor volts / sensor supply.</p></div>
</section>

<section id="j">
<div class="card"><h3>J1708 bus (listen only, this board never transmits)</h3><p id="jst"></p>
<div class="row"><a href="/j1587.txt"><button>Download capture</button></a><button onclick="post('/api/j1587/clear')">Clear</button></div></div>
<div class="card"><h3>Trouble codes (broadcast by each module on PID 194)</h3><div class="scroll"><table id="dtab"></table></div>
<p class="mute">Codes the modules announce on their own. PID = a measured parameter, SID = a component the module defines (look the number up in that module's manual). A code stays listed until Clear or a restart; "last sent" shows whether the module is still announcing it. Reading stored history or clearing codes needs the TIM to transmit, which this firmware doesn't do.</p></div>
<div class="card"><h3>Parameters heard</h3><div class="scroll"><table id="jtab"></table></div>
<p class="mute">Scalings are from SAE J1587; check them against a known gauge before trusting them.</p></div>
</section>

<section id="cal">
<div class="card"><h3>Zero (do it with the engine off for a true zero)</h3><p id="calst"></p>
<div class="row"><button class="pri" onclick="zero('all')">Zero all</button><button onclick="zero('shunt')">Shunt</button><button onclick="zero('c59')">C59 baro</button><button onclick="zero('oil_in')">Oil inlet</button><button onclick="zero('oil_out')">Oil outlet</button><button onclick="zero('air')">Air cleaner</button><button onclick="post('/api/cal/undo')">Undo last change</button></div>
<div class="scroll"><table id="zerotab"></table></div></div>
<div class="card"><h3>Voltage trim against a meter</h3><p class="mute">Measure the wire with a meter (5 V or more), type the reading, press Set.</p><div class="scroll"><table id="vtab"></table></div></div>
<div class="card"><h3>Sensor curves</h3><p class="mute">Pressure = low + (ratio - ratio low) / (ratio high - ratio low) x (high - low). The usual 0.5-4.5 V sensor is ratio 0.10-0.90. C59 (0-30 psia, FLIS) and C63 (0-15 psia, Sensata 67CP 0015A label) are from part data; the oil curves are placeholders until that sensor's range is known.</p><div class="scroll"><table id="ctab"></table></div></div>
<div class="card"><h3>Battery current direction</h3><p>The shunt carries all battery current: cranking should read positive (discharge) and charging negative. If cranking reads negative, the sense wires are reversed. <button onclick="sign()">Flip sign</button> <span id="signst"></span></p></div>
</section>

<section id="mod"><div class="card"><h3>ESP-NOW (channel 1)</h3><p id="nowst"></p><div class="scroll"><table id="nowtab"></table></div>
<p class="mute">The TIM broadcasts "tim_v" and "tim_p" packets every second and answers STATUS, GET, PING and HELP.</p></div></section>

<section id="set">
<div class="card"><h3>Dashboard sensors</h3><p class="mute">Tick what shows on the Dashboard; arrows change the order. Every J1587 value appears here once it's been heard on the bus; ones without a known scaling show as a raw number.</p>
<ul class="sel" id="sellist"></ul><p><button class="pri" onclick="saveDash()">Save dashboard</button></p></div>
<div class="card"><h3>Firmware update</h3><p>Running <b id="fw"></b>. Choose the <code>.bin</code> file from Arduino (Sketch, Export Compiled Binary) and press Upload. The board restarts; if the new firmware fails in its first 30 s, it goes back to this one. If the file picker does nothing in the pop-up sign-in window, open <b>http://192.168.4.1</b> in Chrome instead.</p>
<div class="row"><input type="file" id="bin" accept=".bin"><button class="pri" onclick="upload()">Upload</button><span id="upst"></span></div></div>
<div class="card"><h3>Wi-Fi</h3><p class="mute" id="wst"></p>
<label>Hotspot name <input id="ap_ssid"></label><label>Hotspot password (8+ characters, blank = keep) <input id="ap_pass" type="password"></label>
<label><input type="checkbox" id="sta_en"> Also join shop Wi-Fi (2.4 GHz only). While joined, the hotspot and ESP-NOW move to the router's channel, so the BMS link only works if that is channel 1. If it can't join within 20 s it gives up and puts the hotspot back on channel 1. It tries again every 30 s, or every 5 min while someone is on the hotspot.</label>
<label>Shop Wi-Fi name, exactly as it appears (capitals matter; Scan and "Use" fills it in) <input id="sta_ssid"></label>
<label><input type="checkbox" id="sta_ch1"> Channel 1 only: join only an access point on channel 1, so the hotspot and the BMS link stay on channel 1</label><label>Shop Wi-Fi password (blank = keep) <input id="sta_pass" type="password"></label>
<div class="row"><button class="pri" onclick="saveWifi()">Save and restart</button><button onclick="post('/api/wifi/retry')">Try shop Wi-Fi now</button><button onclick="scan()">Scan for networks</button></div>
<div class="scroll"><table id="scantab"></table></div></div>
<div class="card"><h3>Device</h3><div class="row"><button onclick="if(confirm('Restart the TIM?'))post('/api/reboot')">Restart</button>
<button onclick="if(confirm('Erase calibration, Wi-Fi and dashboard settings?'))post('/api/factory',{confirm:'yes'})">Factory reset</button></div>
<p class="mute">Settings, Calibrate and Update ask for a login: user <b>admin</b>, password = the hotspot password.</p></div>
</section>
</main><div id="msg"></div>
<script>
const $=id=>document.getElementById(id);let meta=null,live=null,cur='dash',timer=null,jsel=[];
const ST=['ok','no ADC','open / unplugged','sensor supply fault','out of range'];
function msg(t){const m=$('msg');m.textContent=t;m.style.display='block';clearTimeout(m._t);m._t=setTimeout(()=>m.style.display='none',4000)}
async function get(u){const r=await fetch(u,{cache:'no-store'});return r.json()}
async function post(u,a={}){a.t=Math.floor(Date.now()/1000);const r=await fetch(u,{method:'POST',body:new URLSearchParams(a)});
 if(r.status==401){msg('Login needed: user admin, hotspot password');return null}const j=await r.json();msg(j.ok?'Done':('Refused: '+j.err));refresh();return j}
function fmt(v,d){return v==null?'--':Number(v).toFixed(d)}
function esc(s){return String(s).replace(/[&<>"]/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;'}[c]))}
function extra(id,v){if(v==null)return '';if(id=='fuel_vac')return ' ('+(v*2.036).toFixed(2)+' inHg)';if(id=='air')return ' ('+(v/27.68).toFixed(3)+' psi)';return ''}
function sigList(){const a=meta.sig.map((s,i)=>({id:s.id,name:s.name,unit:s.unit,dec:s.dec,i}));
 for(const j of (live&&live.j)||[])a.push({id:j.id,name:j.name,unit:j.unit,dec:j.dec,j:true});return a}
function valOf(s){if(!live)return{v:null,st:1};if(s.j){const j=live.j.find(x=>x.id==s.id);return j?{v:j.v,st:j.age>5000?1:0,age:j.age,mn:j.mn,mx:j.mx,tmn:j.tmn,tmx:j.tmx}:{v:null,st:1}}
 const x=live.sig[s.i];return{v:x[0],st:x[1],mn:x[2],mx:x[3],tmn:x[4],tmx:x[5]}}
function mmLine(s,x){if(x.mn==null)return '<div class="mm">min/max: no reading yet</div>';const f=v=>s.id=='c58'?c58(v):fmt(v,s.dec);
 return `<div class="mm">min ${f(x.mn)} @ ${x.tmn} s<br>max ${f(x.mx)} @ ${x.tmx} s</div>`}
function c58(v){return v==null?'--':(v>0.5?'OPEN':'closed')}
function renderDash(){const ids=meta.dash.split(',').filter(x=>x);const all=sigList();let h='';
 for(const id of ids){let s=all.find(x=>x.id==id);if(!s){if(id[0]=='j')h+=`<div class="tile"><div class="n">${esc(id)}</div><div class="v mute">--</div><div class="s mute">not heard on J1587</div></div>`;continue}
  const x=valOf(s);const cls=x.st?'warn':'ok';let vs=s.id=='c58'?c58(x.v):fmt(x.v,s.dec);
  let note=s.j?(x.st?'stale':'J1587'):ST[x.st];if(s.id=='c58'&&x.v>0.5)note='restricted or broken wire';
  h+=`<div class="tile"><div class="n">${esc(s.name)}</div><div class="v">${vs}<span class="u">${esc(s.unit)}</span></div><div class="s ${cls}">${esc(note)}${extra(s.id,x.v)}</div>${mmLine(s,x)}</div>`}
 $('tiles').innerHTML=h||'<p>No sensors chosen. Pick some under Settings.</p>'}
function renderEng(){let h='<tr><th>Signal</th><th>Value</th><th>Status</th></tr>';
 meta.sig.forEach((s,i)=>{const x=live.sig[i];h+=`<tr><td>${esc(s.name)}</td><td>${s.id=='c58'?c58(x[0]):fmt(x[0],s.dec+1)+' '+esc(s.unit)}${extra(s.id,x[0])}</td><td class="${x[1]?'warn':'ok'}">${ST[x[1]]}</td></tr>`});$('engtab').innerHTML=h}
async function tick(){try{
 live=await get('/api/live');const e=live.eng;$('eng').textContent='engine '+e.state+(e.rpm!=null?' '+e.rpm+' rpm':'');
 $('eng').className=e.state=='off'?'ok':'warn';$('hdr').textContent='up '+live.up+' s';$('upnow').textContent=live.up;
 if(cur=='dash')renderDash();
 if(cur=='sig'){renderEng();const r=await get('/api/raw');
  $('adcsum').textContent='ADCs: '+['U7 0x48','U8 0x49','U9 0x4A','U10 0x4B'].map((n,i)=>n+(r.ok[i]?' ok':' MISSING')+(r.err[i]?' ('+r.err[i]+' errors)':'')).join(', ')+'. '+r.hz+' scans/s, free heap '+r.heap+'. Wi-Fi: '+r.wifi;
  let h='<tr><th>Channel</th><th>C55 pin / wire</th><th>ADC V</th><th>min</th><th>max</th><th>Input</th><th>Ratio</th></tr>';
  for(const c of r.ch)h+=`<tr><td>${c.n}</td><td>${c.pin}</td><td>${fmt(c.v,4)}</td><td>${fmt(c.min,4)}</td><td>${fmt(c.max,4)}</td><td>${fmt(c.in,c.n=='SHUNT_I'?0:3)}${c.n=='SHUNT_I'?' A':' V'}</td><td>${c.r==null?'':fmt(c.r,4)}</td></tr>`;$('rawtab').innerHTML=h}
 if(cur=='j'){const j=await get('/api/j1587');const s=j.st;
  $('jst').innerHTML=`${s.frames} good frames, ${s.bad} bad checksum, ${s.malformed} malformed, ${s.long} over 21 bytes, ${s.uart} UART errors, ${s.ign} ignored (not a J1587 MID), ${s.bytes} bytes. Parameter table ${s.slots}/${s.slotmax}${s.pdrop?', '+s.pdrop+' dropped (table full)':''}${s.ddrop?', '+s.ddrop+' trouble codes dropped':''}. Last frame: ${s.last==null?'never':(s.last/1000).toFixed(1)+' s ago'}`;
  let h='<tr><th>MID</th><th>PID</th><th>Name</th><th>Value</th><th>Raw</th><th>Age</th><th>Count</th></tr>';
  j.pids.sort((a,b)=>a.mid-b.mid||a.pid-b.pid);
  let d='<tr><th>Module</th><th>Code</th><th>Fault</th><th>Status</th><th>Count</th><th>Last sent</th></tr>';
  j.dtc.sort((a,b)=>b.active-a.active||a.mid-b.mid);
  for(const c of j.dtc)d+=`<tr><td>${c.mid} ${esc(c.midn)}</td><td>${c.sid?'SID':'PID'} ${c.num}${c.name?' '+esc(c.name):''}</td><td>FMI ${c.fmi}: ${esc(c.fmin)}</td><td class="${c.active?'bad':'mute'}">${c.active?'ACTIVE':'inactive'}</td><td>${c.count||''}</td><td>${(c.age/1000).toFixed(0)} s ago</td></tr>`;
  $('dtab').innerHTML=j.dtc.length?d:'<tr><td>No trouble codes heard.</td></tr>';
  for(const p of j.pids)h+=`<tr><td>${p.mid} ${esc(p.midn)}</td><td>${p.pid}</td><td>${esc(p.name)}</td><td>${p.v==null?'':fmt(p.v,2)+' '+esc(p.unit)}</td><td><code>${p.hex}</code></td><td>${(p.age/1000).toFixed(1)} s</td><td>${p.n}</td></tr>`;$('jtab').innerHTML=h}
 if(cur=='cal')await renderCal();
 if(cur=='mod'){const n=await get('/api/nodes');$('nowst').textContent=`This TIM: ${n.me}, ${n.ready?'ready':'NOT ready'}, sent ${n.tx} (${n.fail} failed), received ${n.rx}.`;
  let h='<tr><th>MAC</th><th>Type</th><th>RSSI</th><th>Last</th><th>Packets</th><th>Last payload</th></tr>';
  for(const x of n.nodes)h+=`<tr><td>${x.mac}</td><td>${esc(x.type)}</td><td>${x.rssi}</td><td>${(x.age/1000).toFixed(1)} s</td><td>${x.n}</td><td><code>${esc(x.last)}</code></td></tr>`;$('nowtab').innerHTML=h}
}catch(e){$('hdr').textContent='connection lost'}}
const CN=['C59 fuel inlet (psia)','C61 oil outlet (psi)','C62 oil inlet (psi)','C63 air cleaner (psia)'];
const VI=['v1437','v1139','v1138','v1431','v1274','v2010','v1435a','v1435b','v2085'];
function when(t){return t?new Date(t*1000).toLocaleString():'never'}
async function renderCal(){const c=await get('/api/cal');const e=c.eng;
 $('calst').innerHTML=e.state=='off'?'<span class="ok">Engine off ('+esc(e.reason)+').</span>':'<span class="warn">Engine '+e.state+' ('+esc(e.reason)+'). Zeroing still works, but with the engine running it stores a non-zero reading as zero.</span>';
 const z=[['Starter shunt','zero at '+fmt(c.shunt_zero,4)+' V',c.epoch[9]],['C59 barometric reference',fmt(c.baro,3)+' psia (now '+fmt(c.unc[0],3)+', ratio '+fmt(c.ratio[0],4)+')',c.epoch[10]],
  ['Oil inlet C62','offset '+fmt(c.zero[2],3)+' psi (ratio now '+fmt(c.ratio[2],4)+')',c.epoch[13]],['Oil outlet C61','offset '+fmt(c.zero[1],3)+' psi (ratio now '+fmt(c.ratio[1],4)+')',c.epoch[12]],
  ['Air cleaner C63','reference '+(c.zero[3]>5?fmt(c.zero[3],3)+' psia':'not set (using C59 baro)')+' (now '+fmt(c.unc[3],3)+' psia, ratio '+fmt(c.ratio[3],4)+')',c.epoch[14]]];
 $('zerotab').innerHTML='<tr><th>Item</th><th>Stored</th><th>Set</th></tr>'+z.map(r=>`<tr><td>${r[0]}</td><td>${r[1]}</td><td>${when(r[2])}</td></tr>`).join('');
 if(!document.activeElement||document.activeElement.tagName!='INPUT'){
 let h='<tr><th>Signal</th><th>Board reads</th><th>Trim</th><th>Meter V</th><th></th></tr>';
 VI.forEach((id,i)=>{const s=meta.sig.find(x=>x.id==id);h+=`<tr><td>${esc(s.name)}</td><td>${fmt(c.vraw[i]*c.vgain[i],2)} V</td><td>${fmt(c.vgain[i],4)}</td><td><input id="m_${id}" size="6" inputmode="decimal"></td><td><button onclick="meter('${id}')">Set</button> <button onclick="post('/api/cal/reset',{what:'${id}'})">Reset</button></td></tr>`});$('vtab').innerHTML=h;
 h='<tr><th>Sensor</th><th>Ratio low</th><th>Ratio high</th><th>Low</th><th>High</th><th></th></tr>';
 c.curve.forEach((k,i)=>{h+=`<tr><td>${CN[i]}</td>`+k.map((v,j)=>`<td><input id="c${i}_${j}" size="5" value="${v}" inputmode="decimal"></td>`).join('')+`<td><button onclick="curve(${i})">Save</button></td></tr>`});$('ctab').innerHTML=h}
 $('signst').textContent='now '+(c.shunt_sign>0?'normal':'reversed');window._sign=c.shunt_sign}
function zero(w){if(confirm('Zero '+w+' now? Engine should be off.'))post('/api/cal/zero',{what:w})}
function meter(id){const v=$('m_'+id).value;if(v)post('/api/cal/meter',{id,v})}
function curve(i){post('/api/cal/curve',{c:i,rlo:$('c'+i+'_0').value,rhi:$('c'+i+'_1').value,plo:$('c'+i+'_2').value,phi:$('c'+i+'_3').value})}
function sign(){post('/api/cal/shuntsign',{s:-(window._sign||1)})}
function renderSel(){const ids=meta.dash.split(',').filter(x=>x);const all=sigList();const order=[...ids,...all.map(s=>s.id).filter(i=>!ids.includes(i))];
 jsel=order.map(id=>({id,on:ids.includes(id),s:all.find(x=>x.id==id)}));drawSel()}
function drawSel(){$('sellist').innerHTML=jsel.map((x,k)=>`<li><input type="checkbox" ${x.on?'checked':''} onchange="jsel[${k}].on=this.checked"><span>${esc(x.s?x.s.name:x.id+' (not heard yet)')}</span><button onclick="mv(${k},-1)">&uarr;</button><button onclick="mv(${k},1)">&darr;</button></li>`).join('')}
function mv(k,d){const t=k+d;if(t<0||t>=jsel.length)return;[jsel[k],jsel[t]]=[jsel[t],jsel[k]];drawSel()}
async function saveDash(){const ids=jsel.filter(x=>x.on).map(x=>x.id).join(',');const r=await post('/api/dash',{ids});if(r&&r.ok)meta.dash=ids}
function saveWifi(){if(confirm('Save Wi-Fi settings and restart? Reconnect to the hotspot afterwards.'))post('/api/wifi',{ap_ssid:$('ap_ssid').value,ap_pass:$('ap_pass').value,sta_en:$('sta_en').checked?1:0,sta_ssid:$('sta_ssid').value,sta_ch1:$('sta_ch1').checked?1:0,sta_pass:$('sta_pass').value})}
async function scan(){$('scantab').innerHTML='<tr><td>Scanning (the hotspot may drop for a few seconds)...</td></tr>';
 try{const r=await get('/api/wifi/scan');r.nets.sort((a,b)=>b.rssi-a.rssi);
  $('scantab').innerHTML='<tr><th>Network</th><th>Channel</th><th>Signal</th><th></th></tr>'+r.nets.map(n=>`<tr><td>${esc(n.ssid||'(hidden)')}</td><td>${n.ch}</td><td>${n.rssi} dBm</td><td><button onclick="$('sta_ssid').value=this.dataset.s" data-s="${esc(n.ssid)}">Use</button></td></tr>`).join('')}
 catch(e){$('scantab').innerHTML='<tr><td>Scan failed; reconnect to the hotspot and try again.</td></tr>'}}
function upload(){const f=$('bin').files[0];if(!f){msg('Choose a .bin file first');return}
 const x=new XMLHttpRequest();const fd=new FormData();fd.append('firmware',f,f.name);x.open('POST','/update');
 x.upload.onprogress=e=>{$('upst').textContent=Math.round(100*e.loaded/e.total)+'%'};
 x.onload=()=>{if(x.status==401){$('upst').textContent='login needed, press Upload again';return}let j={};try{j=JSON.parse(x.responseText)}catch(e){}
  $('upst').textContent=j.ok?'Done. Restarting, reload in 20 s.':'Failed: '+(j.err||x.status)};
 x.onerror=()=>{$('upst').textContent='upload failed'};x.send(fd)}
async function refresh(){meta=await get('/api/meta');$('fw').textContent=meta.fw;$('ap_ssid').value=meta.wifi.ap_ssid;$('sta_en').checked=meta.wifi.sta_en;$('sta_ssid').value=meta.wifi.sta_ssid;$('sta_ch1').checked=meta.wifi.sta_ch1;$('wst').textContent=meta.wifi.status;
 $('dashnote').textContent='Engineering units: psi, V, A. Change what shows under Settings.';await tick();if(cur=='set')renderSel()}
document.querySelectorAll('#nav button').forEach(b=>b.onclick=()=>{cur=b.dataset.s;document.querySelectorAll('#nav button').forEach(x=>x.classList.toggle('on',x==b));
 document.querySelectorAll('section').forEach(s=>s.classList.toggle('on',s.id==cur));if(cur=='set')renderSel();tick()});
refresh();setInterval(()=>{if(cur!='set')tick()},1000);
</script></body></html>)HTML";
