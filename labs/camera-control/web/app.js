const state = { config: null, zones: [], points: [] };

const $ = (id) => document.getElementById(id);
const log = (message, data) => {
  const suffix = data === undefined ? '' : `\n${JSON.stringify(data, null, 2)}`;
  $('log').textContent = `${new Date().toLocaleTimeString()}  ${message}${suffix}\n\n${$('log').textContent}`.slice(0, 6000);
};

function apiBase() {
  return ($('api-base').value || window.location.origin).replace(/\/$/, '');
}

async function request(path, options = {}) {
  const response = await fetch(`${apiBase()}${path}`, {
    ...options,
    headers: { 'Content-Type': 'application/json', ...(options.headers || {}) },
  });
  const body = await response.json().catch(() => ({}));
  if (!response.ok || body.ok === false) throw new Error(body.error || `HTTP ${response.status}`);
  return body.data;
}

let zoneImage = null;

function loadZoneSnapshot() {
  const image = new Image();
  image.onload = () => { zoneImage = image; drawZones(); };
  image.onerror = () => { zoneImage = null; drawZones(); log('Sin imagen de cámara para zonas. Activa el preview o la detección e intenta de nuevo.'); };
  image.src = `${apiBase()}/api/snapshot?t=${Date.now()}`;
}

function drawZones() {
  const canvas = $('zone-canvas');
  const ctx = canvas.getContext('2d');
  ctx.clearRect(0, 0, canvas.width, canvas.height);
  if (zoneImage) ctx.drawImage(zoneImage, 0, 0, canvas.width, canvas.height);
  else { ctx.fillStyle = '#172125'; ctx.fillRect(0, 0, canvas.width, canvas.height); }
  for (const zone of state.zones) {
    const points = zone.points || [];
    if (points.length < 2) continue;
    ctx.beginPath();
    points.forEach(([x, y], index) => index ? ctx.lineTo(x * canvas.width, y * canvas.height) : ctx.moveTo(x * canvas.width, y * canvas.height));
    ctx.closePath();
    ctx.fillStyle = zone.type === 'alert' ? '#d66f6333' : zone.type === 'exclude' ? '#77828a33' : '#d7b47d33';
    ctx.strokeStyle = zone.type === 'alert' ? '#d66f63' : zone.type === 'exclude' ? '#77828a' : '#d7b47d';
    ctx.lineWidth = 2;
    ctx.fill(); ctx.stroke();
    if (points[0]) { ctx.fillStyle = '#fff'; ctx.fillText(zone.name, points[0][0] * canvas.width + 6, points[0][1] * canvas.height - 6); }
  }
  if (state.points.length) {
    ctx.beginPath();
    state.points.forEach(([x, y], index) => index ? ctx.lineTo(x * canvas.width, y * canvas.height) : ctx.moveTo(x * canvas.width, y * canvas.height));
    ctx.strokeStyle = '#fff'; ctx.lineWidth = 2; ctx.stroke();
    state.points.forEach(([x, y]) => { ctx.fillStyle = '#fff'; ctx.beginPath(); ctx.arc(x * canvas.width, y * canvas.height, 4, 0, Math.PI * 2); ctx.fill(); });
  }
  $('zones-list').innerHTML = state.zones.length ? state.zones.map((zone, index) => `<div class="zone-item"><span>${index + 1}. ${zone.name} · ${zone.type}</span><button class="ghost" data-remove-zone="${index}">Quitar</button></div>`).join('') : '<span class="muted small">No hay zonas guardadas.</span>';
  document.querySelectorAll('[data-remove-zone]').forEach((button) => button.addEventListener('click', () => { state.zones.splice(Number(button.dataset.removeZone), 1); drawZones(); }));
}

function fillConfig(config) {
  state.config = config;
  state.zones = config.detection.zones || [];
  $('camera-host').value = config.camera.host || '';
  $('camera-user').value = config.camera.username || '';
  $('camera-pass').value = config.camera.password || '';
  $('cloud-user').value = config.camera.cloudUsername || 'admin';
  $('cloud-pass').value = config.camera.cloudPassword || '';
  $('record-mode').value = config.detection.recordMode || 'events';
  $('provider').value = config.detection.provider || 'native';
  $('fallback').value = config.detection.fallback || 'local';
  $('dwell').value = config.detection.dwellSeconds ?? 10;
  drawZones();
}

async function loadConfig() { try { fillConfig(await request('/api/config')); log('Configuración cargada.'); } catch (error) { log(`Error cargando configuración: ${error.message}`); } }

async function saveConfig() {
  const next = JSON.parse(JSON.stringify(state.config || { lab: {}, camera: {}, detection: {}, operator: {} }));
  next.camera = { ...(next.camera || {}), host: $('camera-host').value, username: $('camera-user').value, password: $('camera-pass').value, cloudUsername: $('cloud-user').value, cloudPassword: $('cloud-pass').value };
  next.detection = { ...(next.detection || {}), recordMode: $('record-mode').value, provider: $('provider').value, fallback: $('fallback').value, dwellSeconds: Number($('dwell').value), zones: state.zones };
  try { fillConfig(await request('/api/config', { method: 'PUT', body: JSON.stringify(next) })); log('config.toml actualizado.'); } catch (error) { log(`Error guardando: ${error.message}`); }
}

async function refreshStatus() {
  try { const status = await request('/api/status'); const connection = status.connection || {}; const transport = connection.transport ? ` · ${connection.transport}` : ''; $('camera-status').textContent = `${status.model || 'Cámara'} · ${status.firmware || 'sin firmware'}${transport}`; log('Estado de cámara leído.', status); } catch (error) { $('camera-status').textContent = 'Sin respuesta'; log(`Error de cámara: ${error.message}`); }
}

async function refreshCapabilities() {
  try { const capabilities = await request('/api/capabilities'); log('Capacidades PTZ leídas.', capabilities); } catch (error) { log(`Error leyendo capacidades: ${error.message}`); }
}

async function refreshOperator() {
  try { const status = await request('/api/operator/status'); const active = status.operatorActive; $('operator-status').textContent = active ? 'Operador con prioridad' : 'Operador libre'; $('operator-detail').textContent = active ? 'El LLM simulado no puede iniciar audio mientras el operador tiene prioridad.' : 'El operador puede tomar control antes de hablar.'; } catch (error) { log(`Error de operador: ${error.message}`); }
}

async function move(body) { try { await request('/api/ptz', { method: 'PATCH', body: JSON.stringify(body) }); log('Comando PTZ enviado.', body); } catch (error) { log(`Error PTZ: ${error.message}`); } }

async function loadPresets() {
  try { const result = await request('/api/presets'); const presets = result?.presets || result?.result?.presets || []; $('presets').innerHTML = presets.length ? presets.map((preset) => `<div class="preset-row"><span>${preset.name || preset.id}</span><button class="secondary" data-preset="${preset.id}">Ir</button></div>`).join('') : 'La cámara no devolvió posiciones.'; document.querySelectorAll('[data-preset]').forEach((button) => button.addEventListener('click', () => request('/api/preset', { method: 'POST', body: JSON.stringify({ action: 'goto', id: button.dataset.preset }) }).then(() => log('Preset ejecutado.')).catch((error) => log(`Error preset: ${error.message}`)))); } catch (error) { log(`Error leyendo presets: ${error.message}`); }
}

async function talk() {
  try { const source = $('talk-source').value; const result = await request('/api/talk', { method: 'POST', body: JSON.stringify({ text: $('talk-text').value, lang: $('talk-lang').value, source }) }); log(`Audio enviado por ${source}.`, result); } catch (error) { log(`Error de conversación: ${error.message}`); }
}

async function setOperator(path) { try { await request(path, { method: 'POST' }); await refreshOperator(); log(path.includes('takeover') ? 'Control tomado.' : 'Control liberado.'); } catch (error) { log(`Error de operador: ${error.message}`); } }

function previewSource() { return $('preview-source').value || 'labcam'; }

function previewUrl() {
  const api = ((state.config?.lab?.go2rtcApi) || 'http://127.0.0.1:1985').replace(/\/$/, '');
  return `${api}/api/stream.mp4?src=${previewSource()}`;
}

let previewController = null;
let previewTimer = null;

function trackLiveEdge(video, sourceBuffer) {
  const buffered = sourceBuffer.buffered;
  if (!buffered.length) return;
  const end = buffered.end(buffered.length - 1);
  if (end - video.currentTime > 2) video.currentTime = Math.max(end - 0.5, buffered.start(0));
  if (buffered.start(0) < video.currentTime - 30 && !sourceBuffer.updating) {
    try { sourceBuffer.remove(buffered.start(0), video.currentTime - 20); } catch { }
  }
}

async function pumpPreviewStream(reader, mediaSource, video) {
  const codecs = 'video/mp4; codecs="avc1.64001F"';
  const sourceBuffer = mediaSource.addSourceBuffer(codecs);
  sourceBuffer.mode = 'sequence';
  let pending = new Uint8Array(0);
  const appendBox = async (chunk) => {
    const merged = new Uint8Array(pending.length + chunk.length);
    merged.set(pending); merged.set(chunk, pending.length);
    let offset = 0;
    while (merged.length - offset >= 8) {
      const view = new DataView(merged.buffer, merged.byteOffset + offset, 8);
      const size = view.getUint32(0);
      if (size < 8 || offset + size > merged.length) break;
      offset += size;
    }
    if (offset) {
      const ready = merged.subarray(0, offset);
      pending = merged.slice(offset);
      if (sourceBuffer.updating) await new Promise((resolve) => sourceBuffer.addEventListener('updateend', resolve, { once: true }));
      sourceBuffer.appendBuffer(ready);
      await new Promise((resolve) => sourceBuffer.addEventListener('updateend', resolve, { once: true }));
      trackLiveEdge(video, sourceBuffer);
    } else {
      pending = merged;
    }
  };
  while (true) {
    const { done, value } = await reader.read();
    if (done) break;
    await appendBox(value);
  }
}

async function startPreview() {
  stopPreview();
  $('preview-hint').textContent = 'Conectando a go2rtc…';
  const video = $('preview');
  const controller = new AbortController();
  previewController = controller;
  video.classList.add('active');
  try {
    const response = await fetch(previewUrl(), { signal: controller.signal });
    if (!response.ok) throw new Error(`HTTP ${response.status}`);
    const mediaSource = new MediaSource();
    video.src = URL.createObjectURL(mediaSource);
    video.muted = true;
    await new Promise((resolve) => mediaSource.addEventListener('sourceopen', resolve, { once: true }));
    if (controller.signal.aborted) return;
    const reader = response.body.getReader();
    pumpPreviewStream(reader, mediaSource, video).catch((error) => {
      if (!controller.signal.aborted) { $('preview-hint').textContent = `Stream interrumpido: ${error.message}`; stopPreview(); }
    });
    previewTimer = setInterval(() => { video.play().catch(() => { }); }, 1000);
    $('preview-hint').textContent = 'Preview en vivo.';
  } catch (error) {
    video.classList.remove('active');
    if (!controller.signal.aborted) $('preview-hint').textContent = `go2rtc no responde: ${error.message}`;
  }
}

function stopPreview() {
  if (previewController) { previewController.abort(); previewController = null; }
  if (previewTimer) { clearInterval(previewTimer); previewTimer = null; }
  const video = $('preview');
  video.classList.remove('active');
  if (video.src) { URL.revokeObjectURL(video.src); video.removeAttribute('src'); video.load(); }
  $('preview-hint').textContent = 'Sin preview.';
}

$('zone-canvas').addEventListener('click', (event) => { const rect = event.currentTarget.getBoundingClientRect(); state.points.push([(event.clientX - rect.left) / rect.width, (event.clientY - rect.top) / rect.height]); drawZones(); });
$('zone-refresh').addEventListener('click', loadZoneSnapshot);
$('add-zone').addEventListener('click', () => { if (state.points.length < 3) return log('Una zona necesita al menos tres puntos.'); state.zones.push({ name: $('zone-name').value || `zona-${state.zones.length + 1}`, type: $('zone-type').value, points: state.points }); state.points = []; drawZones(); });
$('clear-points').addEventListener('click', () => { state.points = []; drawZones(); });
$('load-config').addEventListener('click', loadConfig); $('save-config').addEventListener('click', saveConfig); $('refresh-status').addEventListener('click', refreshStatus); $('refresh-capabilities').addEventListener('click', refreshCapabilities); $('load-presets').addEventListener('click', loadPresets); $('takeover').addEventListener('click', () => setOperator('/api/operator/takeover')); $('release').addEventListener('click', () => setOperator('/api/operator/release')); $('talk').addEventListener('click', talk);
document.querySelectorAll('[data-direction]').forEach((button) => button.addEventListener('click', () => move({ direction: Number(button.dataset.direction) })));
$('absolute-move').addEventListener('click', () => move({ x: Number($('move-x').value), y: Number($('move-y').value) }));
$('direction-move').addEventListener('click', () => move({ direction: Number($('move-direction').value) }));
$('calibrate').addEventListener('click', () => request('/api/ptz/calibrate', { method: 'POST' }).then(() => log('Corrección PTZ iniciada.')).catch((error) => log(`Error corrigiendo PTZ: ${error.message}`)));
$('stop-move').addEventListener('click', () => request('/api/ptz/stop', { method: 'POST' }).then(() => log('Orden de detención enviada.')).catch((error) => log(`Error deteniendo PTZ: ${error.message}`)));
$('alarm-on').addEventListener('click', () => request('/api/alarm', { method: 'POST', body: JSON.stringify({ enabled: true, volume: 80 }) }).then(() => log('Alarma activada.')).catch((error) => log(`Error de alarma: ${error.message}`)));
$('alarm-off').addEventListener('click', () => request('/api/alarm', { method: 'POST', body: JSON.stringify({ enabled: false }) }).then(() => log('Alarma apagada.')).catch((error) => log(`Error de alarma: ${error.message}`)));

$('preview-start').addEventListener('click', startPreview);
$('preview-stop').addEventListener('click', stopPreview);
$('preview-source').addEventListener('change', () => { if ($('preview').classList.contains('active')) startPreview(); });
$('preview').addEventListener('error', () => { if (!$('preview').getAttribute('src') || previewController) return; $('preview-hint').textContent = 'go2rtc no responde o la fuente RTSP no está disponible.'; });

const faceState = { running: false, loaded: false, ws: null, wsTimer: null, pollTimer: null, lastCaptureTs: 0, history: [] };

function overlayVideoRect() {
  const video = $('preview');
  const stage = $('video-stage');
  if (!video.videoWidth || !video.videoHeight) return null;
  const stageRect = stage.getBoundingClientRect();
  const videoRect = video.getBoundingClientRect();
  const scale = Math.min(videoRect.width / video.videoWidth, videoRect.height / video.videoHeight) || 1;
  const width = video.videoWidth * scale;
  const height = video.videoHeight * scale;
  return { left: videoRect.left - stageRect.left + (videoRect.width - width) / 2, top: videoRect.top - stageRect.top + (videoRect.height - height) / 2, width, height };
}

function syncOverlay() {
  const overlay = $('face-overlay');
  const rect = overlayVideoRect();
  if (!rect) return;
  overlay.style.left = `${rect.left}px`;
  overlay.style.top = `${rect.top}px`;
  overlay.style.width = `${rect.width}px`;
  overlay.style.height = `${rect.height}px`;
  for (const box of overlay.children) {
    const data = box.dataset;
    box.style.left = `${Number(data.x) * rect.width}px`;
    box.style.top = `${Number(data.y) * rect.height}px`;
    box.style.width = `${Number(data.w) * rect.width}px`;
    box.style.height = `${Number(data.h) * rect.height}px`;
  }
}

function renderTick(tick) {
  const overlay = $('face-overlay');
  const boxes = new Map();
  for (const face of tick.faces || []) {
    const key = `${Math.round(face.x * 60)}:${Math.round(face.y * 60)}:${Math.round(face.w * 60)}`;
    boxes.set(key, face);
  }
  for (const box of Array.from(overlay.children)) if (!boxes.has(box.dataset.key)) box.remove();
  for (const [key, face] of boxes) {
    let box = overlay.querySelector(`[data-key="${key}"]`);
    if (!box) {
      box = document.createElement('div');
      box.className = 'face-box';
      box.dataset.key = key;
      const tag = document.createElement('span');
      tag.className = 'face-tag';
      box.appendChild(tag);
      overlay.appendChild(box);
    }
    box.classList.toggle('known', Boolean(face.known));
    box.querySelector('.face-tag').textContent = face.known && face.person ? face.person : `rostro ${(face.score * 100).toFixed(0)}%`;
    box.dataset.x = face.x; box.dataset.y = face.y; box.dataset.w = face.w; box.dataset.h = face.h;
  }
  syncOverlay();
  if (faceState.running) $('face-status').textContent = `${(tick.fps || 0).toFixed(0)} fps · ${(tick.faces || []).length} rostro(s)`;
}

function faceBadge(known, person) {
  const badge = $('face-badge');
  badge.className = `badge ${known ? 'known' : 'unknown'}`;
  badge.textContent = known ? `Conocido · ${person || 'sin nombre'}` : 'Desconocido';
}

function renderCapture(capture) {
  if (!capture || !capture.image || capture.ts === faceState.lastCaptureTs) return;
  faceState.lastCaptureTs = capture.ts;
  $('face-image').src = capture.image;
  faceBadge(capture.known, capture.person);
  $('face-score').textContent = `score ${(capture.score * 100).toFixed(0)}% · ${capture.known ? `match ${(capture.match * 100).toFixed(0)}%` : 'sin coincidencia'} · ${new Date(capture.ts).toLocaleTimeString()} · ${(capture.size / 1024).toFixed(0)} KB`;
  faceState.history = [capture, ...faceState.history].slice(0, 6);
  renderHistory();
}

function renderHistory() {
  $('face-history').innerHTML = faceState.history.map((capture, index) => `<img src="${capture.image}" data-history="${index}" title="${capture.known && capture.person ? capture.person : 'Desconocido'} · ${new Date(capture.ts).toLocaleTimeString()}" />`).join('');
  for (const image of document.querySelectorAll('[data-history]')) image.addEventListener('click', () => openLightbox(faceState.history[Number(image.dataset.history)].image));
}

function openLightbox(src) {
  const lightbox = document.createElement('div');
  lightbox.id = 'face-lightbox';
  lightbox.innerHTML = `<img src="${src}" alt="Captura ampliada" />`;
  lightbox.addEventListener('click', () => lightbox.remove());
  document.body.appendChild(lightbox);
}

async function loadFaces() {
  try { const people = await request('/api/faces'); $('face-people').innerHTML = people.length ? people.map((person) => `<div class="person-row"><span>${person.name}</span><span class="muted small">${person.embeddings} muestra(s)</span></div>`).join('') : 'Sin personas enroladas.'; } catch (error) { $('face-people').textContent = `Error: ${error.message}`; }
}

async function enrollFace() {
  const name = $('face-name').value.trim();
  if (!name) return $('face-enroll-hint').textContent = 'Escribe un nombre antes de enrolar.';
  $('face-enroll').disabled = true;
  try {
    await request('/api/faces/enroll', { method: 'POST', body: JSON.stringify({ name }) });
    $('face-enroll-hint').textContent = `${name} enrolado. Las próximas detecciones lo mostrarán como conocido.`;
    $('face-name').value = '';
    await loadFaces();
  } catch (error) {
    $('face-enroll-hint').textContent = error.message;
  } finally {
    $('face-enroll').disabled = false;
  }
}

function faceSocketUrl() {
  const base = apiBase().replace(/^http/, 'ws');
  return `${base}/api/detections/ws`;
}

function connectFaceSocket() {
  clearInterval(faceState.pollTimer);
  try { faceState.ws?.close(); } catch { }
  let socket;
  try { socket = new WebSocket(faceSocketUrl()); } catch { startFacePolling(); return; }
  faceState.ws = socket;
  socket.onmessage = (event) => {
    const message = JSON.parse(event.data);
    if (message.type === 'tick') renderTick(message);
    if (message.type === 'capture') renderCapture(message);
  };
  socket.onclose = () => { if (faceState.running) { faceState.ws = null; startFacePolling(); } };
  socket.onerror = () => socket.close();
}

function startFacePolling() {
  clearInterval(faceState.pollTimer);
  faceState.pollTimer = setInterval(async () => {
    try { const snapshot = await request('/api/detections'); renderTick(snapshot); renderCapture(snapshot.capture); } catch { }
  }, 400);
}

async function refreshFaceSnapshot() {
  try {
    const snapshot = await request('/api/detections');
    faceState.running = snapshot.running;
    faceState.loaded = snapshot.loaded;
    updateFaceUi();
    if (snapshot.running) { connectFaceSocket(); renderTick(snapshot); renderCapture(snapshot.capture); faceState.history = snapshot.history || []; renderHistory(); }
  } catch { }
}

function updateFaceUi() {
  const running = faceState.running;
  $('face-card').hidden = !running;
  $('face-toggle').textContent = running ? 'Detener detección' : 'Detección';
  $('face-toggle').classList.toggle('primary', !running);
  $('face-toggle').classList.toggle('secondary', running);
  $('face-toggle-2').textContent = running ? 'Detener' : 'Iniciar';
  $('face-status').textContent = running ? 'Conectando…' : 'Detenido';
  $('face-overlay').classList.toggle('active', running);
}

async function toggleDetections() {
  try {
    const result = await request('/api/detections/toggle', { method: 'POST' });
    faceState.running = result.running;
    updateFaceUi();
    if (result.running) { connectFaceSocket(); refreshFaceSnapshot(); } else { clearInterval(faceState.pollTimer); faceState.ws?.close(); faceState.ws = null; $('face-overlay').innerHTML = ''; }
  } catch (error) {
    log(`Error de detección: ${error.message}`);
    updateFaceUi();
  }
}

function toggleFullscreen() {
  const stage = $('video-stage');
  if (document.fullscreenElement) document.exitFullscreen();
  else if (stage.requestFullscreen) stage.requestFullscreen();
  else if (stage.webkitRequestFullscreen) stage.webkitRequestFullscreen();
}

$('face-toggle').addEventListener('click', toggleDetections);
$('face-toggle-2').addEventListener('click', toggleDetections);
$('face-enroll').addEventListener('click', enrollFace);
$('face-name').addEventListener('keydown', (event) => { if (event.key === 'Enter') enrollFace(); });
$('preview-fullscreen').addEventListener('click', toggleFullscreen);
window.addEventListener('resize', syncOverlay);
document.addEventListener('fullscreenchange', syncOverlay);

loadFaces();

loadConfig(); refreshOperator();
request('/api/preview').then((preview) => log(preview.ready ? `Preview listo: ${preview.api} (src ${preview.sub} / ${preview.main})` : `Preview no disponible: ${preview.error}`)).catch(() => {});
refreshFaceSnapshot();
loadZoneSnapshot();
