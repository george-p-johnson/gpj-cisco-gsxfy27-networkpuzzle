(() => {
  const statusEl = document.getElementById('status');
  const statusText = document.getElementById('statusText');
  const gameStateEl = document.getElementById('gameState');
  const gameStateValue = document.getElementById('gameStateValue');
  const startBtn = document.querySelector('.pulse-btn.start');
  const boardStatusEl = document.getElementById('boardStatus');
  const connectionWarningEl = document.getElementById('connectionWarning');

  const IDLE_STATE = 1;

  const playerButtons = new Map(
    Array.from(document.querySelectorAll('.player-btn')).map((btn) => [
      Number(btn.dataset.player),
      btn,
    ])
  );

  const boardDots = new Map(
    Array.from(document.querySelectorAll('.board-dot')).map((dot) => [
      Number(dot.dataset.board),
      dot,
    ])
  );

  let ws;
  let reconnectDelay = 1000;

  function setStatus(connected) {
    statusEl.classList.remove('connected', 'disconnected');
    if (connected === true) {
      statusEl.classList.add('connected');
      statusText.textContent = 'Connected';
    } else if (connected === false) {
      statusEl.classList.add('disconnected');
      statusText.textContent = 'Disconnected';
    } else {
      statusText.textContent = 'Connecting…';
    }
    boardStatusEl.classList.toggle('stale', connected !== true);
  }

  function applyWaveshare(waveshare) {
    for (const [num, dot] of boardDots) {
      dot.classList.toggle('live', Boolean(waveshare && waveshare[num]));
    }
  }

  function applyGameState(gameState, gameStateLabel) {
    if (gameState == null) {
      gameStateEl.removeAttribute('data-state');
      gameStateValue.textContent = '—';
    } else {
      gameStateEl.setAttribute('data-state', String(gameState));
      gameStateValue.textContent = gameStateLabel || `State ${gameState}`;
    }
    for (const btn of playerButtons.values()) {
      btn.disabled = gameState !== IDLE_STATE;
    }
  }

  function applyPlayers(players) {
    for (const [num, btn] of playerButtons) {
      const active = Boolean(players[num]);
      btn.classList.toggle('active', active);
      btn.querySelector('.state').textContent = active ? 'ON' : 'OFF';
    }
  }

  function updateStartButton(gameState, players, anyConnected) {
    const anyPlayerActive = Object.values(players || {}).some(Boolean);
    startBtn.disabled = gameState !== IDLE_STATE || !anyPlayerActive || Boolean(anyConnected);
  }

  function updateConnectionWarning(gameState, anyConnected) {
    connectionWarningEl.hidden = !(gameState === IDLE_STATE && anyConnected);
  }

  function connect() {
    const proto = location.protocol === 'https:' ? 'wss' : 'ws';
    ws = new WebSocket(`${proto}://${location.host}/ws`);

    ws.addEventListener('open', () => {
      reconnectDelay = 1000;
    });

    ws.addEventListener('message', (event) => {
      let msg;
      try {
        msg = JSON.parse(event.data);
      } catch {
        return;
      }
      if (msg.type === 'state') {
        setStatus(msg.connected);
        applyPlayers(msg.players);
        applyGameState(msg.gameState, msg.gameStateLabel);
        updateStartButton(msg.gameState, msg.players, msg.anyConnected);
        updateConnectionWarning(msg.gameState, msg.anyConnected);
        applyWaveshare(msg.waveshare);
      }
    });

    ws.addEventListener('close', () => {
      setStatus(null);
      setTimeout(connect, reconnectDelay);
      reconnectDelay = Math.min(reconnectDelay * 1.5, 10000);
    });

    ws.addEventListener('error', () => {
      ws.close();
    });
  }

  function send(payload) {
    if (ws && ws.readyState === WebSocket.OPEN) {
      ws.send(JSON.stringify(payload));
    }
  }

  for (const [num, btn] of playerButtons) {
    btn.addEventListener('click', () => send({ type: 'toggle', player: num }));
  }

  for (const btn of document.querySelectorAll('.pulse-btn')) {
    btn.addEventListener('click', () => {
      send({ type: 'pulse', action: btn.dataset.action });
      btn.classList.add('firing');
      setTimeout(() => btn.classList.remove('firing'), 200);
    });
  }

  connect();
})();
