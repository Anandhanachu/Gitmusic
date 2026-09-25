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
  sessionId:        null,   // active session ID string
  username:         null,   // current GitHub username
  deviceStatus:     'DISCONNECTED',  // AVAILABLE | BUSY | DISCONNECTED
  ws:               null,
  wsReconnectTimer: null,
  wsReconnectCount: 0,
  pingTimer:        null,
  isAnimating:      false,  // niyamax-style reveal animation active
  currentRequestId: 0,      // stale request cancellation
  // Synchronized Music & Timeline state
  musicState:       'IDLE', // IDLE | REVEALING | PLAYING | STOPPED
  levels:           null,   // 52x7 array
  timeline:         null,   // { tempo, bpm, duration_ms, events }
  audioUrl:         null,
  animFrameId:      null,
  playbackStartMs:  0,
  activeCell:       null,
};

// ── DOM references ─────────────────────────────────────────────────────────

const $ = id => document.getElementById(id);

const dom = {
  bannerDot:       $('banner-dot'),
  bannerText:      $('banner-text'),
  deviceBanner:    $('device-banner'),

  connectPanel:    $('connect-panel'),
  sessionPanel:    $('session-panel'),

  usernameInput:   $('username-input'),
  connectBtn:      $('connect-btn'),
  connectError:    $('connect-error'),

  ghAvatar:        $('gh-avatar'),
  ghUsernameLabel: $('gh-username-label'),
  sessionIdLabel:  $('session-id-label'),
  espDot:          $('esp-dot'),
  espStatusText:   $('esp-status-text'),
  disconnectBtn:   $('disconnect-btn'),
  backSearchBtn:   $('back-search-btn'),
  sessionError:    $('session-error'),

  valStreak:       $('val-streak'),
  valLongest:      $('val-longest'),
  valToday:        $('val-today'),
  valTotal:        $('val-total'),
  valWeekly:       $('val-weekly'),
  valMonthly:      $('val-monthly'),

  // Synchronized Music Controls
  musicToggleBtn:  $('music-toggle-btn'),
  musicBtnIcon:    $('music-btn-icon'),
  musicBtnText:    $('music-btn-text'),
  musicBtnSpinner: $('music-btn-spinner'),
  musicStateBadge: $('music-state-badge'),
  musicStatusText: $('music-status-text'),
  contribGrid:     $('contrib-grid'),
  timelineProgress:$('timeline-progress'),
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

    case 'music_ready':
      console.log('[WS] ESP32 reported music_ready');
      if (state.musicState !== 'PLAYING') {
        setMusicState('IDLE', 'Ready to play on ESP32 speaker');
      }
      break;

    case 'music_start':
      console.log('[WS] music_start received');
      if (state.musicState !== 'PLAYING') {
        startSynchronizedPlayback();
      }
      break;

    case 'music_stop':
      console.log('[WS] music_stop received');
      if (state.musicState === 'PLAYING') {
        stopSynchronizedPlayback();
      }
      break;

    case 'music_loop':
      console.log('[WS] music_loop received');
      handlePlaybackLoop();
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

  const requestId = ++state.currentRequestId;
  state.isAnimating = true;
  state.levels = null;
  state.timeline = null;

  // Immediately enter animated placeholder reveal state (Stage 1)
  dom.connectPanel.classList.add('hidden');
  dom.sessionPanel.classList.remove('hidden');
  dom.ghUsernameLabel.textContent = username;
  dom.ghAvatar.src = `https://github.com/${username}.png?size=56`;
  dom.ghAvatar.alt = `${username} avatar`;
  dom.sessionIdLabel.textContent = '–';

  // Reset stat values to placeholders during reveal
  setStatValue(dom.valStreak,  '–');
  setStatValue(dom.valLongest, '–');
  setStatValue(dom.valToday,   '–');
  setStatValue(dom.valTotal,   '–');
  setStatValue(dom.valWeekly,  '–');
  setStatValue(dom.valMonthly, '–');

  // Disable music controls during reveal
  if (dom.musicToggleBtn) dom.musicToggleBtn.disabled = true;
  setMusicState('REVEALING', 'Transforming GitHub activity into GitMusic…');

  // Render the animated placeholder grid
  renderPlaceholderGrid();

  // Enforce 3000 ms reveal animation timing in parallel with fetch
  const animationTimer = new Promise(resolve => setTimeout(resolve, 3000));

  let data = null;
  let fetchError = null;

  try {
    const res = await fetch(`${BASE_URL}/api/connect`, {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ username }),
    });

    data = await res.json();
    if (!res.ok || !data.success) {
      fetchError = data.detail || data.message || 'User not found.';
    }
  } catch (err) {
    console.error('Connect error:', err);
    fetchError = 'Unable to reach the backend. Is it running?';
  }

  // Await the ~3-second reveal animation
  await animationTimer;

  // Ignore stale request if a newer search was initiated
  if (requestId !== state.currentRequestId) return;

  state.isAnimating = false;

  if (fetchError) {
    // Stop cleanly and show error on connect panel; do not reveal fake grid
    showConnectPanel();
    showConnectError(`✗ ${fetchError}`);
    return;
  }

  // Success: reveal authentic contribution graph and session
  state.sessionId = data.session_id;
  state.username  = data.username;

  if (state.ws && state.ws.readyState === WebSocket.OPEN) {
    state.ws.send(JSON.stringify({ type: 'register_session', session_id: state.sessionId }));
  }

  populateSessionPanel(data);

  // If user has zero contributions, display notice
  const totalContribs = data.stats ? data.stats.total_contributions : 0;
  if (totalContribs === 0) {
    setMusicState('IDLE', '⚠ No contributions found for this user in the past year');
  } else {
    setMusicState('IDLE', 'Ready to play on ESP32 speaker');
  }

  if (dom.musicToggleBtn) dom.musicToggleBtn.disabled = false;
}

// ── Disconnect flow ────────────────────────────────────────────────────────

async function handleDisconnect() {
  if (dom.disconnectBtn) dom.disconnectBtn.disabled = true;
  if (dom.backSearchBtn) dom.backSearchBtn.disabled = true;

  if (state.sessionId) {
    try {
      await fetch(`${BASE_URL}/api/disconnect`, {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ session_id: state.sessionId }),
      });
    } catch (err) {
      console.error('Disconnect error:', err);
    }
  }

  if (dom.disconnectBtn) dom.disconnectBtn.disabled = false;
  if (dom.backSearchBtn) dom.backSearchBtn.disabled = false;

  clearSessionState();
  showConnectPanel();
}

// ── Session panel ─────────────────────────────────────────────────────────

function populateSessionPanel(data) {
  const stats = data.stats || {};
  const u = data.username;

  state.currentStreak = stats.current_streak ?? 0;
  state.levels = data.levels || null;
  state.timeline = data.timeline || null;
  state.audioUrl = data.audio_url || (state.sessionId ? `/api/music/${state.sessionId}/audio.wav` : null);

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

  // Render authentic 52x7 contribution grid
  renderContributionGrid(state.levels);

  setMusicState('IDLE', 'Ready to play on ESP32 speaker');
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
  state.currentRequestId++;
  state.isAnimating = false;
  stopSynchronizedPlayback();
  state.sessionId = null;
  state.username  = null;
  state.levels    = null;
  state.timeline  = null;
  state.audioUrl  = null;
  setMusicState('IDLE', 'Ready to play');
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

// ── Contribution Grid Rendering ──────────────────────────────────────────

function renderPlaceholderGrid() {
  if (!dom.contribGrid) return;
  dom.contribGrid.innerHTML = '';
  dom.contribGrid.className = 'contrib-grid loading';

  for (let col = 0; col < 52; col++) {
    const colMod = col % 3;
    for (let row = 0; row < 7; row++) {
      const cell = document.createElement('div');
      cell.className = 'contrib-cell day-cell';
      const cellIndex = col * 7 + row;
      cell.style.setProperty('--cell-index', cellIndex);
      cell.dataset.week = col;
      cell.dataset.day = row;
      cell.dataset.weekMod = colMod;
      dom.contribGrid.appendChild(cell);
    }
  }
}

function renderContributionGrid(levels) {
  if (!dom.contribGrid) return;
  dom.contribGrid.innerHTML = '';
  dom.contribGrid.className = 'contrib-grid';

  // 52 weeks (cols), 7 days (rows)
  for (let col = 0; col < 52; col++) {
    for (let row = 0; row < 7; row++) {
      let lvl = 0;
      if (Array.isArray(levels) && levels[col] && levels[col][row] !== undefined) {
        lvl = levels[col][row];
      }
      const cell = document.createElement('div');
      cell.className = `contrib-cell lvl-${lvl}`;
      cell.dataset.week = col;
      cell.dataset.day = row;
      cell.title = `Week ${col + 1}, Day ${row + 1}: Level ${lvl}`;
      dom.contribGrid.appendChild(cell);
    }
  }
}

// ── Music Playback State Machine ──────────────────────────────────────────

function setMusicState(newState, statusText = '') {
  state.musicState = newState;

  if (dom.musicStateBadge) {
    dom.musicStateBadge.className = 'music-state-badge';
    dom.musicStateBadge.textContent = newState;
    dom.musicStateBadge.classList.add(`badge-${newState.toLowerCase()}`);
  }

  if (dom.musicStatusText && statusText) {
    dom.musicStatusText.textContent = statusText;
  }

  if (!dom.musicToggleBtn) return;

  if (newState === 'REVEALING') {
    dom.musicToggleBtn.disabled = true;
    dom.musicToggleBtn.classList.remove('is-playing');
    dom.musicBtnSpinner.classList.add('hidden');
    dom.musicBtnIcon.innerHTML = `
      <svg class="icon-play" viewBox="0 0 24 24" fill="currentColor">
        <path d="M8 5v14l11-7z"/>
      </svg>
    `;
    dom.musicBtnText.textContent = 'Play';
  } else if (newState === 'PLAYING') {
    dom.musicToggleBtn.disabled = false;
    dom.musicToggleBtn.classList.add('is-playing');
    dom.musicBtnSpinner.classList.add('hidden');
    dom.musicBtnIcon.innerHTML = `
      <svg class="icon-stop" viewBox="0 0 24 24" fill="currentColor">
        <path d="M6 6h12v12H6z"/>
      </svg>
    `;
    dom.musicBtnText.textContent = 'Stop';
  } else {
    // IDLE / STOPPED / READY
    dom.musicToggleBtn.disabled = state.isAnimating;
    dom.musicToggleBtn.classList.remove('is-playing');
    dom.musicBtnSpinner.classList.add('hidden');
    dom.musicBtnIcon.innerHTML = `
      <svg class="icon-play" viewBox="0 0 24 24" fill="currentColor">
        <path d="M8 5v14l11-7z"/>
      </svg>
    `;
    dom.musicBtnText.textContent = 'Play';
  }
}

// ── Music Button Click Handler (Instant Play <-> Stop Toggle) ────────────

function handleMusicToggle() {
  if (state.isAnimating) return; // Locked during reveal animation

  if (state.deviceStatus === 'DISCONNECTED') {
    if (dom.musicStatusText) {
      dom.musicStatusText.textContent = 'ESP32 device is not connected';
    }
    return;
  }

  if (state.musicState === 'PLAYING') {
    // Currently playing -> toggle immediately to STOP
    triggerStopCommand();
  } else {
    // Currently idle/stopped -> toggle immediately to PLAY
    triggerPlayCommand();
  }
}

function triggerPlayCommand() {
  // Start synchronized visual animation immediately and switch button to Stop
  startSynchronizedPlayback();

  // Send play command to backend and ESP32
  if (state.ws && state.ws.readyState === WebSocket.OPEN) {
    state.ws.send(JSON.stringify({ type: 'music_play' }));
  }
  fetch(`${BASE_URL}/api/music/play`, { method: 'POST' }).catch(() => {});
}

function triggerStopCommand() {
  // Stop synchronized visual animation immediately and switch button to Play
  stopSynchronizedPlayback();

  // Send stop command to backend and ESP32
  if (state.ws && state.ws.readyState === WebSocket.OPEN) {
    state.ws.send(JSON.stringify({ type: 'music_stop' }));
  }
  fetch(`${BASE_URL}/api/music/stop`, { method: 'POST' }).catch(() => {});
}

// ── Synchronized Playback Engine (Website is visual-only, Sound only on ESP32) ──

function startSynchronizedPlayback() {
  if (state.musicState === 'PLAYING') return;

  setMusicState('PLAYING', '▶ Playing melody on ESP32 speaker…');
  clearCellHighlights();

  state.playbackStartMs = performance.now();

  // Visual synchronization loop (NO audio from browser)
  if (state.animFrameId) cancelAnimationFrame(state.animFrameId);
  state.animFrameId = requestAnimationFrame(syncAnimationLoop);
}

function stopSynchronizedPlayback() {
  if (state.animFrameId) {
    cancelAnimationFrame(state.animFrameId);
    state.animFrameId = null;
  }

  clearCellHighlights();

  if (dom.timelineProgress) {
    dom.timelineProgress.style.width = '0%';
  }

  setMusicState('IDLE', '⏹ Stopped');
}

function handlePlaybackLoop() {
  if (state.musicState !== 'PLAYING') return;
  console.log('[Loop] Seamlessly continuing synchronized timeline loop');
  clearCellHighlights();
  state.playbackStartMs = performance.now();
}

function clearCellHighlights() {
  if (state.activeCell) {
    state.activeCell.classList.remove('cell-highlight', 'cell-trail');
    state.activeCell = null;
  }
  document.querySelectorAll('.cell-highlight, .cell-trail').forEach(el => {
    el.classList.remove('cell-highlight', 'cell-trail');
  });
}

function syncAnimationLoop() {
  if (state.musicState !== 'PLAYING') return;

  const durationMs = state.timeline ? state.timeline.duration_ms : 29538;
  const currentMs = performance.now() - state.playbackStartMs;

  // Loop calculation
  const loopCurrentMs = currentMs % durationMs;

  // Update scrubber progress bar
  if (dom.timelineProgress) {
    const pct = Math.min(100, Math.max(0, (loopCurrentMs / durationMs) * 100));
    dom.timelineProgress.style.width = `${pct.toFixed(1)}%`;
  }

  // Find active musical event in timeline and illuminate corresponding cell
  if (state.timeline && Array.isArray(state.timeline.events)) {
    const events = state.timeline.events;
    let currentEvent = null;

    for (let i = 0; i < events.length; i++) {
      const ev = events[i];
      if (loopCurrentMs >= ev.time && loopCurrentMs < (ev.time + ev.duration)) {
        currentEvent = ev;
        break;
      }
    }

    if (currentEvent) {
      const targetCell = dom.contribGrid.querySelector(
        `[data-week="${currentEvent.week}"][data-day="${currentEvent.day}"]`
      );

      if (targetCell && targetCell !== state.activeCell) {
        if (state.activeCell) {
          state.activeCell.classList.remove('cell-highlight');
          state.activeCell.classList.add('cell-trail');
          const prev = state.activeCell;
          setTimeout(() => prev.classList.remove('cell-trail'), 400);
        }
        targetCell.classList.add('cell-highlight');
        state.activeCell = targetCell;
      }
    } else if (state.activeCell) {
      state.activeCell.classList.remove('cell-highlight');
      state.activeCell.classList.add('cell-trail');
      const prev = state.activeCell;
      setTimeout(() => prev.classList.remove('cell-trail'), 400);
      state.activeCell = null;
    }
  }

  state.animFrameId = requestAnimationFrame(syncAnimationLoop);
}

// ── Event listeners ────────────────────────────────────────────────────────

dom.connectBtn.addEventListener('click', handleConnect);

dom.usernameInput.addEventListener('keydown', e => {
  if (e.key === 'Enter') handleConnect();
  if (!dom.connectError.classList.contains('hidden')) {
    hideError(dom.connectError);
  }
});

dom.disconnectBtn.addEventListener('click', handleDisconnect);

if (dom.backSearchBtn) {
  dom.backSearchBtn.addEventListener('click', handleDisconnect);
}

window.addEventListener('keydown', e => {
  if (e.key === 'Escape' && dom.sessionPanel && !dom.sessionPanel.classList.contains('hidden')) {
    handleDisconnect();
  }
});

if (dom.musicToggleBtn) {
  dom.musicToggleBtn.addEventListener('click', handleMusicToggle);
}

// ── Init ───────────────────────────────────────────────────────────────────

async function fetchInitialDeviceStatus() {
  try {
    const res = await fetch(`${BASE_URL}/api/device/status`);
    if (res.ok) {
      const data = await res.json();
      applyDeviceStatus(data.status, data.current_user);
    }
  } catch { /* WS will update us anyway */ }
}

connectWebSocket();
fetchInitialDeviceStatus();

dom.usernameInput.focus();
