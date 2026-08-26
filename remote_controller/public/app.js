(() => {
  const statusEl = document.getElementById('status');
  const statusText = document.getElementById('statusText');
  const gameStateEl = document.getElementById('gameState');
  const gameStateValue = document.getElementById('gameStateValue');
  const startBtn = document.querySelector('.pulse-btn.start');
  const boardStatusEl = document.getElementById('boardStatus');
  const connectionWarningEl = document.getElementById('connectionWarning');
  const connectionWarningDetailEl = document.getElementById('connectionWarningDetail');
  const answerKeyToggleEl = document.getElementById('answerKeyToggle');
  const answerKeyEl = document.getElementById('answerKey');
  const docentInstructionsToggleEl = document.getElementById('docentInstructionsToggle');
  const docentInstructionsEl = document.getElementById('docentInstructions');

  const IDLE_STATE = 1;
  const GAMEPLAY_STATE = 6;
  const RESULTS_STATE = 7;
  const RESULT_LABELS = { 1: 'WINNER', 2: '2ND PLACE', 3: '3RD PLACE' };

  // Matches the --p1/--p2/--p3 colors in style.css -- named here so any
  // copy mentioning a player number can also name its color, since the
  // patch panels are physically color-coded to match.
  const PLAYER_COLOR_NAMES = { 1: 'Green', 2: 'Navy', 3: 'Teal' };

  function appendPlayerColorTag(el, player) {
    const tag = document.createElement('span');
    tag.className = `player-color-tag p${player}`;
    tag.textContent = `(${PLAYER_COLOR_NAMES[player]})`;
    el.append(' ', tag);
  }

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
  }

  // A player's boards being offline overrides everything else -- the button
  // stays disabled even during Idle, since there's nothing to play on.
  function updatePlayerButtons(gameState, players, boardsReady) {
    for (const [num, btn] of playerButtons) {
      const ready = !boardsReady || boardsReady[num];
      const active = Boolean(players && players[num]);
      btn.disabled = gameState !== IDLE_STATE || (!ready && !active);
      btn.classList.toggle('boards-offline', !ready);
      btn.title = ready ? '' : 'Boards offline for this player';
    }
  }

  // Result labels only mean anything for an active player while a round is
  // actually underway/decided. 0 is ambiguous before then -- during
  // Gameplay it just means "still going" (not yet finished), and only reads
  // as "Try Again" once Results locks it in. Outside those two states (or
  // for an inactive player), ignore playerResults entirely and fall back to
  // the plain ON/OFF/NO BOARDS label.
  function resultLabel(gameState, result) {
    if (RESULT_LABELS[result]) return RESULT_LABELS[result];
    if (gameState === RESULTS_STATE) return 'TRY AGAIN';
    return null;
  }

  function applyPlayers(players, boardsReady, gameState, playerResults) {
    for (const [num, btn] of playerButtons) {
      const active = Boolean(players[num]);
      const ready = !boardsReady || boardsReady[num];
      const showResult = active && (gameState === GAMEPLAY_STATE || gameState === RESULTS_STATE);
      const result = showResult ? playerResults && playerResults[num] : undefined;
      const label = showResult ? resultLabel(gameState, result) : null;
      btn.classList.toggle('active', active);
      if (label) {
        btn.dataset.result = String(result || 0);
      } else {
        delete btn.dataset.result;
      }
      btn.querySelector('.state').textContent = label || (active ? 'ON' : ready ? 'OFF' : 'NO BOARDS');
    }
  }

  // Mirrors server.js's CABLE_BLOCK_THRESHOLD -- a single stray reading
  // (P1q1's socket is finicky and occasionally misreads as connected)
  // shouldn't stop a round, so Start only actually gets blocked once a
  // real handful of cables are reporting connected.
  const CABLE_BLOCK_THRESHOLD = 3;

  function updateStartButton(gameState, players, connectedCables) {
    const anyPlayerActive = Object.values(players || {}).some(Boolean);
    const tooManyCables = (connectedCables || []).length > CABLE_BLOCK_THRESHOLD;
    startBtn.disabled = gameState !== IDLE_STATE || !anyPlayerActive || tooManyCables;
  }

  function updateConnectionWarning(gameState, connectedCables) {
    const count = (connectedCables || []).length;
    const show = gameState === IDLE_STATE && count > 0;
    connectionWarningEl.hidden = !show;
    if (show && count > CABLE_BLOCK_THRESHOLD) {
      // Only worth naming the specific sockets once there are enough of
      // them to be a real blocker -- below that it's as likely to be the
      // finicky P1q1 socket as an actual leftover cable, so naming it would
      // just send the docent chasing a false lead.
      connectionWarningDetailEl.textContent = connectedCables
        .map((c) => `Player ${c.player} (${PLAYER_COLOR_NAMES[c.player]}) Q${c.question}`)
        .join(', ');
    } else {
      connectionWarningDetailEl.textContent = '';
    }
  }

  // Static for the server's lifetime (read once from answer_table.tsv at
  // launch) -- render it once on first receipt rather than rebuilding the
  // DOM on every state broadcast.
  let answerKeyRendered = false;

  function renderAnswerKey(answerKey) {
    answerKeyEl.innerHTML = '';
    for (let player = 1; player <= 3; player++) {
      const rows = answerKey[player] || [];
      if (!rows.length) continue;

      const group = document.createElement('div');
      group.className = 'answer-key-group';

      const heading = document.createElement('h3');
      heading.append(`Player ${player}`);
      appendPlayerColorTag(heading, player);
      group.appendChild(heading);

      for (const row of rows) {
        const item = document.createElement('div');
        item.className = 'answer-key-row';

        const q = document.createElement('span');
        q.className = 'answer-key-q';
        q.textContent = `Q${row.question}`;

        const socket = document.createElement('span');
        socket.className = 'answer-key-a';
        socket.textContent = `Socket A${row.answerSocket}`;

        const answer = document.createElement('span');
        answer.className = 'answer-key-text';
        answer.textContent = row.answer;

        item.append(q, socket, answer);
        group.appendChild(item);
      }

      answerKeyEl.appendChild(group);
    }
  }

  answerKeyToggleEl.addEventListener('click', () => {
    const showing = answerKeyEl.hidden;
    answerKeyEl.hidden = !showing;
    answerKeyToggleEl.setAttribute('aria-expanded', String(showing));
    answerKeyToggleEl.textContent = showing ? 'Hide Answer Key' : 'Show Answer Key';
  });

  docentInstructionsToggleEl.addEventListener('click', () => {
    const showing = docentInstructionsEl.hidden;
    docentInstructionsEl.hidden = !showing;
    docentInstructionsToggleEl.setAttribute('aria-expanded', String(showing));
    docentInstructionsToggleEl.textContent = showing ? 'Hide Docent Instructions' : 'Show Docent Instructions';
  });

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
        applyPlayers(msg.players, msg.boardsReady, msg.gameState, msg.playerResults);
        applyGameState(msg.gameState, msg.gameStateLabel);
        updatePlayerButtons(msg.gameState, msg.players, msg.boardsReady);
        updateStartButton(msg.gameState, msg.players, msg.connectedCables);
        updateConnectionWarning(msg.gameState, msg.connectedCables);
        applyWaveshare(msg.waveshare);
        if (!answerKeyRendered && msg.answerKey) {
          renderAnswerKey(msg.answerKey);
          answerKeyRendered = true;
        }
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
