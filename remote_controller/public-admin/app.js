(() => {
  const statusEl = document.getElementById('status');
  const statusText = document.getElementById('statusText');
  const panelsEl = document.getElementById('panels');

  const RESISTOR_LABELS = [
    '470Ω', '1kΩ', '2.2kΩ', '4.7kΩ', '10kΩ',
    '22kΩ', '33kΩ', '47kΩ', '68kΩ', '100kΩ', 'NONE',
  ];

  function resistorLabel(index) {
    return RESISTOR_LABELS[index] ?? `? (${index})`;
  }

  let waveshareLive = {};
  let waveshareDetail = {};
  // Per-board [ch1, ch2, ch3] correct-answer indices, sent by the server on
  // every adminState message -- it reads answer_table.tsv once at launch
  // (see server.js loadAnswerIndex). Empty until that first message arrives.
  let answerIndex = {};

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
  }

  function channelRow(label, index, correctIndex) {
    const div = document.createElement('div');
    if (correctIndex === undefined) {
      div.className = 'channel-row unknown';
      div.innerHTML = `
        <span class="channel-label">${label}</span>
        <span class="channel-reading">${resistorLabel(index)}</span>
        <span class="channel-answer">answer key unavailable</span>
      `;
      return div;
    }
    const match = index === correctIndex;
    div.className = 'channel-row' + (match ? ' match' : ' mismatch');
    div.innerHTML = `
      <span class="channel-label">${label}</span>
      <span class="channel-reading">${resistorLabel(index)}</span>
      <span class="channel-answer">answer: ${resistorLabel(correctIndex)} (idx ${correctIndex})</span>
      <span class="channel-verdict">${match ? '✓ correct' : '✗ wrong'}</span>
    `;
    return div;
  }

  function render() {
    panelsEl.innerHTML = '';
    for (let player = 1; player <= 3; player++) {
      const panelSection = document.createElement('section');
      panelSection.className = 'panel-group';

      const heading = document.createElement('h2');
      heading.textContent = `Player ${player}`;
      panelSection.appendChild(heading);

      const boardsRow = document.createElement('div');
      boardsRow.className = 'boards-row';

      const boards = [player * 2 - 1, player * 2];
      for (const board of boards) {
        const detail = waveshareDetail[board];
        const live = Boolean(waveshareLive[board]);

        const card = document.createElement('div');
        card.className = 'board-card' + (live ? ' live' : ' stale');

        // Board->question mapping matches question_game/CLAUDE.md Section 6D:
        // odd boards (first device in the pair) cover q1-3, even boards q4-6.
        const startQuestion = board % 2 === 1 ? 1 : 4;
        const questionRange = `Q${startQuestion}–Q${startQuestion + 2}`;
        card.innerHTML = `
          <div class="board-card-header">
            <span class="board-card-title">Board ${board} <span class="board-card-sub">(${questionRange})</span></span>
            <span class="board-live-dot"></span>
          </div>
        `;

        if (!detail) {
          const empty = document.createElement('div');
          empty.className = 'board-empty';
          empty.textContent = 'No data received yet';
          card.appendChild(empty);
        } else {
          const [a1, a2, a3] = answerIndex[board] || [];
          card.appendChild(channelRow(`Q${startQuestion}`, detail.ch1Index, a1));
          card.appendChild(channelRow(`Q${startQuestion + 1}`, detail.ch2Index, a2));
          card.appendChild(channelRow(`Q${startQuestion + 2}`, detail.ch3Index, a3));

          if (a1 !== undefined) {
            const allCorrect = detail.ch1Index === a1 && detail.ch2Index === a2 && detail.ch3Index === a3;
            const footer = document.createElement('div');
            footer.className = 'board-card-footer';
            footer.innerHTML = `
              <span class="all-correct ${allCorrect ? 'yes' : 'no'}">
                ${allCorrect ? 'ALL CORRECT' : 'not solved'}
              </span>
            `;
            card.appendChild(footer);
          }
        }

        boardsRow.appendChild(card);
      }

      panelSection.appendChild(boardsRow);
      panelsEl.appendChild(panelSection);
    }
  }

  let ws;
  let reconnectDelay = 1000;

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
        waveshareLive = msg.waveshare || {};
        render();
      } else if (msg.type === 'adminState') {
        waveshareDetail = msg.waveshareDetail || {};
        answerIndex = msg.answerIndex || {};
        render();
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

  for (const btn of document.querySelectorAll('.pulse-btn')) {
    btn.addEventListener('click', () => {
      send({ type: 'pulse', action: btn.dataset.action });
      btn.classList.add('firing');
      setTimeout(() => btn.classList.remove('firing'), 200);
    });
  }

  render();
  connect();
})();
