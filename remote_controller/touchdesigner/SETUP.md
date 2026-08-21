# Wiring the remote controller into TouchDesigner

The remote controller server talks to TouchDesigner over OSC/UDP, the same
transport already used by the ESP32 boards -- just on different ports so it
never collides with the boards' telemetry on port 9000.

| Direction | Port | Purpose |
| --- | --- | --- |
| Remote → TD | `9001` (UDP) | Button presses (toggles + pulses) |
| TD → Remote | `9002` (UDP) | Heartbeat + current game state |

Both defaults live in [`../.env.example`](../.env.example) and can be changed there.

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

## 4. Firewall

Since everything above uses `127.0.0.1` (remote server and TouchDesigner on
the same PC), Windows Firewall shouldn't need any changes for the OSC
traffic. The only inbound rule you may need is for the **web page's HTTP
port** (default `8080`) so other devices on the LAN can reach it — Node will
prompt for this automatically the first time it runs, if the popup doesn't
appear, allow `node.exe` for Private networks.
