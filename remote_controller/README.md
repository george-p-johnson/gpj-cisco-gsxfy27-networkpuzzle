# Remote Controller

A small web app for operating the GSXFY27 Network Puzzle game from a phone,
tablet, or laptop on the same network as the show PC.

- 3 toggle buttons: **Activate Player 1 / 2 / 3** (only enabled while the game state is Idle *and* that player's boards are online -- Player 1 needs boards 1/2, Player 2 needs boards 3/4, Player 3 needs boards 5/6; shows "NO BOARDS" otherwise)
- 2 pulse buttons: **Start Game** (only enabled while the game state is Idle, at least one player is active, *and* no board has a patch cord still plugged in from the previous game -- shows a docent-facing reminder naming exactly which player/question is still connected, e.g. "Player 2 Q4", otherwise), **Reset Game** (also auto-activates any player not already ON *and* whose boards are online, so a fresh reset defaults to all reachable players ready)
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
the show PC autostarts everything, with crash auto-recovery
(`../START_CISCO_NETWORK_PUZZLE.bat` + Task Scheduler).

## Message reference

| UI action | OSC address (Remote → TD) | Value |
| --- | --- | --- |
| Toggle Player 1/2/3 | `/remote/player/1`, `/2`, `/3` | `1` = on, `0` = off |
| Start Game | `/remote/start_game` | `1` |
| Reset Game | `/remote/reset_game` | `1` |
| Open Monitors *(admin page only)* | `/remote/open_monitors` | `1` |
| Reset Best Time *(admin page only)* | `/remote/reset_best_time` | `1` |

Reset Game also triggers up to three additional `/remote/player/<n>` = `1`
messages right after it (staggered `PLAYER_ACTIVATE_STAGGER_MS` apart, see
`.env.example`) — one for each player not already active, so TD sees the
same activation messages it would if someone had toggled them by hand.

| TD → Remote | OSC address | Purpose |
| --- | --- | --- |
| Heartbeat | `/remote/heartbeat` | Sent ~every 1s; drives Connected/Disconnected |
| Game state | `/remote/game_state` | Int `1`-`7`; drives the game-state display |
| Player active | `/remote/player_active` | 3 ints `[p1, p2, p3]` as 0/1; authoritative -- overwrites the server's local player state on receipt (see below) |
| Player result | `/remote/player_result` | 3 ints `[p1, p2, p3]`, placement code `0`-`3`: `0` = Try Again, `1` = Winner, `2` = 2nd place, `3` = 3rd place. TD flips `1`-`3` live per player as they finish during Gameplay; `0` is set for anyone still unfinished at the Gameplay->Results boundary once time runs out. Drives the WINNER/2ND PLACE/3RD PLACE/TRY AGAIN label on each active player's button, shown only while game state is Gameplay or Results (otherwise the plain ON/OFF/NO BOARDS label is shown, ignoring any stale placement from a previous round) |
| Board status | `/remote/waveshare/1` ... `/remote/waveshare/6` | `1` = board reporting recently, `0` = stale/offline; drives the Boards indicator, and (paired up) the per-player `boardsReady` flags below |
| Board detail | `/remote/waveshare_detail/1` ... `/6` | 3 ints: `ch1Index, ch2Index, ch3Index` (0-10 resistor classification, see below) -- raw per-channel readings power the private admin status page only |

Board status also drives a public, non-admin-gated `boardsReady` map in the
state broadcast: Player 1 is ready only when boards 1 *and* 2 are both
reporting, Player 2 needs boards 3/4, Player 3 needs boards 5/6 (same
board->panel mapping as `question_game/CLAUDE.md` Section 6D). A player
whose boards aren't ready has its **Activate Player N** button disabled
(shown as "NO BOARDS") even while the game state is Idle, and is skipped by
Reset Game's auto-activate. The server also rejects a `toggle` trying to
turn that player ON outright, so this can't be bypassed by a stale/cached
page -- though an already-active player can still be toggled back OFF if its
boards drop out mid-idle.

Board detail also drives two public, non-admin-gated derived fields:
* `anyConnected` -- `true` whenever any channel on any board reads other
  than `10` (NONE) -- i.e. something is still plugged in somewhere. This
  blocks the **Start Game** button and shows the docent reminder above
  (server also rejects a `start_game` pulse outright while it's true, so
  this can't be bypassed by a stale/cached page).
* `connectedCables` -- an array of `{ player, question }` pairs (question
  `1`-`6`) naming exactly which sockets are still plugged in, rendered under
  the docent reminder (e.g. "Player 2 Q4, Player 3 Q1"). Both fields are
  presence-only -- neither reveals which resistor is plugged in or whether
  it's correct, so both are safe to send to every connected client, not just
  admin ones.

Resistor classification index (same 0-10 scale the ESP32 firmware uses and
the admin page translates to a label -- see `public-admin/app.js`
`RESISTOR_LABELS`):

| Index | Resistor |
| :---: | --- |
| 0 | 470Ω |
| 1 | 1kΩ |
| 2 | 2.2kΩ |
| 3 | 4.7kΩ |
| 4 | 10kΩ |
| 5 | 22kΩ |
| 6 | 33kΩ |
| 7 | 47kΩ |
| 8 | 68kΩ |
| 9 | 100kΩ |
| 10 | NONE (socket empty / open circuit) |

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
has the page open. Player state specifically is also confirmed by TD
(`/remote/player_active`, not just assumed from the last command the server
sent) -- otherwise a server restart would show every player OFF regardless
of what TD actually had active, since the server's own memory resets to
all-OFF on startup. See `touchdesigner/SETUP.md` Section 5c for the TD-side
wiring.

## Private admin status page

`public-admin/` is a second, separate page showing each board's live
resistor-classification reading per question (Q1-Q3 or Q4-Q6 depending on
the board, translated from the 0-10 index to a resistor value, e.g. "33kΩ"),
grouped by Player 1/2/3 -- e.g. to check whether Q4 on Player 2's boards is
plugged into the right resistor. It also carries the
**Open Monitors** and **Reset Best Time** pulse buttons, moved here from the
public control page since they're maintenance actions, not something a
random visitor to the public URL should be able to trigger. TD only sends the raw
classification (no match data), so the server reads the correct-answer key
itself: once at launch, from `answer_table.tsv` at the repo root (the same
file the ESP32 boards' SD cards read from -- see `server.js`
`loadAnswerIndex()`) and sends it to admin clients alongside the live
readings. **Editing `answer_table.tsv` needs a server restart to take
effect** -- it's read once at startup, not watched for changes. It's only
served to requests whose `Host` header matches `ADMIN_HOSTNAME` (`.env`, default
`admin.gsxnetworkpuzzle.com`); any other hostname never sees that directory.
See `touchdesigner/SETUP.md` Section 4b for the TD-side wiring this needs,
and `DEPLOYMENT.md` for exposing it as its own subdomain through the
Cloudflare Tunnel.

Every response served on the admin hostname carries `Cache-Control: no-store`
(both browser and Cloudflare-edge caching disabled) -- this page changes
often during development, and a cached `app.js` will keep running old logic
against new data long after the server's been updated (e.g. rendering a
field the current code no longer sends). The public control page is
unaffected and keeps normal caching.

**`no-store` only prevents *new* staleness -- it doesn't retroactively clear
copies Cloudflare's edge had already cached from before that header
existed.** Confirmed in practice: after adding `no-store` and deploying an
`app.js` change, some clients still received the old file -- a hard refresh
on the affected device wasn't enough, but Cloudflare edge nodes are
per-location, so different visitors can be served from different (stale vs.
fresh) edges simultaneously. Fix: **Cloudflare dashboard → Caching →
Configuration → Purge Everything** (or a Custom Purge of
`admin.gsxnetworkpuzzle.com/*`). This is a global edge purge and needs
dashboard access -- there's no CLI/API token set up for it in this project,
so it has to be done manually when it comes up.
