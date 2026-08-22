# Wiring the remote controller into TouchDesigner

The remote controller server talks to TouchDesigner over OSC/UDP, the same
transport already used by the ESP32 boards -- just on different ports so it
never collides with the boards' telemetry on port 9000.

| Direction | Port | Purpose |
| --- | --- | --- |
| Remote → TD | `9001` (UDP) | Button presses (toggles + pulses) |
| TD → Remote | `9002` (UDP) | Heartbeat + current game state |
| TD → Boards | `9003` (UDP, broadcast on `192.168.50.0/24`) | Current game state, for lighting effects |

The first two use loopback (`127.0.0.1`) since the remote server and
TouchDesigner run on the same PC. The third is separate -- it goes out over
the same Ethernet network the boards' own telemetry (port `9000`) arrives
on, to `192.168.50.255` so all six boards receive it with a single send.

Both `.env`-configurable defaults live in [`../.env.example`](../.env.example).

## 1. Receive commands: OSC In DAT

1. Add an **OSC In DAT** to your network (`oscin1`).
2. Set `Network Port` to **9001**.
3. Leave `Local Address` blank (listens on all interfaces) — the remote
   controller connects from `127.0.0.1` since it runs on the same PC.

Each row it outputs has an `address` column like `/remote/player/1` and an
`args` column with the value.

## 2. Dispatch commands: Callbacks DAT (not DAT Execute)

**Use `oscin1`'s own `Callbacks DAT` (`onReceiveOSC`), not a `DAT Execute`
watching its table.** An earlier version of this doc recommended a
`DAT Execute` looping over `oscin1`'s rows on every table change -- that
pattern is broken for a live command stream: if the OSC In DAT retains more
than one row (which it does by default), every new incoming message causes
the loop to *re-process every old row still sitting in the table too*, so a
stale command from minutes ago can fire again alongside a brand new one.
This was actually hit and confirmed live: toggling a player replayed an old
`start_game` command. `onReceiveOSC` fires exactly once per real incoming
packet, with no table/history involved, so this class of bug can't happen.

Select `oscin1` → find its `Callbacks DAT` parameter → create one (this
generates a template with the correct `onReceiveOSC` signature for your TD
build) → fill it in:

```python
# oscin1's Callbacks DAT

def onReceiveOSC(dat, rowIndex, message, bytes, timeStamp, address, args, peer):
    value = int(args[0]) if args else 0

    if address == '/remote/player/1':
        set_player_active(1, value)
    elif address == '/remote/player/2':
        set_player_active(2, value)
    elif address == '/remote/player/3':
        set_player_active(3, value)
    elif address == '/remote/start_game':
        start_game()
    elif address == '/remote/reset_game':
        reset_game()
    elif address == '/remote/open_monitors':
        open_monitors()
    elif address == '/remote/reset_best_time':
        reset_best_time()
    return


def set_player_active(player_num, is_active):
    # TODO: wire to your actual player-activation logic
    debug(f'player {player_num} -> {"ON" if is_active else "OFF"}')

    # Remember it so both the periodic timer (Section 3/5) and this
    # immediate send can read it back -- see Section 5b.
    op('player_active_holder').store('player_%d' % player_num, bool(is_active))
    send_player_active_to_boards()


def start_game():
    # TODO: pulse your Start Game logic
    debug('start_game')


def reset_game():
    # TODO: pulse your Reset Game logic
    debug('reset_game')


def open_monitors():
    # TODO: pulse your Open Monitors logic
    debug('open_monitors')


def reset_best_time():
    # TODO: pulse your Reset Best Time logic
    debug('reset_best_time')
```

Pulse-type commands (`start_game`, `reset_game`, `open_monitors`,
`reset_best_time`) always arrive with value `1` — there's no explicit "off"
message, treat receipt of the message as the pulse itself.

Reset Game additionally causes the remote server to send up to three more
`/remote/player/<n>` messages right after it (staggered ~150ms apart, see
`PLAYER_ACTIVATE_STAGGER_MS` in `.env`) — it auto-activates any player that
wasn't already ON. No special handling needed here: they arrive as ordinary
`/remote/player/<n>` messages through the same callback above.

## 3. Send status back: OSC Out DAT + a Timer/Execute

So the web page can show **Connected**/**Disconnected** and the **current
game state**, TouchDesigner needs to send two small OSC messages back
roughly once a second.

The remote controller understands 7 game-state integers:

| Value | State |
| :---: | --- |
| 1 | Idle |
| 2 | Three |
| 3 | Two |
| 4 | One |
| 5 | Start |
| 6 | Gameplay |
| 7 | Results |

1. Add an **OSC Out DAT** (`oscout_remote`).
   - `Network Address`: `127.0.0.1`
   - `Network Port`: `9002`
2. Add a **Timer CHOP** (or a **Timer COMP**) set to fire every ~1 second,
   with a **CHOP Execute DAT** watching it. On each tick, send both the
   heartbeat and the current state integer (pull the state value from
   wherever your game logic tracks it -- e.g. a Select DAT/CHOP/stored var):

```python
# CHOP Execute callback, watching a 1-second Timer CHOP

def onValueChange(channel, sampleIndex, val, prev):
    op('oscout_remote').sendOSC('/remote/heartbeat', [1])

    # TODO: replace with wherever your game logic stores the current state (1-7)
    current_state = int(op('game_state_holder')['state'])
    op('oscout_remote').sendOSC('/remote/game_state', [current_state])
    return
```

If the remote server doesn't receive a heartbeat for 3 seconds
(`HEARTBEAT_TIMEOUT_MS` in `.env`), it flips every connected browser to
"Disconnected" (the last known game state stays displayed, just grayed out
by the disconnected indicator). Sending `/remote/game_state` on the same
1-second cadence as the heartbeat means a browser that just loaded the page
picks up the correct current state within a second, instead of waiting for
the next actual state transition.

If you'd rather send `/remote/game_state` only when the state actually
changes (event-driven, from whatever callback drives your state machine)
that works too -- just make sure it's also sent once from the same place
the heartbeat timer starts, so a freshly loaded page isn't stuck on "—"
until the next transition.

## 4. Report board (Waveshare) status

The remote page can also show a live/stale indicator for each of the six
ESP32 boards. Board firmware (`question_game.ino`) now sends an 8th int arg,
`heartbeat`, on every `/waveshare/<index>` message -- it increments on every
send regardless of whether the resistor readings changed, so a listener can
tell "board alive, unchanged reading" apart from "board gone". **This
requires reflashing all six boards** with the updated firmware (see
`question_game/CLAUDE.md` Section 9C) before the steps below will work.

Board comms (`oscin_comms`) is an **OSC In CHOP**, so the `heartbeat` arg
lands on its own channel per board (naming depends on your CHOP's
address/split settings -- confirm the actual name with a board sending live
data, e.g. something like `waveshare1_7`; adjust the code below to match).

Rather than timestamping every incoming packet, liveness is decided by
comparing that channel's *current* value against whatever it held on the
*previous* 1-second tick -- since `heartbeat` increments on every send, an
unchanged value between two ticks means the board has gone quiet. This reads
directly on the same timer that already sends `/remote/heartbeat` and
`/remote/game_state` (Section 3); no separate `CHOP Execute DAT` watching
`oscin_comms` is needed:

```python
# Same onCycle callback as Section 3, appended
oscin_boards = op('oscin_comms')
for board in range(1, 7):
    chan_name = 'waveshare%d_7' % board  # <- adjust to your real channel naming
    heartbeat = int(oscin_boards[chan_name].eval()) if chan_name in oscin_boards.chans() else None
    prev_heartbeat = oscin_boards.fetch('prev_heartbeat_%d' % board, None)
    live = heartbeat is not None and heartbeat != prev_heartbeat
    oscin_boards.store('prev_heartbeat_%d' % board, heartbeat)
    op('oscout_remote').sendOSC('/remote/waveshare/%d' % board, [1 if live else 0])
```

A board that's actually gone leaves `oscin_comms` holding its last-received
value forever, so `live` correctly flips to `False` on the very next tick
after it stops sending -- same ~1-second detection window as a timestamp
approach, without needing wall-clock time or a second watcher DAT.

Board index → panel, if you want it to match the physical layout: boards
1-2 are Panel 1 (Player 1), 3-4 are Panel 2 (Player 2), 5-6 are Panel 3
(Player 3) -- same mapping as `question_game/CLAUDE.md` Section 6D. The
remote page's Boards row is already grouped this way.

**If you'd rather not touch `oscin_comms`'s channel-naming assumptions at
all:** the alternative is swapping board comms from an OSC In CHOP to an
OSC In DAT (+ a `DAT to CHOP` downstream for anything that still needs the
old channels), which gets you `onReceiveOSC(address, args)` with `args[7]`
as the heartbeat directly, no channel-name guessing required. More
disruptive to your existing network, but more robust long-term.

## 5. Send game state to the boards (lighting effects)

Board firmware now listens on UDP **9003** for a `/game_state` broadcast
(one int32 arg, same 1-7 convention as everywhere else) and drives its LEDs
accordingly -- **this requires reflashing all six boards** first (see
`question_game/CLAUDE.md` Section 9C). The lighting behavior is baked into
firmware, not something TD controls per-LED -- TD only needs to broadcast
the current state number:

| State | Board lighting behavior |
| --- | --- |
| Idle (1) | Flickering green (mimics an Ethernet switch port's activity LED), regardless of what's plugged in |
| Three (2) | CH1 red, CH2/CH3 off |
| Two (3) | CH1 + CH2 red, CH3 off |
| One (4) | CH1 + CH2 + CH3 red |
| Start (5) | All off |
| Gameplay (6) / Results (7) | Real connection data: green = correct, red = wrong resistor plugged in, off = socket empty |

A board whose player is inactive (see Section 5b below) shows Idle lighting
throughout, regardless of which of the states above the rest of the game is
actually in.

1. Add an **OSC Out DAT** (`oscout_boards`):
   - `Network Address`: `192.168.50.255` (subnet broadcast -- reaches all
     six boards with one send; the boards' own static IPs are
     `192.168.50.11`-`192.168.50.16`, but you don't need to target them
     individually)
   - `Network Port`: `9003`
2. Send `/game_state` on the same 1-second timer as Section 3, and ideally
   also **immediately whenever the state actually changes** rather than
   waiting for the next tick -- the countdown states (Three/Two/One) are
   short, so a full second of latency on the lighting cue would be visibly
   laggy on stage. If your state machine already has a single place where
   the state transitions, fire the send from there too:

   ```python
   # Same onCycle callback as Section 3, appended
   op('oscout_boards').sendOSC('/game_state', [current_state])
   ```

   (`current_state` is the same int you're already computing for
   `/remote/game_state` in Section 3 -- just fan it out to a second OSC Out
   DAT.)

3. Sanity check on the bench before the full six-board reflash: flash one
   board, watch its Serial output for `>> Listening for game-state
   broadcasts on UDP 9003`, then step your TD state through Idle → Three →
   Two → One → Start → Gameplay and confirm the LEDs match the table above
   at each step (Gameplay needs an actual patch cord plugged in to see
   green/red -- otherwise all three sockets read NONE and stay off, which
   is also correct).

## 5b. Send player-active state to the boards

Board firmware also listens on the same UDP **9003** port for a
`/player_active` broadcast (**3 int32 args**, `[p1, p2, p3]` as 0/1) --
**this requires the same reflash** as Section 5 (see `question_game/CLAUDE.md`
Section 7D). Each board knows which player it belongs to (boards 1/2 = P1,
3/4 = P2, 5/6 = P3, same mapping as Section 4's Board index → panel note
above) and, when that player is inactive, shows Idle lighting no matter what
`/game_state` says -- so a 2-player game leaves the unused player's two
boards flickering idle throughout the whole match instead of running the
countdown/match lighting for nobody.

1. Reuse the same `oscout_boards` DAT from Section 5 -- no new OSC Out DAT
   needed, just a new address on the same port.
2. Give TD somewhere to remember the 3 players' active state so it can be
   read back by both an immediate send and the periodic timer. A `Base COMP`
   with `.store()`/`.fetch()` works well (called `player_active_holder`
   below); a Table DAT works too if you prefer.
3. In `set_player_active()` (Section 2's Callbacks DAT), store the value and
   immediately re-broadcast to the boards -- already shown inline in
   Section 2 above:

   ```python
   def set_player_active(player_num, is_active):
       # TODO: wire to your actual player-activation logic
       op('player_active_holder').store('player_%d' % player_num, bool(is_active))
       send_player_active_to_boards()


   def send_player_active_to_boards():
       holder = op('player_active_holder')
       p1 = 1 if holder.fetch('player_1', True) else 0
       p2 = 1 if holder.fetch('player_2', True) else 0
       p3 = 1 if holder.fetch('player_3', True) else 0
       op('oscout_boards').sendOSC('/player_active', [p1, p2, p3])
   ```

   Default each `fetch()` to `True` (matches the firmware's own default of
   all-active) so a board never gets a spurious idle-lock before TD has
   heard anything.

4. Also call `send_player_active_to_boards()` from the same 1-second timer
   that already sends `/game_state` (Section 3/5), so a board that reboots
   mid-game (or missed a packet) still converges on the correct state within
   a second, rather than waiting for the next actual toggle. That function
   lives in `oscin1`'s Callbacks DAT (a different DAT/Python module than this
   Timer's CHOP Execute DAT), so reach across with `.module`:

   ```python
   # Same onCycle callback as Section 3/5, appended
   op('oscin1').module.send_player_active_to_boards()
   ```

5. Sanity check on the bench: with a board flashed and player-active wiring
   done, toggle a player OFF on the remote page while that board's state is
   Idle, then step the TD state through Three → Two → One → Gameplay -- the
   board should stay on flickering-green idle lighting the whole time
   instead of following the countdown/match lighting. Toggle the player back
   ON and confirm the board immediately starts following `/game_state`
   again on the next send.

## 6. Firewall

Since everything above uses `127.0.0.1` (remote server and TouchDesigner on
the same PC), Windows Firewall shouldn't need any changes for the OSC
traffic. The only inbound rule you may need is for the **web page's HTTP
port** (default `8080`) so other devices on the LAN can reach it — Node will
prompt for this automatically the first time it runs, if the popup doesn't
appear, allow `node.exe` for Private networks.
