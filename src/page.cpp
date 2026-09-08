#include "page.h"

const char PAGE_TABS[] = R"HTML(<!doctype html>
<html><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Meshtastic Monitor</title>
<style>
body{margin:0;background:#f5f7f6;color:#17221e;font:14px Georgia,serif}
main{max-width:1450px;margin:auto;padding:28px 18px}
h1{font-size:28px;margin:0 0 5px}p{color:#52605a;margin:0 0 22px}
nav{display:flex;gap:8px;margin-bottom:14px;border-bottom:1px solid #cbd8d0}
button{border:0;border-bottom:3px solid transparent;background:transparent;color:#52605a;cursor:pointer;font:inherit;padding:9px 14px}
button[aria-selected=true]{border-bottom-color:#28704f;color:#17221e;font-weight:bold}
section[hidden]{display:none}table{width:100%;border-collapse:collapse;background:#fff}
th,td{padding:10px;text-align:left;border-bottom:1px solid #dce4df;vertical-align:top}
th{background:#e2ebe5;font-size:12px}code{display:block;max-width:460px;white-space:pre-wrap;word-break:break-word;font:12px ui-monospace,monospace}
.detail{min-width:220px}.detail strong{display:block;color:#234f3e;font-size:13px;margin-bottom:4px}.detail span{display:block;margin:2px 0}.meta{color:#65736c;font-size:12px}
.raw{margin-top:7px}.raw summary{color:#65736c;cursor:pointer;font-size:12px}.muted{color:#87938d}
@media(max-width:1050px){main{padding:18px 10px}.topic-cell{display:none}}@media(max-width:760px){td,th{padding:7px}.detail{min-width:150px}}
@media(max-width:560px){th:nth-child(3),td:nth-child(3),th:nth-child(5),td:nth-child(5){display:none}}
</style></head><body><main><h1>Meshtastic Monitor</h1><p id="status">Loading recent data...</p>
<nav role="tablist"><button id="packets-tab" role="tab" aria-selected="true" aria-controls="packets-panel">Packets</button><button id="nodes-tab" role="tab" aria-selected="false" aria-controls="nodes-panel">Nodes</button></nav>
<section id="packets-panel" role="tabpanel" aria-labelledby="packets-tab"><table><thead><tr><th>Received</th><th>Topic</th><th>Region</th><th>Channel</th><th>Node</th><th>Packet</th><th>Sender</th><th>Details</th></tr></thead><tbody id="packets"></tbody></table></section>
<section id="nodes-panel" role="tabpanel" aria-labelledby="nodes-tab" hidden><table><thead><tr><th>Node ID</th><th>Long name</th><th>Short name</th><th>Hardware</th><th>Role</th><th>Licensed</th><th>Messaging</th><th>PKI</th><th>Last seen</th></tr></thead><tbody id="nodes"></tbody></table></section>
</main><script>
const packetBody=document.querySelector('#packets'),nodeBody=document.querySelector('#nodes'),status=document.querySelector('#status'),packetPanel=document.querySelector('#packets-panel'),nodePanel=document.querySelector('#nodes-panel');
function esc(s){const x=document.createElement('span');x.textContent=s==null?'':String(s);return x.innerHTML}
function number(v,unit){return v==null?'':`<span>${esc(v)}${unit||''}</span>`}
function details(x){const m=x.measurement;if(!m)return '<span class="muted">No decoded measurement</span>';let html=`<strong>${esc(m.kind)}</strong>`;if(m.text)html+=`<span>${esc(m.text)}</span>`;if(m.long_name||m.short_name)html+=`<span>${esc(m.long_name)} ${m.short_name?'('+esc(m.short_name)+')':''}</span>`;if(m.latitude!=null)html+=number(m.latitude,', ')+number(m.longitude,'');if(m.altitude!=null)html+=number(m.altitude,' m');if(m.battery_level!=null)html+=number(m.battery_level,'% battery');if(m.voltage!=null)html+=number(m.voltage,' V');if(m.temperature!=null)html+=number(m.temperature,' C');if(m.relative_humidity!=null)html+=number(m.relative_humidity,'% RH');if(m.pressure!=null)html+=number(m.pressure,' hPa');return html}
function renderPackets(p){packetBody.innerHTML=p.map(x=>`<tr><td>${new Date(x.received_at*1000).toLocaleString()}</td><td class="topic-cell"><code>${esc(x.topic)}</code><span class="meta">${esc(x.transport)} / ${esc(x.encoding)}</span></td><td>${esc(x.region)}</td><td>${esc(x.channel)}</td><td>${esc(x.node)}</td><td><strong>${esc(x.packet_type)||'<span class="muted">binary</span>'}</strong><span class="meta">${esc(x.sender)}</span></td><td>${esc(x.sender)}</td><td class="detail">${details(x)}<details class="raw"><summary>Raw payload</summary><code>${esc(x.decoded_payload_hex||x.payload_hex)}</code></details></td></tr>`).join('')}
function yesNo(v){return v==null?'<span class="muted">Unknown</span>':v?'Yes':'No'}
function renderNodes(nodes){nodeBody.innerHTML=nodes.map(x=>`<tr><td><code>${esc(x.node_id)}</code></td><td>${esc(x.long_name)||'<span class="muted">Unnamed</span>'}</td><td>${esc(x.short_name)}</td><td>${esc(x.hardware_model)||'<span class="muted">Unknown</span>'}</td><td>${esc(x.role)||'<span class="muted">Unknown</span>'}</td><td>${yesNo(x.is_licensed)}</td><td>${x.is_unmessagable===null?'<span class="muted">Unknown</span>':x.is_unmessagable?'Disabled':'Enabled'}</td><td>${x.has_public_key?'Available':'None'}</td><td>${new Date(x.last_seen*1000).toLocaleString()}</td></tr>`).join('')||'<tr><td colspan="9" class="muted">No nodeinfo packets received yet</td></tr>'}
function showTab(name){const nodes=name==='nodes';packetPanel.hidden=nodes;nodePanel.hidden=!nodes;document.querySelector('#packets-tab').setAttribute('aria-selected',String(!nodes));document.querySelector('#nodes-tab').setAttribute('aria-selected',String(nodes))}
document.querySelector('#packets-tab').addEventListener('click',()=>showTab('packets'));document.querySelector('#nodes-tab').addEventListener('click',()=>showTab('nodes'));
async function load(){try{const [packets,nodes]=await Promise.all([fetch('api/packets').then(x=>x.json()),fetch('api/nodes').then(x=>x.json())]);renderPackets(packets);renderNodes(nodes);status.textContent=`${packets.length} recent packets · ${nodes.length} seen nodes`;}catch(e){status.textContent='Unable to load monitor data';}}
load();setInterval(load,10000)
</script></body></html>)HTML";