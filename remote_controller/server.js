require('dotenv').config();

const fs = require('fs');
const path = require('path');
const os = require('os');
const express = require('express');
const { WebSocketServer } = require('ws');
const osc = require('osc');

const HTTP_PORT = Number(process.env.HTTP_PORT || 8080);
const TD_HOST = process.env.TD_HOST || '127.0.0.1';
const TD_OSC_SEND_PORT = Number(process.env.TD_OSC_SEND_PORT || 9001);
const TD_OSC_LISTEN_PORT = Number(process.env.TD_OSC_LISTEN_PORT || 9002);
const HEARTBEAT_TIMEOUT_MS = Number(process.env.HEARTBEAT_TIMEOUT_MS || 3000);
const PLAYER_ACTIVATE_STAGGER_MS = Number(process.env.PLAYER_ACTIVATE_STAGGER_MS || 150);
const ADMIN_HOSTNAME = (process.env.ADMIN_HOSTNAME || 'admin.gsxnetworkpuzzle.com').toLowerCase();

if (HTTP_PORT === 9000) {
  console.error('HTTP_PORT cannot be 9000 -- that port is reserved for ESP32 board OSC telemetry.');
  process.exit(1);
}

// ---- App state -------------------------------------------------------

const GAME_STATE_NAMES = {
  1: 'Idle',
  2: 'Three',
  3: 'Two',
  4: 'One',
  5: 'Start',
  6: 'Gameplay',
  7: 'Results',
};

const state = {
  players: { 1: false, 2: false, 3: false },
  connected: false,
  gameState: null,
  waveshare: { 1: false, 2: false, 3: false, 4: false, 5: false, 6: false },
  // Per-board resistor-classification detail (admin status page only -- see
  // ADMIN_HOSTNAME below). Populated from /remote/waveshare_detail/<n>, which
  // TD sends as 3 raw index values (op.PROJECT.par.Index<board><q>) -- just
  // the 0-10 resistor classification per channel, no match/correctness data.
  waveshareDetail: { 1: null, 2: null, 3: null, 4: null, 5: null, 6: null },
  // True if any socket on any board reads other than NONE (10) -- i.e. a
  // patch cord is still plugged in somewhere, most likely left over from the
  // previous game. Blocks Start Game (see the pulse handler below) so a
  // docent can't accidentally start a new round with stale connections still
  // in place. Not admin-gated: it's presence-only, doesn't reveal any actual
  // reading or the answer key, so it's safe on the public state broadcast.
  anyConnected: false,
};

const EMPTY_INDEX = 10;

function boardHasConnection(detail) {
  return Boolean(detail) && [detail.ch1Index, detail.ch2Index, detail.ch3Index].some((idx) => idx !== EMPTY_INDEX);
}

function computeAnyConnected() {
  return Object.values(state.waveshareDetail).some(boardHasConnection);
}

const ANSWER_TABLE_PATH = path.join(__dirname, '..', 'answer_table.tsv');

// Per-board (1-6) array of the 3 correct resistor indices for that board's
// CH1/CH2/CH3, loaded once at launch from answer_table.tsv (repo root -- the
// same file the ESP32 boards' SD cards read from). Board->question mapping
// matches question_game/CLAUDE.md Section 6D: board 1 = panel 1 questions
// 1-3, board 2 = panel 1 questions 4-6, board 3 = panel 2 questions 1-3, etc.
function loadAnswerIndex() {
  const result = {};
  let raw;
  try {
    raw = fs.readFileSync(ANSWER_TABLE_PATH, 'utf8');
  } catch (err) {
    console.error(`Could not read answer_table.tsv at ${ANSWER_TABLE_PATH}: ${err.message}`);
    console.error('Admin status page will show readings without a correct-answer comparison.');
    return result;
  }

  const byPanelQuestion = {};
  const rows = raw.split(/\r?\n/).map((line) => line.trim()).filter(Boolean);
  for (const row of rows.slice(1)) { // skip header row
    const cols = row.split('\t');
    const questionMatch = /^P(\d+)q(\d+)$/.exec(cols[0] || '');
    const index = Number(cols[3]);
    if (!questionMatch || Number.isNaN(index)) continue;
    const panel = Number(questionMatch[1]);
    const question = Number(questionMatch[2]);
    byPanelQuestion[panel] = byPanelQuestion[panel] || {};
    byPanelQuestion[panel][question] = index;
  }

  for (let board = 1; board <= 6; board++) {
    const panel = Math.ceil(board / 2);
    const startQuestion = ((board - 1) % 2) * 3 + 1;
    const questions = byPanelQuestion[panel] || {};
    const channels = [questions[startQuestion], questions[startQuestion + 1], questions[startQuestion + 2]];
    if (channels.every((v) => v !== undefined)) {
      result[board] = channels;
    } else {
      console.error(`answer_table.tsv missing rows for board ${board} (panel ${panel}, questions ${startQuestion}-${startQuestion + 2}).`);
    }
  }
  return result;
}

const answerIndex = loadAnswerIndex();

function isAdminHost(hostHeader) {
  const host = (hostHeader || '').split(':')[0].toLowerCase();
  return host === ADMIN_HOSTNAME;
}

// ---- OSC link to TouchDesigner ---------------------------------------

const oscPort = new osc.UDPPort({
  localAddress: '0.0.0.0',
  localPort: TD_OSC_LISTEN_PORT,
  remoteAddress: TD_HOST,
  remotePort: TD_OSC_SEND_PORT,
  metadata: true,
});

let heartbeatTimer = null;

function markConnected(isConnected) {
  if (state.connected === isConnected) return;
  state.connected = isConnected;
  broadcastState();
}

function armHeartbeatTimeout() {
  if (heartbeatTimer) clearTimeout(heartbeatTimer);
  heartbeatTimer = setTimeout(() => markConnected(false), HEARTBEAT_TIMEOUT_MS);
}

function oscArgValue(msg, index) {
  const arg = msg.args && msg.args[index];
  return arg && typeof arg === 'object' ? arg.value : arg;
}

const WAVESHARE_ADDR_RE = /^\/remote\/waveshare\/([1-6])$/;
const WAVESHARE_DETAIL_ADDR_RE = /^\/remote\/waveshare_detail\/([1-6])$/;
const WAVESHARE_DETAIL_FIELDS = ['ch1Index', 'ch2Index', 'ch3Index'];

oscPort.on('message', (msg) => {
  if (msg.address === '/remote/heartbeat') {
    markConnected(true);
    armHeartbeatTimeout();
  } else if (msg.address === '/remote/game_state') {
    const value = Number(oscArgValue(msg, 0));
    if (GAME_STATE_NAMES[value] && value !== state.gameState) {
      state.gameState = value;
      broadcastState();
    }
  } else if (msg.address === '/remote/player_active') {
    // Authoritative from TD -- overwrites whatever the server locally assumed
    // from the last toggle/reset command. Without this, player ON/OFF was
    // only ever the server's own memory of the last command it sent, which
    // resets to all-OFF on every server restart regardless of TD's actual
    // state (this was hit for real: a restart showed all players OFF on the
    // page while TD still had them ON).
    const next = {
      1: Boolean(Number(oscArgValue(msg, 0))),
      2: Boolean(Number(oscArgValue(msg, 1))),
      3: Boolean(Number(oscArgValue(msg, 2))),
    };
    if (next[1] !== state.players[1] || next[2] !== state.players[2] || next[3] !== state.players[3]) {
      state.players = next;
      broadcastState();
    }
  } else {
    const waveshareMatch = msg.address.match(WAVESHARE_ADDR_RE);
    const waveshareDetailMatch = msg.address.match(WAVESHARE_DETAIL_ADDR_RE);
    if (waveshareMatch) {
      const board = Number(waveshareMatch[1]);
      const live = Boolean(Number(oscArgValue(msg, 0)));
      if (state.waveshare[board] !== live) {
        state.waveshare[board] = live;
        broadcastState();
      }
    } else if (waveshareDetailMatch) {
      const board = Number(waveshareDetailMatch[1]);
      const detail = {};
      WAVESHARE_DETAIL_FIELDS.forEach((field, i) => {
        detail[field] = Number(oscArgValue(msg, i));
      });
      state.waveshareDetail[board] = detail;
      broadcastAdminState();

      const anyConnected = computeAnyConnected();
      if (state.anyConnected !== anyConnected) {
        state.anyConnected = anyConnected;
        broadcastState();
      }
    }
  }
});

oscPort.on('error', (err) => {
  console.error('OSC port error:', err.message);
});

oscPort.open();
armHeartbeatTimeout();

function sendOSC(address, value) {
  oscPort.send({
    address,
    args: [{ type: 'i', value: value ? 1 : 0 }],
  });
}

// ---- HTTP + static site ------------------------------------------------

const app = express();
const publicStatic = express.static(path.join(__dirname, 'public'));
// cacheControl/etag/lastModified disabled: express.static's own Cache-Control
// header would otherwise be set unconditionally, clobbering the explicit
// no-store header set below.
const adminStatic = express.static(path.join(__dirname, 'public-admin'), {
  cacheControl: false,
  etag: false,
  lastModified: false,
});

// Host-gated: the admin status page (private resistor-reading detail) only
// exists behind ADMIN_HOSTNAME. Any other Host header -- including the main
// public hostname -- gets the regular control page and never sees the
// admin directory at all, so there's no path to guess.
app.use((req, res, next) => {
  if (isAdminHost(req.headers.host)) {
    // no-store: this page changes often during development, and both the
    // browser and Cloudflare's edge will otherwise cache it -- previously
    // caused a stale app.js to keep rendering removed fields (e.g. a
    // leftover "hb undefined") well after the server was updated.
    res.set('Cache-Control', 'no-store');
    adminStatic(req, res, next);
  } else {
    publicStatic(req, res, next);
  }
});

const server = app.listen(HTTP_PORT, () => {
  console.log(`Remote controller running:`);
  console.log(`  Local:   http://localhost:${HTTP_PORT}`);
  for (const addrs of Object.values(os.networkInterfaces())) {
    for (const addr of addrs || []) {
      if (addr.family === 'IPv4' && !addr.internal) {
        console.log(`  Network: http://${addr.address}:${HTTP_PORT}`);
      }
    }
  }
  console.log(`Sending OSC commands to ${TD_HOST}:${TD_OSC_SEND_PORT}`);
  console.log(`Listening for TouchDesigner heartbeat on UDP ${TD_OSC_LISTEN_PORT}`);
});

// ---- WebSocket link to browser clients ---------------------------------

const wss = new WebSocketServer({ server, path: '/ws' });

function currentStateMessage() {
  return JSON.stringify({
    type: 'state',
    players: state.players,
    connected: state.connected,
    gameState: state.gameState,
    gameStateLabel: GAME_STATE_NAMES[state.gameState] || null,
    waveshare: state.waveshare,
    anyConnected: state.anyConnected,
  });
}

function broadcastState() {
  const msg = currentStateMessage();
  for (const client of wss.clients) {
    if (client.readyState === client.OPEN) client.send(msg);
  }
}

function adminStateMessage() {
  return JSON.stringify({
    type: 'adminState',
    waveshareDetail: state.waveshareDetail,
    answerIndex,
  });
}

function broadcastAdminState() {
  const msg = adminStateMessage();
  for (const client of wss.clients) {
    if (client.isAdmin && client.readyState === client.OPEN) client.send(msg);
  }
}

const VALID_PULSES = new Set(['reset_game', 'start_game', 'open_monitors', 'reset_best_time']);
const IDLE_STATE = 1;

wss.on('connection', (ws, req) => {
  ws.isAdmin = isAdminHost(req.headers.host);
  ws.send(currentStateMessage());
  if (ws.isAdmin) ws.send(adminStateMessage());

  ws.on('message', (raw) => {
    let msg;
    try {
      msg = JSON.parse(raw);
    } catch {
      return;
    }

    if (msg.type === 'toggle' && [1, 2, 3].includes(msg.player)) {
      if (state.gameState !== IDLE_STATE) return;
      const next = !state.players[msg.player];
      state.players[msg.player] = next;
      sendOSC(`/remote/player/${msg.player}`, next);
      broadcastState();
    } else if (msg.type === 'pulse' && VALID_PULSES.has(msg.action)) {
      if (msg.action === 'start_game') {
        if (state.gameState !== IDLE_STATE) return;
        if (!Object.values(state.players).some(Boolean)) return;
        if (state.anyConnected) return;
      }
      sendOSC(`/remote/${msg.action}`, 1);

      if (msg.action === 'reset_game') {
        const playersToActivate = [1, 2, 3].filter((player) => !state.players[player]);
        for (const player of playersToActivate) {
          state.players[player] = true;
        }
        if (playersToActivate.length > 0) broadcastState();

        // Staggered rather than fired back-to-back: TD's OSC In DAT/CHOP
        // processes on its own frame cadence, and multiple UDP packets
        // arriving within the same frame can get coalesced or dropped.
        playersToActivate.forEach((player, i) => {
          setTimeout(() => sendOSC(`/remote/player/${player}`, true), i * PLAYER_ACTIVATE_STAGGER_MS);
        });
      }
    }
  });
});
