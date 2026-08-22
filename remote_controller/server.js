require('dotenv').config();

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
};

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
  } else {
    const waveshareMatch = msg.address.match(WAVESHARE_ADDR_RE);
    if (waveshareMatch) {
      const board = Number(waveshareMatch[1]);
      const live = Boolean(Number(oscArgValue(msg, 0)));
      if (state.waveshare[board] !== live) {
        state.waveshare[board] = live;
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
app.use(express.static(path.join(__dirname, 'public')));

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
  });
}

function broadcastState() {
  const msg = currentStateMessage();
  for (const client of wss.clients) {
    if (client.readyState === client.OPEN) client.send(msg);
  }
}

const VALID_PULSES = new Set(['reset_game', 'start_game', 'open_monitors', 'reset_best_time']);
const IDLE_STATE = 1;

wss.on('connection', (ws) => {
  ws.send(currentStateMessage());

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
