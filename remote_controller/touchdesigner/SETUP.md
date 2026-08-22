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

## 2. Dispatch commands: DAT Execute

Add a **DAT Execute DAT**, point it at `oscin1`, enable **Row Change**, and
use something like:

```python
# DAT Execute callback, watching oscin1

def onTableChange(dat):
    for row in range(1, dat.numRows):
        address = dat[row, 'address'].val
        value = int(float(dat[row, 'args'].val))

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
message, treat receipt of the row as the pulse itself.

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

1. Add an **OSC Out DAT** (`oscout1`).
   - `Network Address`: `127.0.0.1`
   - `Network Port`: `9002`
2. Add a **Timer CHOP** (or a **Timer COMP**) set to fire every ~1 second,
   with a **CHOP Execute DAT** watching it. On each tick, send both the
   heartbeat and the current state integer (pull the state value from
   wherever your game logic tracks it -- e.g. a Select DAT/CHOP/stored var):

```python
# CHOP Execute callback, watching a 1-second Timer CHOP

def onValueChange(channel, sampleIndex, val, prev):
    op('oscout1').sendOSC('/remote/heartbeat', [1])

    # TODO: replace with wherever your game logic stores the current state (1-7)
    current_state = int(op('game_state_holder')['state'])
    op('oscout1').sendOSC('/remote/game_state', [current_state])
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

Board comms (`oscin_comms`) is an **OSC In CHOP**, not a DAT, so this uses a
`CHOP Execute DAT` rather than `onReceiveOSC`:

1. First, confirm the actual channel name the `heartbeat` arg lands on --
   open `oscin_comms`'s channel list with a board sending live data and look
   for the last channel per address (naming depends on your CHOP's
   address/split settings, e.g. something like `waveshare1/7` or
   `waveshare1_7`). Adjust the regex below to match what you actually see.

2. Add a `CHOP Execute DAT` watching `oscin_comms`, using `onValueChange`
   (fires on *every* message here, since `heartbeat` always changes):

   ```python
   # CHOP Execute DAT watching oscin_comms
   import re

   def onValueChange(channel, sampleIndex, val, prev):
       m = re.match(r'waveshare(\d)_7$', channel.name)  # <- adjust to your real channel naming
       if m:
           board = int(m.group(1))
           op('oscin_comms').store('last_seen_%d' % board, absTime.seconds)
       return
   ```

3. On the same 1-second timer that already sends `/remote/heartbeat` and
   `/remote/game_state` (Section 3), also send one message per board:

   ```python
   # Same onCycle callback as Section 3, appended
   LIVE_WINDOW = 1.0  # seconds; boards report ~every 0.3s while connected

   oscin_boards = op('oscin_comms')
   for board in range(1, 7):
       last_seen = oscin_boards.fetch('last_seen_%d' % board, None)
       live = last_seen is not None and (absTime.seconds - last_seen) < LIVE_WINDOW
       op('oscout_remote').sendOSC('/remote/waveshare/%d' % board, [1 if live else 0])
   ```

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
| Idle (1) | All LEDs off, regardless of what's plugged in |
| Three (2) | CH1 red, CH2/CH3 off |
| Two (3) | CH1 + CH2 red, CH3 off |
| One (4) | CH1 + CH2 + CH3 red |
| Start (5) | All off |
| Gameplay (6) / Results (7) | Real connection data: green = correct, red = wrong resistor plugged in, off = socket empty |

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

## 6. Firewall

Since everything above uses `127.0.0.1` (remote server and TouchDesigner on
the same PC), Windows Firewall shouldn't need any changes for the OSC
traffic. The only inbound rule you may need is for the **web page's HTTP
port** (default `8080`) so other devices on the LAN can reach it — Node will
prompt for this automatically the first time it runs, if the popup doesn't
appear, allow `node.exe` for Private networks.
