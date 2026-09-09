const packetBody = document.querySelector('#packets');
const nodeBody = document.querySelector('#nodes');
const status = document.querySelector('#status');
const packetPanel = document.querySelector('#packets-panel');
const nodePanel = document.querySelector('#nodes-panel');
const packetFilter = document.querySelector('#packet-filter');
const nodeFilter = document.querySelector('#node-filter');
const tableState = {
  packets: { items: [], filter: '', sortKey: null, descending: false },
  nodes: { items: [], filter: '', sortKey: null, descending: false },
};

function esc(value) { const element = document.createElement('span'); element.textContent = value == null ? '' : String(value); return element.innerHTML; }
function number(value, unit) { return value == null ? '' : `<span>${esc(value)}${unit || ''}</span>`; }
function details(packet) {
  const measurement = packet.measurement;
  let html = packet.observer ? `<span class="meta">Observer: ${esc(packet.observer)}</span>` : '';
  if (packet.content_hash) html += `<span class="meta">Hash: <code>${esc(packet.content_hash)}</code></span>`;
  if (!measurement) return html || '<span class="muted">No decoded measurement</span>';
  html += `<strong>${esc(measurement.kind)}</strong>`;
  if (measurement.text) html += `<span>${esc(measurement.text)}</span>`;
  if (measurement.long_name || measurement.short_name) html += `<span>${esc(measurement.long_name)} ${measurement.short_name ? `(${esc(measurement.short_name)})` : ''}</span>`;
  if (measurement.latitude != null) html += number(measurement.latitude, ', ') + number(measurement.longitude, '');
  if (measurement.altitude != null) html += number(measurement.altitude, ' m');
  if (measurement.battery_level != null) html += number(measurement.battery_level, '% battery');
  if (measurement.voltage != null) html += number(measurement.voltage, ' V');
  if (measurement.temperature != null) html += number(measurement.temperature, ' C');
  if (measurement.relative_humidity != null) html += number(measurement.relative_humidity, '% RH');
  if (measurement.pressure != null) html += number(measurement.pressure, ' hPa');
  return html;
}

function matchesFilter(item, filter) {
  return JSON.stringify(item).toLowerCase().includes(filter.toLowerCase());
}

function compareValues(left, right) {
  if (left == null) return right == null ? 0 : -1;
  if (right == null) return 1;
  if (typeof left === 'number' && typeof right === 'number') return left - right;
  return String(left).localeCompare(String(right));
}

function visibleItems(table) {
  const state = tableState[table];
  const items = state.items.filter(item => matchesFilter(item, state.filter));
  if (!state.sortKey) return items;
  return items.sort((left, right) => (state.descending ? -1 : 1) * compareValues(left[state.sortKey], right[state.sortKey]));
}

function updateSortIndicators() {
  document.querySelectorAll('th button[data-table]').forEach(button => {
    const state = tableState[button.dataset.table];
    if (button.dataset.key === state.sortKey) button.dataset.sortDirection = state.descending ? 'descending' : 'ascending';
    else delete button.dataset.sortDirection;
  });
}

function renderPackets() {
  const packets = visibleItems('packets');
  packetBody.innerHTML = packets.map(packet => `<tr><td>${new Date(packet.received_at * 1000).toLocaleString()}</td><td class="topic-cell"><details class="topic"><summary>View</summary><code>${esc(packet.topic)}</code><span class="meta">${esc(packet.transport)} / ${esc(packet.encoding)}</span></details></td><td>${esc(packet.region)}</td><td>${esc(packet.channel)}</td><td>${esc(packet.node)}</td><td><strong>${esc(packet.packet_type) || '<span class="muted">binary</span>'}</strong></td><td>${esc(packet.sender)}</td><td>${esc(packet.observer)}</td><td class="detail">${details(packet)}<details class="raw"><summary>Raw payload</summary><code>${esc(packet.decoded_payload_hex || packet.payload_hex)}</code></details></td></tr>`).join('');
}

function yesNo(value) { return value == null ? '<span class="muted">Unknown</span>' : value ? 'Yes' : 'No'; }
function renderNodes() {
  const nodes = visibleItems('nodes');
  nodeBody.innerHTML = nodes.map(node => `<tr><td><code>${esc(node.node_id)}</code></td><td>${esc(node.long_name) || '<span class="muted">Unnamed</span>'}</td><td>${esc(node.short_name)}</td><td>${esc(node.hardware_model) || '<span class="muted">Unknown</span>'}</td><td>${esc(node.role) || '<span class="muted">Unknown</span>'}</td><td>${yesNo(node.is_licensed)}</td><td>${node.is_unmessagable === null ? '<span class="muted">Unknown</span>' : node.is_unmessagable ? 'Disabled' : 'Enabled'}</td><td>${node.has_public_key ? 'Available' : 'None'}</td><td>${new Date(node.last_seen * 1000).toLocaleString()}</td></tr>`).join('') || '<tr><td colspan="9" class="muted">No nodeinfo packets received yet</td></tr>';
}

function showTab(name) {
  const showNodes = name === 'nodes';
  packetPanel.hidden = showNodes;
  nodePanel.hidden = !showNodes;
  document.querySelector('#packets-tab').setAttribute('aria-selected', String(!showNodes));
  document.querySelector('#nodes-tab').setAttribute('aria-selected', String(showNodes));
}

document.querySelector('#packets-tab').addEventListener('click', () => showTab('packets'));
document.querySelector('#nodes-tab').addEventListener('click', () => showTab('nodes'));
packetFilter.addEventListener('input', () => { tableState.packets.filter = packetFilter.value.trim(); renderPackets(); });
nodeFilter.addEventListener('input', () => { tableState.nodes.filter = nodeFilter.value.trim(); renderNodes(); });
document.querySelectorAll('th button[data-table]').forEach(button => {
  button.addEventListener('click', () => {
    const state = tableState[button.dataset.table];
    if (state.sortKey !== button.dataset.key) {
      state.sortKey = button.dataset.key;
      state.descending = false;
    } else if (!state.descending) {
      state.descending = true;
    } else {
      state.sortKey = null;
      state.descending = false;
    }
    updateSortIndicators();
    if (button.dataset.table === 'packets') renderPackets();
    else renderNodes();
  });
});

async function load() {
  try {
    const [packets, nodes] = await Promise.all([fetch('/api/packets').then(response => response.json()), fetch('/api/nodes').then(response => response.json())]);
    tableState.packets.items = packets;
    tableState.nodes.items = nodes;
    renderPackets();
    renderNodes();
    updateSortIndicators();
    status.textContent = `${packets.length} recent packets · ${nodes.length} seen nodes`;
  } catch (error) {
    status.textContent = 'Unable to load monitor data';
  }
}

load();
setInterval(load, 10000);