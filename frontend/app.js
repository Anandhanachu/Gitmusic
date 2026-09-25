/**
 * app.js – GitMusic frontend application
 *
 * Responsibilities:
 *  - Maintain a WebSocket connection to the backend (/ws/client)
 *  - Handle connect / disconnect REST calls (/api/connect, /api/disconnect)
 *  - Update UI based on real-time device status broadcasts
 *  - Keep session state in memory only (no localStorage of sensitive data)
 */

'use strict';

// ── Configuration ─────────────────────────────────────────────────────────

const BASE_URL    = window.location.origin;                        // e.g. http://localhost:8000
const WS_PROTOCOL = window.location.protocol === 'https:' ? 'wss' : 'ws';
const WS_URL      = `${WS_PROTOCOL}://${window.location.host}/ws/client`;

const RECONNECT_DELAY_MS    = 3000;   // WS reconnect interval
const MAX_RECONNECT_ATTEMPTS = 10;
const PING_INTERVAL_MS       = 25000; // keep-alive ping

// ── State ─────────────────────────────────────────────────────────────────

const state = {
  sessionId:       null,   // active session ID string
  username:        null,   // current GitHub username
  deviceStatus:    'DISCONNECTED',  // AVAILABLE | BUSY | DISCONNECTED
  ws:              null,
  wsReconnectTimer: null,
  wsReconnectCount: 0,
  pingTimer:       null,
};

// ── DOM references ─────────────────────────────────────────────────────────

const $ = id => document.getElementById(id);

const dom = {
  bannerDot:      $('banner-dot'),
  bannerText:     $('banner-text'),
  deviceBanner:   $('device-banner'),

  connectPanel:   $('connect-panel'),
  sessionPanel:   $('session-panel'),

  usernameInput:  $('username-input'),
  connectBtn:     $('connect-btn'),
  connectError:   $('connect-error'),

  ghAvatar:       $('gh-avatar'),
  ghUsernameLabel:$('gh-username-label'),
  sessionIdLabel: $('session-id-label'),
  espDot:         $('esp-dot'),
  espStatusText:  $('esp-status-text'),
  disconnectBtn:  $('disconnect-btn'),
  sessionError:   $('session-error'),

  valStreak:      $('val-streak'),
  valLongest:     $('val-longest'),
  valToday:       $('val-today'),
  valTotal:       $('val-total'),
  valWeekly:      $('val-weekly'),
  valMonthly:     $('val-monthly'),

  playBtn:        $('play-btn'),
  stopBtn:        $('stop-btn'),
  musicStatus:    $('music-status'),
};

// ── WebSocket ──────────────────────────────────────────────────────────────

function connectWebSocket() {
  if (state.ws && (state.ws.readyState === WebSocket.OPEN || state.ws.readyState === WebSocket.CONNECTING)) {
    return; // already connected or connecting
  }

  setBannerConnecting();

  try {
    state.ws = new WebSocket(WS_URL);
  } catch (err) {
    console.error('WS create failed:', err);
    scheduleWsReconnect();
    return;
  }

  state.ws.addEventListener('open', () => {
    console.log('[WS] Connected to backend.');
    state.wsReconnectCount = 0;

    // If we have an active session, register it
    if (state.sessionId) {
      state.ws.send(JSON.stringify({ type: 'register_session', session_id: state.sessionId }));
    }

    startPing();
  });

  state.ws.addEventListener('message', evt => {
    let msg;
    try { msg = JSON.parse(evt.data); } catch { return; }
    handleServerMessage(msg);
  });

  state.ws.addEventListener('close', evt => {
    console.warn('[WS] Closed:', evt.code, evt.reason);
    stopPing();
    scheduleWsReconnect();
  });

  state.ws.addEventListener('error', () => {
    console.error('[WS] Error – will attempt reconnect.');
  });
}

function scheduleWsReconnect() {
  if (state.wsReconnectTimer) return; // already scheduled
  if (state.wsReconnectCount >= MAX_RECONNECT_ATTEMPTS) {
    setBannerDisconnected('Server unreachable. Refresh the page to retry.');
    return;
  }

  state.wsReconnectCount++;
  setBannerConnecting();

  state.wsReconnectTimer = setTimeout(() => {
    state.wsReconnectTimer = null;
    connectWebSocket();
  }, RECONNECT_DELAY_MS * Math.min(state.wsReconnectCount, 4));
}

function startPing() {
  stopPing();
  state.pingTimer = setInterval(() => {
    if (state.ws && state.ws.readyState === WebSocket.OPEN) {
      state.ws.send(JSON.stringify({ type: 'client_ping' }));
    }
  }, PING_INTERVAL_MS);
}

function stopPing() {
  if (state.pingTimer) { clearInterval(state.pingTimer); state.pingTimer = null; }
}

// ── Message handler ────────────────────────────────────────────────────────

function handleServerMessage(msg) {
  switch (msg.type) {
    case 'device_status':
      applyDeviceStatus(msg.status, msg.username);
      break;

    case 'session_timeout':
      showSessionError(msg.message || 'Your session expired. The device has been released.');
      clearSessionState();
      showConnectPanel();
      break;

    case 'error':
      showConnectError(msg.message || 'Server error.');
      break;

    default:
      // ignore unknown message types
  }
}

// ── Device status ─────────────────────────────────────────────────────────

function applyDeviceStatus(status, username = null) {
  state.deviceStatus = status;

  // Update banner
  dom.deviceBanner.className = 'device-banner';

  if (status === 'AVAILABLE') {
    dom.deviceBanner.classList.add('banner--available');
    setDotClass(dom.bannerDot, 'dot--available');
    dom.bannerText.textContent = 'GitMusic device is available';
    dom.connectBtn.disabled = false;

  } else if (status === 'BUSY') {
    dom.deviceBanner.classList.add('banner--busy');
    setDotClass(dom.bannerDot, 'dot--busy');
    const who = username || 'another user';
    dom.bannerText.textContent = `GitMusic device is in use by ${who}`;

    // Only disable connect button if WE don't own the session
    if (!state.sessionId) {
      dom.connectBtn.disabled = true;
    }

  } else {
    dom.deviceBanner.classList.add('banner--disconnected');
    setDotClass(dom.bannerDot, 'dot--disconnected');
    dom.bannerText.textContent = 'GitMusic device is disconnected';
    dom.connectBtn.disabled = true;
  }

  // Update ESP32 status in the session panel
  if (dom.sessionPanel && !dom.sessionPanel.classList.contains('hidden')) {
    updateEspStatus(status);
  }
}

function updateEspStatus(status) {
  if (status === 'DISCONNECTED') {
    setDotClass(dom.espDot, 'dot--disconnected');
    dom.espStatusText.textContent = 'Disconnected';
  } else if (status === 'BUSY') {
    setDotClass(dom.espDot, 'dot--busy');
    dom.espStatusText.textContent = 'Connected – Active';
  } else {
    setDotClass(dom.espDot, 'dot--available');
    dom.espStatusText.textContent = 'Connected';
  }
}

// ── Connect flow ───────────────────────────────────────────────────────────

async function handleConnect() {
  const username = dom.usernameInput.value.trim();

  if (!username) {
    showConnectError('Please enter a GitHub username.');
    return;
  }

  if (!/^[a-zA-Z0-9](?:[a-zA-Z0-9-]{0,37}[a-zA-Z0-9])?$/.test(username)) {
    showConnectError('Invalid GitHub username format.');
    return;
  }

  hideError(dom.connectError);
  setBtnLoading(dom.connectBtn, true);
  dom.usernameInput.disabled = true;

  try {
    const res = await fetch(`${BASE_URL}/api/connect`, {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ username }),
    });

    const data = await res.json();

    if (!res.ok || !data.success) {
      const err = data.detail || data.message || 'Connection failed. Please try again.';
      showConnectError(err);
      return;
    }

    // Success
    state.sessionId = data.session_id;
    state.username  = data.username;

    // Register session with WS so the backend can target us
    if (state.ws && state.ws.readyState === WebSocket.OPEN) {
      state.ws.send(JSON.stringify({ type: 'register_session', session_id: state.sessionId }));
    }

    populateSessionPanel(data);
    showSessionPanel();

  } catch (err) {
    console.error('Connect error:', err);
    showConnectError('Unable to reach the backend. Is it running?');
  } finally {
    setBtnLoading(dom.connectBtn, false);
    dom.usernameInput.disabled = false;
  }
}

// ── Disconnect flow ────────────────────────────────────────────────────────

async function handleDisconnect() {
  if (!state.sessionId) return;

  dom.disconnectBtn.disabled = true;

  try {
    await fetch(`${BASE_URL}/api/disconnect`, {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ session_id: state.sessionId }),
    });
  } catch (err) {
    console.error('Disconnect error:', err);
  }

  clearSessionState();
  showConnectPanel();
}

// ── Session panel ─────────────────────────────────────────────────────────

function populateSessionPanel(data) {
  const stats = data.stats || {};
  const u = data.username;

  state.currentStreak = stats.current_streak ?? 0;

  dom.ghAvatar.src = `https://github.com/${u}.png?size=56`;
  dom.ghAvatar.alt = `${u} GitHub avatar`;
  dom.ghUsernameLabel.textContent = u;
  dom.sessionIdLabel.textContent = state.sessionId
    ? `${state.sessionId.slice(0, 8)}…`
    : '–';

  setStatValue(dom.valStreak,  stats.current_streak  ?? '–', 'days');
  setStatValue(dom.valLongest, stats.longest_streak  ?? '–', 'days');
  setStatValue(dom.valToday,   stats.today_contributions ?? '–');
  setStatValue(dom.valTotal,   stats.total_contributions ?? '–');
  setStatValue(dom.valWeekly,  stats.weekly_contributions ?? '–');
  setStatValue(dom.valMonthly, stats.monthly_contributions ?? '–');

  updateEspStatus(state.deviceStatus);
}

function setStatValue(el, val, suffix = '') {
  const display = val !== '–' && suffix ? `${val}` : `${val}`;
  el.textContent = display;
  el.classList.add('animating');
  el.addEventListener('animationend', () => el.classList.remove('animating'), { once: true });
}

function showSessionPanel() {
  dom.connectPanel.classList.add('hidden');
  dom.sessionPanel.classList.remove('hidden');
  dom.disconnectBtn.disabled = false;
  hideError(dom.sessionError);
}

function showConnectPanel() {
  dom.sessionPanel.classList.add('hidden');
  dom.connectPanel.classList.remove('hidden');
  dom.usernameInput.value = '';
  dom.usernameInput.focus();
}

function clearSessionState() {
  state.sessionId = null;
  state.username  = null;
  if (dom.playBtn) dom.playBtn.classList.remove('playing');
  if (dom.stopBtn) dom.stopBtn.disabled = true;
  if (dom.musicStatus) dom.musicStatus.textContent = 'Ready to play melody';
}

// ── Banner states ──────────────────────────────────────────────────────────

function setBannerConnecting() {
  dom.deviceBanner.className = 'device-banner';
  setDotClass(dom.bannerDot, 'dot--connecting');
  dom.bannerText.textContent = 'Connecting to server…';
}

function setBannerDisconnected(msg) {
  dom.deviceBanner.className = 'device-banner banner--disconnected';
  setDotClass(dom.bannerDot, 'dot--disconnected');
  dom.bannerText.textContent = msg || 'Server disconnected.';
}

// ── Error helpers ─────────────────────────────────────────────────────────

function showConnectError(msg) {
  dom.connectError.textContent = msg;
  dom.connectError.classList.remove('hidden');
}

function showSessionError(msg) {
  dom.sessionError.textContent = msg;
  dom.sessionError.classList.remove('hidden');
}

function hideError(el) {
  el.classList.add('hidden');
  el.textContent = '';
}

// ── UI helpers ─────────────────────────────────────────────────────────────

function setBtnLoading(btn, loading) {
  btn.disabled = loading;
  btn.classList.toggle('loading', loading);
}

function setDotClass(dot, cls) {
  dot.className = dot.className.replace(/dot--\w+/g, '').trim() + ' ' + cls;
}

// ── ESP32 Hardware Music Controls (PLAY & STOP) ─────────────────────────

function handlePlay() {
  if (dom.playBtn && dom.playBtn.classList.contains('playing')) return;

  // Send simple WebSocket command to ESP32: {"type":"play"}
  if (state.ws && state.ws.readyState === WebSocket.OPEN) {
    state.ws.send(JSON.stringify({ type: 'play' }));
  }
  // REST fallback
  fetch(`${BASE_URL}/api/play`, { method: 'POST' }).catch(() => {});

  if (dom.playBtn) dom.playBtn.classList.add('playing');
  if (dom.stopBtn) dom.stopBtn.disabled = false;
  if (dom.musicStatus) dom.musicStatus.textContent = '▶ Playing melody locally on ESP32 speaker…';
}

function handleStop() {
  // Send simple WebSocket command to ESP32: {"type":"stop"}
  if (state.ws && state.ws.readyState === WebSocket.OPEN) {
    state.ws.send(JSON.stringify({ type: 'stop' }));
  }
  // REST fallback
  fetch(`${BASE_URL}/api/stop`, { method: 'POST' }).catch(() => {});

  if (dom.playBtn) dom.playBtn.classList.remove('playing');
  if (dom.stopBtn) dom.stopBtn.disabled = true;
  if (dom.musicStatus) dom.musicStatus.textContent = '⏹ Stopped (Press PLAY to restart from beginning)';
}

// ── Event listeners ────────────────────────────────────────────────────────

dom.connectBtn.addEventListener('click', handleConnect);

dom.usernameInput.addEventListener('keydown', e => {
  if (e.key === 'Enter') handleConnect();
  // Hide error as user types
  if (!dom.connectError.classList.contains('hidden')) {
    hideError(dom.connectError);
  }
});

dom.disconnectBtn.addEventListener('click', handleDisconnect);

if (dom.playBtn) {
  dom.playBtn.addEventListener('click', handlePlay);
}

if (dom.stopBtn) {
  dom.stopBtn.addEventListener('click', handleStop);
}

// ── Init ───────────────────────────────────────────────────────────────────

// Poll device status once on load as a fallback
async function fetchInitialDeviceStatus() {
  try {
    const res = await fetch(`${BASE_URL}/api/device/status`);
    if (res.ok) {
      const data = await res.json();
      applyDeviceStatus(data.status, data.current_user);
    }
  } catch { /* WS will update us anyway */ }
}

// Start WS connection immediately
connectWebSocket();
fetchInitialDeviceStatus();

// Focus the username input on load
dom.usernameInput.focus();
