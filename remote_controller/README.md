# Remote Controller

A small web app for operating the GSXFY27 Network Puzzle game from a phone,
tablet, or laptop on the same network as the show PC.

- 3 toggle buttons: **Activate Player 1 / 2 / 3** (only enabled while the game state is Idle)
- 2 pulse buttons: **Start Game** (only enabled while the game state is Idle *and* at least one player is active), **Reset Game** (also auto-activates any player not already ON, so a fresh reset defaults to all 3 players ready)
- A **More options** menu with 2 hidden pulse buttons: **Open Monitors**,
  **Reset Best Time**
- A live **Connected / Disconnected** indicator for the link to TouchDesigner
- A live **game state** display (Idle / Three / Two / One / Start / Gameplay / Results)
- A live **Boards** indicator showing which of the 6 ESP32 question boards are currently reporting, grouped by panel

It runs as a small Node.js server on the same Windows PC as TouchDesigner:
the server hosts the control page over plain HTTP/WebSocket for any device
on the LAN, and separately speaks OSC/UDP to TouchDesigner (matching the
OSC convention the ESP32 boards already use, on different ports so it never
touches port 9000).

```
Browser  <--WebSocket-->  Node server  <--OSC/UDP-->  TouchDesigner
(any LAN device)          (this PC)                    (this PC)
```

## Setup

```bash
npm install
cp .env.example .env
```

Edit `.env` if you want a different HTTP port (default `8080`) or different
OSC ports. See [`.env.example`](.env.example) for all options.

## Run

```bash
npm start
```

The console prints the URLs to use:

```
Local:   http://localhost:8080
Network: http://192.168.x.x:8080
```

Open the `Network` URL from any phone/tablet/laptop on the same network as
the show PC.

## TouchDesigner side

See [`touchdesigner/SETUP.md`](touchdesigner/SETUP.md) for the OSC In DAT /
Callbacks DAT / heartbeat wiring needed inside the `.toe` to receive
commands and report connection status back.

## Show PC deployment

See [`DEPLOYMENT.md`](DEPLOYMENT.md) for how this is made reachable from
outside the LAN (Cloudflare Tunnel, `https://gsxnetworkpuzzle.com`) and how
the show PC autostarts everything (`../start_show.bat` + Task Scheduler).

## Message reference

| UI action | OSC address (Remote → TD) | Value |
| --- | --- | --- |
| Toggle Player 1/2/3 | `/remote/player/1`, `/2`, `/3` | `1` = on, `0` = off |
| Start Game | `/remote/start_game` | `1` |
| Reset Game | `/remote/reset_game` | `1` |
| Open Monitors | `/remote/open_monitors` | `1` |
| Reset Best Time | `/remote/reset_best_time` | `1` |

Reset Game also triggers up to three additional `/remote/player/<n>` = `1`
messages right after it (staggered `PLAYER_ACTIVATE_STAGGER_MS` apart, see
`.env.example`) — one for each player not already active, so TD sees the
same activation messages it would if someone had toggled them by hand.

| TD → Remote | OSC address | Purpose |
| --- | --- | --- |
| Heartbeat | `/remote/heartbeat` | Sent ~every 1s; drives Connected/Disconnected |
| Game state | `/remote/game_state` | Int `1`-`7`; drives the game-state display |
| Board status | `/remote/waveshare/1` ... `/remote/waveshare/6` | `1` = board reporting recently, `0` = stale/offline; drives the Boards indicator |

Game state values:

| Value | State |
| :---: | --- |
| 1 | Idle |
| 2 | Three |
| 3 | Two |
| 4 | One |
| 5 | Start |
| 6 | Gameplay |
| 7 | Results |

Toggle state and the last-known game state are held centrally on the Node
server, so every connected browser stays in sync if more than one person
has the page open.
