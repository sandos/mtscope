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
function nodeLabel(id, name) { return name ? `<a href="#nodes" data-node-id="${esc(id)}">${esc(name)}</a>` : esc(id); }
function number(value, unit) { return value == null ? '' : `<span>${esc(value)}${unit || ''}</span>`; }
function groupPackets(packets) {
  const groups = new Map();
  packets.forEach(packet => {
    const key = packet.packet_key || `${packet.received_at}:${packet.topic}:${packet.payload_hex}`;
    let group = groups.get(key);
    if (!group) {
      group = { ...packet, observations: [] };
      groups.set(key, group);
    }
    group.observations.push(packet);
    group.observation_count = group.observations.length;
  });
  return Array.from(groups.values());
}

function observationDetails(packet) {
  const observations = packet.observations || [packet];
  const rows = observations.map(observation => {
    const metrics = [
      observation.rx_rssi == null ? '' : `RSSI ${esc(observation.rx_rssi)} dBm`,
      observation.rx_snr == null ? '' : `SNR ${esc(observation.rx_snr)} dB`,
      observation.rx_time == null ? '' : `received ${new Date(observation.rx_time * 1000).toLocaleString()}`,
      observation.hop_start == null || observation.hop_limit == null ? '' : `hops ${esc(observation.hop_start - observation.hop_limit)}`,
    ].filter(Boolean).join(' · ');
    return `<div class="observation"><strong>${nodeLabel(observation.observer, observation.observer_name) || '<span class="muted">Unknown observer</span>'}</strong><span>${esc(new Date(observation.received_at * 1000).toLocaleString())}</span>${metrics ? `<span class="meta">${metrics}</span>` : ''}<span class="meta">${esc(observation.topic)}</span></div>`;
  }).join('');
  return `<details class="observations"${observations.length > 1 ? '' : ''}><summary>Observations (${observations.length})</summary>${rows}</details>`;
}

function details(packet) {
  const measurement = packet.measurement;
  let html = observationDetails(packet);
  if (!measurement) return html + '<span class="muted">No decoded measurement</span>';
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
  packetBody.innerHTML = packets.map(packet => `<tr><td>${new Date(packet.received_at * 1000).toLocaleString()}</td><td class="topic-cell"><details class="topic"><summary>View</summary><code>${esc(packet.topic)}</code><span class="meta">${esc(packet.transport)} / ${esc(packet.encoding)}</span></details></td><td>${esc(packet.region)}</td><td>${esc(packet.channel)}</td><td>${esc(packet.node)}</td><td><strong>${esc(packet.packet_type) || '<span class="muted">binary</span>'}</strong></td><td>${nodeLabel(packet.sender, packet.sender_name)}</td><td>${nodeLabel(packet.observer, packet.observer_name)}</td><td class="detail">${details(packet)}<details class="raw"><summary>Raw payload</summary><code>${esc(packet.decoded_payload_hex || packet.payload_hex)}</code></details></td></tr>`).join('');
}

function yesNo(value) { return value == null ? '<span class="muted">Unknown</span>' : value ? 'Yes' : 'No'; }
function mapLink(node) {
  if (node.latitude == null || node.longitude == null) return '<span class="muted">Unknown</span>';
  const latitude = encodeURIComponent(node.latitude);
  const longitude = encodeURIComponent(node.longitude);
  return `<a href="https://www.openstreetmap.org/?mlat=${latitude}&mlon=${longitude}#map=15/${latitude}/${longitude}" target="_blank" rel="noopener noreferrer">OpenStreetMap</a>`;
}
function renderNodes() {
  const nodes = visibleItems('nodes');
  nodeBody.innerHTML = nodes.map(node => `<tr><td><code>${esc(node.node_id)}</code></td><td>${esc(node.long_name) || '<span class="muted">Unnamed</span>'}</td><td>${esc(node.short_name)}</td><td>${esc(node.hardware_model) || '<span class="muted">Unknown</span>'}</td><td>${esc(node.role) || '<span class="muted">Unknown</span>'}</td><td>${yesNo(node.is_licensed)}</td><td>${node.is_unmessagable === null ? '<span class="muted">Unknown</span>' : node.is_unmessagable ? 'Disabled' : 'Enabled'}</td><td>${node.has_public_key ? 'Available' : 'None'}</td><td>${node.latitude == null ? '<span class="muted">Unknown</span>' : esc(node.latitude)}</td><td>${node.longitude == null ? '<span class="muted">Unknown</span>' : esc(node.longitude)}</td><td>${node.altitude == null ? '<span class="muted">Unknown</span>' : `${esc(node.altitude)} m`}</td><td>${mapLink(node)}</td><td>${new Date(node.last_seen * 1000).toLocaleString()}</td></tr>`).join('') || '<tr><td colspan="13" class="muted">No nodeinfo packets received yet</td></tr>';
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
packetBody.addEventListener('click', event => {
  const link = event.target.closest('a[data-node-id]');
  if (!link) return;
  event.preventDefault();
  nodeFilter.value = link.dataset.nodeId;
  tableState.nodes.filter = link.dataset.nodeId;
  showTab('nodes');
  renderNodes();
});
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
    tableState.packets.items = groupPackets(packets);
    tableState.nodes.items = nodes;
    renderPackets();
    renderNodes();
    updateSortIndicators();
    status.textContent = `${tableState.packets.items.length} logical packets · ${nodes.length} seen nodes`;
  } catch (error) {
    status.textContent = 'Unable to load monitor data';
  }
}

load();
setInterval(load, 10000);