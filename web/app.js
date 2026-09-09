const packetBody = document.querySelector('#packets');
const nodeBody = document.querySelector('#nodes');
const status = document.querySelector('#status');
const packetPanel = document.querySelector('#packets-panel');
const nodePanel = document.querySelector('#nodes-panel');
const packetFilter = document.querySelector('#packet-filter');
const nodeFilter = document.querySelector('#node-filter');
const showStatus = document.querySelector('#show-status');
const statusColumn = document.querySelector('.node-status-column');
const connectionStatus = document.querySelector('#connection-status');
const mqttStatus = document.querySelector('#mqtt-status');
const statusText = document.querySelector('#status-text');
const tableState = {
  packets: { items: [], filter: '', sortKey: null, descending: false },
  nodes: { items: [], filter: '', sortKey: null, descending: false },
};

function esc(value) { const element = document.createElement('span'); element.textContent = value == null ? '' : String(value); return element.innerHTML; }
function nodeLabel(id, name) { return name ? `<a href="#nodes" data-node-id="${esc(id)}">${esc(name)}</a>` : esc(id); }
function number(value, unit) { return value == null ? '' : `<span>${esc(value)}${unit || ''}</span>`; }
function packetTypeLabel(type) {
  if (!type) return '<span class="muted">binary</span>';
  let label = type;
  let security = '';
  if (label.startsWith('decrypted_')) {
    security = '<span class="packet-security decrypted" title="Decrypted" aria-label="Decrypted">&#128275;</span>';
    label = label.slice('decrypted_'.length);
  } else if (label === 'encrypted') {
    security = '<span class="packet-security encrypted" title="Encrypted" aria-label="Encrypted">&#128274;</span>';
    return `${security}<span class="packet-type-label">Encrypted</span>`;
  }
  label = label.replace(/_APP$/, '').replaceAll('_', ' ');
  return `${security}<span class="packet-type-label">${esc(label)}</span>`;
}
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
  packetBody.innerHTML = packets.map(packet => `<tr><td>${new Date(packet.received_at * 1000).toLocaleString()}</td><td class="topic-cell"><details class="topic"><summary>View</summary><code>${esc(packet.topic)}</code><span class="meta">${esc(packet.transport)} / ${esc(packet.encoding)}</span></details></td><td>${esc(packet.region)}</td><td>${esc(packet.channel)}</td><td>${esc(packet.node)}</td><td><strong>${packetTypeLabel(packet.packet_type)}</strong></td><td>${nodeLabel(packet.sender, packet.sender_name)}</td><td>${nodeLabel(packet.observer, packet.observer_name)}</td><td class="detail">${details(packet)}<details class="raw"><summary>Raw payload</summary><code>${esc(packet.decoded_payload_hex || packet.payload_hex)}</code></details></td></tr>`).join('');
}

function yesNo(value) { return value == null ? missing() : value ? 'Yes' : 'No'; }
function missing() { return '<span class="muted">--</span>'; }
function coordinate(value) { return value == null ? missing() : esc(Number(value).toFixed(5)); }
function altitude(value) { return value == null ? missing() : `${esc(Number(value).toFixed(0))} m`; }
function nodeStatus(node) {
  const messaging = node.is_unmessagable === null ? missing() : node.is_unmessagable ? 'Disabled' : 'Enabled';
  return `<span>Licensed: ${yesNo(node.is_licensed)}</span><span>PKI: ${node.has_public_key ? 'Available' : 'None'}</span><span>Messaging: ${messaging}</span>`;
}
function mapLink(node) {
  if (node.latitude == null || node.longitude == null) return missing();
  const latitude = encodeURIComponent(node.latitude);
  const longitude = encodeURIComponent(node.longitude);
  return `<a href="https://www.openstreetmap.org/?mlat=${latitude}&mlon=${longitude}#map=15/${latitude}/${longitude}" target="_blank" rel="noopener noreferrer">OSM</a>`;
}
function renderNodes() {
  const nodes = visibleItems('nodes');
  nodeBody.innerHTML = nodes.map(node => `<tr><td><code>${esc(node.node_id)}</code></td><td>${esc(node.long_name) || missing()}</td><td>${esc(node.short_name)}</td><td class="node-hardware">${esc(node.hardware_model) || missing()}</td><td>${esc(node.role) || missing()}</td><td class="node-status" data-column="status"${showStatus.checked ? '' : ' hidden'}>${nodeStatus(node)}</td><td class="node-latitude">${coordinate(node.latitude)}</td><td class="node-longitude">${coordinate(node.longitude)}</td><td class="node-altitude">${altitude(node.altitude)}</td><td class="node-map">${mapLink(node)}</td><td>${new Date(node.last_seen * 1000).toLocaleString()}</td></tr>`).join('') || '<tr><td colspan="11" class="muted">No nodeinfo packets received yet</td></tr>';
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
showStatus.addEventListener('change', () => {
  statusColumn.classList.toggle('visible', showStatus.checked);
  document.querySelector('th[data-column="status"]').hidden = !showStatus.checked;
  renderNodes();
});
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
  connectionStatus.className = 'connection-status pending';
  connectionStatus.setAttribute('aria-label', 'Refreshing monitor data');
  connectionStatus.title = 'Refreshing monitor data';
  try {
    const [packets, nodes, monitorStatus] = await Promise.all([fetch('/api/packets').then(response => response.json()), fetch('/api/nodes').then(response => response.json()), fetch('/api/status').then(response => response.json())]);
    tableState.packets.items = groupPackets(packets);
    tableState.nodes.items = nodes;
    renderPackets();
    renderNodes();
    updateSortIndicators();
    statusText.textContent = `${tableState.packets.items.length} logical packets · ${nodes.length} seen nodes`;
    connectionStatus.className = 'connection-status connected';
    connectionStatus.setAttribute('aria-label', 'Monitor connected');
    connectionStatus.title = 'Monitor connected';
    mqttStatus.className = `connection-status ${monitorStatus.mqtt_connected ? 'connected' : 'error'}`;
    mqttStatus.setAttribute('aria-label', monitorStatus.mqtt_connected ? 'MQTT connected' : 'MQTT disconnected');
    mqttStatus.title = monitorStatus.mqtt_connected ? 'MQTT connected' : 'MQTT disconnected';
  } catch (error) {
    statusText.textContent = 'Unable to load monitor data';
    connectionStatus.className = 'connection-status error';
    connectionStatus.setAttribute('aria-label', 'Monitor connection error');
    connectionStatus.title = 'Monitor connection error';
    mqttStatus.className = 'connection-status error';
    mqttStatus.setAttribute('aria-label', 'MQTT status unavailable');
    mqttStatus.title = 'MQTT status unavailable';
  }
}

load();
setInterval(load, 10000);