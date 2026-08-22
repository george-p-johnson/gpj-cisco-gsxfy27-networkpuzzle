# Arduino Resistor Match Question/Answer Game: System Reference & Guide

This file provides the comprehensive architectural, electrical, mathematical, and programmatic specification for the 3-Channel Resistor Match Question/Answer Game powered by the **Waveshare ESP32-S3-ETH** microcontroller. It is fully self-contained and serves as the single source of truth for understanding, modifying, or reproducing this system.

---

## 1. System Overview & Game Mechanics

The installation is **six physical Waveshare ESP32-S3-ETH boards, all running the identical `question_game.ino` firmware**, each handling **three question sockets** (channels) and corresponding answer sockets. Which of the six boards a given unit is -- and therefore which questions it's responsible for -- is determined entirely by a config file on that board's SD card, not by anything hardcoded in the firmware. See **Section 6** for how that works.

* **The Patch Cord Interface:** A physical patch cord (carrying an embedded test resistor representing the selected answer) is plugged between a question socket and an answer socket.
* **Continuous Scanning:** The microcontroller continuously scans the three question sockets to identify which answer resistor (if any) is currently connected.
* **Match Indicator LEDs:** Each channel has a dedicated bi-color LED:
  - **Green LED:** Illuminates when the patch cord connects a question socket to its **correct** answer socket (as defined by that board's configuration).
  - **Red LED:** Illuminates if the patch cord is connected to an **incorrect** answer socket. An **unplugged** (open circuit) socket shows **off**, not red -- see Section 8B.
  - LED behavior above applies during **Gameplay**/**Results**; during **Idle**/**Three**/**Two**/**One**/**Start** the LEDs instead follow the countdown/lighting sequence driven by the controller (Section 7D, Section 8B/8C), independent of what's actually plugged in. During **Idle**, this means a randomized green flicker (Section 8C), mimicking an Ethernet switch port's activity LED, rather than solid off.
* **Game Victory State:** A per-board success signal is triggered only when all three channels have correct matches simultaneously.
* **Variable Configurations:** Correct answers are not hardcoded in the firmware. Each board reads its assigned questions and their correct answers from a shared `answer_table.tsv` on its SD card, selecting the right 3 rows based on a `device.cfg` file that names which of the six boards it is. This means the entire installation's questions can be re-authored by editing one spreadsheet-like file and copying it to six SD cards -- no reflashing, no hardware rewiring. See **Section 6**.
* **Networked Telemetry (Two-Way):** Each board reports its live channel states to a central game controller (TouchDesigner) over Ethernet via OSC, and also receives the current game state and per-player active flags back from the controller to drive its lighting -- a board whose player is inactive stays on idle lighting no matter what state the rest of the game is in. See **Section 7**.

---

## 2. Hardware Architecture & Electrical Design

The system determines matches by measuring the voltage drop across a custom voltage divider network.

### A. Circuit Diagram
Each of the three channels utilizes an identical, noise-insulated voltage divider circuit designed to stabilize readings and prevent capacitive/electrostatic interference:

```
                  +3.3V (VCC)
                    |
              [ 10k Ω Fixed ]  <--- Top Reference Resistor (R_top)
                    |
                    +-------------------> ESP32-S3 Analog Pin (GPIO 1, 2, or 3)
                    |               |
             [ Test Resistor ]   [ 100k Ω ] <--- Permanent Pull-Down Resistor (R_PD)
                 (R_test)          (R_PD)
                    |               |
                   GND             GND
```

### B. Component Role & Optimization Choices
* **Top Reference Resistor ($R_{\text{top}}$) = $10\text{ k}\Omega$:** Sits at the geometric center of our target resistor range ($470\ \Omega$ to $100\text{ k}\Omega$). This balances voltage step intervals across low and high values, preventing low-value resistors from falling into the ground noise floor and high-value resistors from compressing near the ADC saturation ceiling.
* **Permanent Pull-Down Resistor ($R_{\text{PD}}$) = $100\text{ k}\Omega$:** 
  - Prevents the analog pin from floating when a patch cord is unplugged. 
  - Forms a baseline voltage divider when the socket is empty (Open Circuit), locking the baseline reading cleanly at $\approx 3040\text{ mV}$.
  - Pulls high-end test values down, expanding the spacing between the highest resistor ($100\text{ k}\Omega$) and an open circuit into a wide, easily detectable $+238\text{ mV}$ gap.
* **Test Socket Resistor ($R_{\text{test}}$):** Placed in the answer socket. When plugged, it sits in parallel with the $100\text{ k}\Omega$ pull-down.

### C. Mathematical Model
When a patch cord is inserted, the lower resistance branch ($R_{\text{bottom}}$) becomes the parallel combination of the test resistor ($R_{\text{test}}$) and the permanent pull-down ($R_{\text{PD}}$):

$$R_{\text{bottom}} = \frac{R_{\text{test}} \times 100,000}{R_{\text{test}} + 100,000}$$

The voltage output ($V_{\text{out}}$) delivered to the ESP32-S3 ADC is:

$$V_{\text{out}} = 3300\text{ mV} \times \frac{R_{\text{bottom}}}{10,000 + R_{\text{bottom}}}$$

---

## 3. Signal Processing & ESP32-S3 ADC Workarounds

The ESP32-S3 features a successive-approximation register (SAR) ADC. Utilizing this hardware reliably across multiple rapid channels requires specific software workarounds.

### A. Multiplexer Residual Charge Flush
* **The Issue:** The ESP32-S3 shares a single internal ADC across its input pins. Rapidly switching the internal multiplexer between pins causes residual capacitive charge from the previous pin to leak into the next channel's measurement, producing false matches.
* **The Solution:** The reading function performs a single "throwaway" read (`analogReadMilliVolts(pin)`) and pauses for $100\ \mu\text{s}$ (`delayMicroseconds(100)`) to allow the multiplexer's internal capacitance to fully discharge before sampling.

### B. ADC Settings & Filtering
* **ADC Settings:** 12-bit resolution (`0` to `4095`) and $11\text{dB}$ attenuation (`ADC_11db`), mapping the input range across the full $0\text{V} - 3.3\text{V}$ scale.
* **Filtering Noise:** To smooth high-frequency voltage fluctuations and EMI, the software averages 15 separate ADC samples over a $15\text{ms}$ interval for each channel.
* **Noise Floor:** Any voltage reading below $75\text{ mV}$ is explicitly discarded as skin-touch capacitive noise or stray ground loop voltage and mapped to `NONE`.

### C. Classification Debounce
A patch cord passes through transient partial-contact states while being inserted or removed, which can momentarily read as a different resistor value than the one actually seated (or produce a fast, wide voltage sweep if the cord is being held/wiggled rather than fully seated -- this is expected behavior, not a fault).

* **`DEBOUNCE_MS` (default `225`):** A classification must hold steady for this many milliseconds before it's promoted to `stableIndex[]` -- the value actually used for LED state and match-checking.
* **Timer-based, not loop-count-based:** uses `millis()` so the debounce duration is exact regardless of the main loop's throttle delay (Section 8B/8C).
* While a new classification is still within its debounce window (`detectedIndex != stableIndex`), the LED is deliberately held **off** rather than flashing red, so a cord being plugged in doesn't strobe red before settling green.

---

## 4. Adaptive Calibration & Auto-Span Architecture

Rather than relying on brittle, hardcoded threshold numbers, the firmware implements a professional, self-normalizing hardware-calibration pipeline.

### A. Measured Ranges Database
Your raw, physical measurements are stored as a static array of range structures (`minMV` and `maxMV`) normalized to a standard $3300\text{ mV}$ rail scale:

| Resistor Class | Your Empirical Range | Calculated Gap Midpoint |
| :--- | :---: | :---: |
| **Noise / Touch** | $0 - 74\text{ mV}$ | — |
| **$470\ \Omega$** | $112 - 186\text{ mV}$ | $218\text{ mV}$ |
| **$1\text{ k}\Omega$** | $250 - 326\text{ mV}$ | $432\text{ mV}$ |
| **$2.2\text{ k}\Omega$** | $539 - 608\text{ mV}$ | $787\text{ mV}$ |
| **$4.7\text{ k}\Omega$** | $967 - 1041\text{ mV}$ | $1283\text{ mV}$ |
| **$10\text{ k}\Omega$** | $1525 - 1596\text{ mV}$ | $1837\text{ mV}$ |
| **$22\text{ k}\Omega$** | $2078 - 2131\text{ mV}$ | $2215\text{ mV}$ |
| **$33\text{ k}\Omega$** | $2300 - 2374\text{ mV}$ | $2387\text{ mV}$ |
| **$47\text{ k}\Omega$** | $2401 - 2551\text{ mV}$ | $2583\text{ mV}$ |
| **$68\text{ k}\Omega$** | $2615 - 2682\text{ mV}$ | $2703\text{ mV}$ |
| **$100\text{ k}\Omega$** | $2725 - 2803\text{ mV}$ | $2921\text{ mV}$ |
| **NONE (Open)** | $\approx 3040\text{ mV}$ | $> 2921\text{ mV}$ |

You can easily tweak these ranges directly inside the `nominalRanges[]` array in the Arduino IDE if physical components are changed!

### B. Runtime Boundary Calculation & Threshold Bias
The software automatically calculates decision boundaries between adjacent resistors at runtime:
$$\text{Threshold}[i] = \text{Max}_i + (\text{Min}_{i+1} - \text{Max}_i) \times \text{THRESHOLD\_BIAS}$$

* **`THRESHOLD_BIAS` parameter (Default: `0.3`):** This customizable constant defines where the boundary is placed within the unassigned gaps.
  - Setting it to `0.5` places boundaries exactly in the mathematical center (midpoint).
  - Setting it to `0.3` biases thresholds lower in the gap (closer to the lower resistor). This gives the higher resistor a massive **downward safety cushion** to tolerate loose wires, power supply dips, or resistor tolerances without misclassifying!

### C. Continuous & Startup Auto-Span Calibration
To completely neutralize chip-to-chip and pin-to-pin ADC gain variations, the software runs an intelligent calibration loop:

1. **Boot-Time reference Scan:** When the ESP32-S3 starts, the program scans all three pins. If you boot with patch cables already plugged in, it searches for any **single empty pin** (reading $> 2850\text{ mV}$) to compute the chip's global scale factor and instantly calibrates *all three* channels on startup. If all three channels are plugged on boot, it defaults to a scale factor of `1.0`.
2. **Continuous Normalization:** Whenever a pin becomes empty during gameplay, the system dynamically measures the raw open-circuit voltage $V_{\text{raw\_open}}$ and updates its scale multiplier.
3. **Aligned Target:** The calibration standard target is aligned to **`3040.0 mV`** (your physical board's natural open baseline). This ensures that on your hardware, the scale factors naturally calibrate to exactly `1.0`, resulting in $100\%$ consistent behavior whether you boot with cables plugged or unplugged:
$$\text{scaleFactor}[i] = \frac{3040.0}{V_{\text{raw\_open}}[i]}$$
All subsequent resistor readings on Pin $i$ are multiplied by `scaleFactor[i]` before classification. This guarantees that all three sockets behave identically on any physical board!

### D. Known Limitation: Single-Point Calibration
`pinScaleFactors[i]` is a single multiplicative correction, anchored **only at the open-circuit point** (~3040mV). This fully corrects for proportional gain differences (e.g. ADC reference tolerance) but does **not** fully correct for a resistor value difference in that channel's own `R_top` -- a tolerance error there distorts the voltage curve non-linearly, so the open-circuit point calibrates back to exactly right while a mid-range reading can still land in the wrong bin.

**Observed case:** on one board, a genuine 47kΩ resistor on CH2 consistently read as 68kΩ (readings clustering at 2460-2620mV, straddling the 2570mV cutoff between those two bins), while CH1/CH3 read the same resistor correctly. Root cause is almost certainly CH2's `R_top` being off nominal 10kΩ. If you see a channel that's consistently one band off (not jittering randomly, and not a partially-seated cord -- see Section 3C), measure that channel's `R_top`/`R_PD` against a known-good channel on the same board before assuming a firmware bug.

---

## 5. Hardware Pin Mapping

### A. Question Channels (Analog Inputs & LEDs)

| Channel | physical Socket | ESP32-S3 Pin | LED RED GPIO | LED GREEN GPIO | Physical Header Pins & Location |
| :---: | :---: | :---: | :---: | :---: | :--- |
| **CH1** | Socket 1 | **GPIO 1** | **GPIO 45** | **GPIO 42** | *(pending verification -- moved off GPIO34/35, which are internally reserved on this board; see FAQ note below)* |
| **CH2** | Socket 2 | **GPIO 2** | **GPIO 38** | **GPIO 39** | Right Header Pins 14, 12 |
| **CH3** | Socket 3 | **GPIO 3** | **GPIO 34** | **GPIO 35** | Right Header Pins 19, 17 |

*Note: LED pins output digital `HIGH` to switch the selected color ON, and `LOW` to switch it OFF. Red and Green pins are operated mutually-exclusively per channel.*

*Note: GPIO33-37 are internally occupied on this board (Waveshare's ESP32-S3-ETH FAQ; believed to be Octal PSRAM data lines) and must not be driven as outputs. CH1 was originally wired to GPIO34/35, which are inside that range -- driving them corrupted state that crashed the SD card driver. Confirmed independently on both pins before moving CH1 to GPIO45/42.*

### B. SD Card (onboard TF slot)

| Function | GPIO |
| :--- | :---: |
| CS (SS) | 4 |
| DI (MOSI) | 6 |
| DO (MISO) | 5 |
| SCK (SCLK) | 7 |

Confirmed against Waveshare's official ESP32-S3-ETH wiki `SD_Card` demo pin table.

### C. Ethernet (onboard W5500)

| Function | GPIO |
| :--- | :---: |
| MISO | 12 |
| MOSI | 11 |
| SCLK | 13 |
| CS | 14 |
| RST | 9 |
| INT | 10 |

Confirmed against Waveshare's official ESP32-S3-ETH wiki `ETH_DHCP`/`ETH_StaticIP` demo pin table.

### D. Critical: SD and Ethernet Must Use Separate SPI Buses
Both peripherals are SPI devices sharing the same physical chip, but must be initialized on **independent** `SPIClass` instances/hosts:

* **SD card uses the default global `SPI` object** (not a separately-hosted `SPIClass`). An explicit `SPIClass(FSPI)` instance was empirically confirmed to **hang `SD.begin()`** on this board with an `Interrupt wdt timeout on CPU1` panic, even though the pins and card were verified good via an isolated test using the default `SPI` object instead. Root cause not fully confirmed, but the fix (use the default object) is solid and reproducible.
* **Ethernet uses a separately-hosted `SPIClass ethSPI(HSPI)`.** This was validated standalone (see `ETH_Test/ETH_Test.ino`) before being integrated alongside the (already-working) SD setup, specifically to avoid repeating the SD/FSPI failure mode blind.
* **If you ever need to touch either bus's init code:** re-validate with the relevant isolated test sketch (`SD_Test/`, `ETH_Test/`) before assuming a change is safe. This board's SPI behavior has not been trustworthy to reason about from documentation alone.

---

## 6. SD Card & Multi-Device Configuration

The same `question_game.ino` firmware runs unmodified on all six boards. Each board's SD card tells it which of the six it is, and supplies the shared answer key it reads its 3 correct answers from.

### A. Files on Every Board's SD Card (root directory)

| File | Purpose |
| :--- | :--- |
| `device.cfg` | This board's identity and (optionally) network override. Unique per board. |
| `answer_table.tsv` | The full question/answer key for all 6 boards. **Identical on every card** -- copy the same file to all six. |

### B. `device.cfg` Format
Plain text, `key=value` per line:

```
waveshare_index=4
```

* **`waveshare_index`** (required, `1`-`6`): which of the six physical boards this SD card belongs to. Falls back to `1` with a Serial warning if missing, unparseable, or out of range.
* **`controller_ip`** (optional): overrides where this board sends OSC telemetry (see Section 7). Falls back to the default `192.168.50.100` if absent.

Six ready-to-copy templates (`device_1.cfg` through `device_6.cfg`) live in [`sd_card_templates/`](../sd_card_templates/) at the repo root -- copy the one matching the board, renaming it to `device.cfg` on the card.

### C. `answer_table.tsv` Format
Tab-separated, header row first:

```
question	answer	ohms	index
P1q1	P1a1	1000	1
P1q2	P1a9	100000	9
P1q3	P1a7	47000	7
...
```

`index` is the resistor classification index (0-9, see Section 4A) that the firmware actually compares against -- `ohms`/`answer` columns are for human reference and aren't parsed by the firmware.

### D. Board Index → Question Mapping
Devices 1/2 serve panel P1, devices 3/4 serve panel P2, devices 5/6 serve panel P3. Within a panel, the first device takes questions 1-3, the second takes questions 4-6:

| `waveshare_index` | Panel | Questions |
| :---: | :---: | :--- |
| 1 | P1 | q1, q2, q3 |
| 2 | P1 | q4, q5, q6 |
| 3 | P2 | q1, q2, q3 |
| 4 | P2 | q4, q5, q6 |
| 5 | P3 | q1, q2, q3 |
| 6 | P3 | q4, q5, q6 |

Computed at boot by `computeQuestionIDs()`: `panel = ((index-1)/2)+1`, `startQuestion = ((index-1)%2)*3+1`.

### E. Boot Sequence & Fallback Behavior
1. SD card mounted on the default `SPI` bus (see Section 5D).
2. `loadDeviceIndex()` reads `waveshare_index` from `device.cfg`.
3. `loadAnswersFromSD()` computes this board's 3 question IDs and scans `answer_table.tsv` for matching rows, filling `correctAnswers[]` from the `index` column.
4. `loadControllerIP()` reads an optional `controller_ip` override.

**If the SD card is missing, unmountable, or a question isn't found in the table:** the firmware logs a warning to Serial and keeps whatever is in the hardcoded fallback `correctAnswers[3]` array in the `.ino` for that channel, rather than halting. This keeps bench-testing possible without an SD card present, at the cost of that channel's answer possibly being wrong until the SD card issue is fixed.

### F. Diagnostic Tool
[`SD_Test/SD_Test.ino`](../SD_Test/SD_Test.ino) is a minimal standalone sketch (mount, write, list, read) for validating the SD card and bus in isolation, independent of game logic -- use this first if SD-related symptoms come up again.

---

## 7. Ethernet & OSC Telemetry

Each board reports its live channel states to a central game controller PC (running TouchDesigner) over Ethernet, via OSC over UDP.

### A. Network Topology
* All six boards and the controller PC connect to a single Ethernet switch (not directly to each other).
* **Static IP addressing**, not DHCP -- a plain switch has no DHCP server, and static addressing is more robust for a fixed installation anyway (deterministic on every power-up).
* **Subnet:** `192.168.50.0/24`.
* **Board IP formula:** `192.168.50.(10 + waveshare_index)` -- e.g. device 4 is `192.168.50.14`.
* **Controller PC:** set to a static IP on the same subnet, conventionally `192.168.50.100`, mask `255.255.255.0`.
* **Power:** boards use the Waveshare PoE add-on module (IEEE 802.3af). Requires either a PoE-capable switch/injector, or USB-C power per board if PoE hardware isn't in the path yet -- the two are independent of network config.

### B. OSC Message Format
Sent once per `loop()` iteration (~every 95ms, see Section 8B's Throttle Delay step) once the Ethernet link is up. Fire-and-forget UDP -- if nothing is listening, packets are silently dropped with no effect on game logic.

* **Address:** `/waveshare/<waveshare_index>` (e.g. `/waveshare/4`)
* **Port:** `9000` (destination), sent to `controllerIP` (default `192.168.50.100`, overridable via `device.cfg`'s `controller_ip`)
* **Args (8 int32, in order):**

| # | Arg | Meaning |
| :---: | :--- | :--- |
| 1 | `ch1Index` | CH1's debounced resistor classification (0-9, 10=NONE -- see Section 4A) |
| 2 | `ch1Match` | `1` if CH1 matches its correct answer, else `0` |
| 3 | `ch2Index` | CH2's classification |
| 4 | `ch2Match` | `1` if CH2 matches |
| 5 | `ch3Index` | CH3's classification |
| 6 | `ch3Match` | `1` if CH3 matches |
| 7 | `allCorrect` | `1` only when all three channels are matched simultaneously (this board's puzzle solved) |
| 8 | `heartbeat` | Increments on every send (wraps, don't rely on the exact value) -- always changes even when args 1-7 don't, so a listener can distinguish "board alive, unchanged reading" from "board gone" |

The OSC packet encoder is hand-rolled (`sendOSCInts()` in `question_game.ino`) rather than a third-party library, to avoid an extra dependency across six boards. It only supports int32 args, which is all this project currently needs.

### C. Firmware Implementation Notes
* Ethernet runs on its own SPI bus (`ethSPI` on `HSPI`) -- see Section 5D for why this must be separate from SD's bus.
* `ETH.begin()` happens in `setup()`, **after** the SD card block, because the static IP depends on `deviceIndex` having already been loaded.
* `ethConnected` is tracked via the `Network.onEvent()` callback (`ARDUINO_EVENT_ETH_GOT_IP` / `..._LOST_IP` / `..._DISCONNECTED`) and gates whether `sendGameStateOSC()` actually sends -- avoids trying to send before the link is ready.

### D. Controller → Board: Game-State Lighting & Player-Active State
Boards also **listen** for incoming UDP: TouchDesigner broadcasts the current game state (and which players are active) to `192.168.50.255:9003`, and each board reacts by driving its LEDs per Section 8B. This reuses the same `udp` object as outgoing telemetry (`udp.begin(STATE_LISTEN_PORT)` in `setup()`, once Ethernet is up) -- `NetworkUDP` supports simultaneous send (`beginPacket`/`write`/`endPacket`) and receive (`parsePacket`/`read`) on one instance, no second socket needed.

Two addresses are handled on this port:

| Address | Args | Purpose |
| :--- | :--- | :--- |
| `/game_state` | 1 int32 | The game state, using the same 1-7 convention the remote controller uses (see `../remote_controller/README.md`) |
| `/player_active` | 3 int32 (p1, p2, p3 as 0/1) | Which of the 3 players are currently active -- see below |

`/game_state` values:

| Value | State |
| :---: | --- |
| 1 | Idle |
| 2 | Three |
| 3 | Two |
| 4 | One |
| 5 | Start |
| 6 | Gameplay |
| 7 | Results |

`currentGameState` defaults to `1` (Idle) at boot, before TD has sent anything. `playerActive[3]` defaults to all-`true` at boot, so a board's lighting is unchanged unless/until TD actually sends `/player_active`.

**Player-active override:** Each board knows which player it belongs to (`myPlayer`, computed at boot from `deviceIndex` -- boards 1/2 -> P1, 3/4 -> P2, 5/6 -> P3, same mapping as Section 6D). If that player is inactive (`playerActive[myPlayer-1] == false`), the board treats itself as Idle for LED purposes regardless of the actual `currentGameState` -- see `boardIdleOverride`/`effectiveState` in `loop()`. This lets a multiplayer variant leave un-used players' boards flickering idle throughout Three/Two/One/Gameplay/Results instead of running the countdown/match lighting for a player who isn't playing. Readings, matching, and OSC telemetry are unaffected -- only the LED decision is overridden.

Parsing (`checkForStateUpdate()` in `question_game.ino`) is a minimal hand-rolled OSC reader, mirroring the hand-rolled encoder already used for outgoing telemetry -- same reasoning, avoids a third-party OSC library dependency. It now branches on the address and type-tag arg count to support both 1-arg and 3-arg messages on the same socket.

### E. Diagnostic Tool
[`ETH_Test/ETH_Test.ino`](../ETH_Test/ETH_Test.ino) is a minimal standalone sketch (link up, static IP, no SD/game logic) for validating Ethernet connectivity in isolation -- use this first if network-related symptoms come up again, before assuming the integration code is at fault.

---

## 8. Software Architecture & Game Flow

The game is structured using a simple polling-loop state machine.

### A. Variable Array Structure
Correct answers live in the global `correctAnswers[3]` array, but **this is populated at boot by `loadAnswersFromSD()`** (see Section 6), not edited directly for normal operation. The array's inline values only serve as the bench-test fallback used when the SD card is unavailable:

```cpp
// Fallback/bench-test values only -- normally overwritten at boot from SD.
int correctAnswers[3] = {
  3,  // CH1 expects 4.7k (Index 3)
  7,  // CH2 expects 47k  (Index 7)
  5   // CH3 expects 22k  (Index 5)
};
```

### B. Logic Processing Flow
On every iteration of the `loop()`:
0. **Check for a game-state update:** `checkForStateUpdate()` (Section 7D) polls for an incoming `/game_state` or `/player_active` broadcast and updates `currentGameState`/`playerActive[]` if one arrived; otherwise it's a no-op and the previous values carry over. `updateIdleFlicker()` then advances each channel's randomized idle-flicker timer (Section 8C) unconditionally, so its timing stays continuous whether or not it's currently visible.
1. **Compute effective state:** `boardIdleOverride = !playerActive[myPlayer-1]`; `effectiveState = boardIdleOverride ? 1 : currentGameState`. All LED decisions below use `effectiveState`, not `currentGameState` directly -- see Section 7D.
2. **Reset Victory Tracker:** A boolean flag `allCorrect` is initialized to `true`.
3. **Channel Polling:** For each channel `i` from `0` to `2`:
   - Measure the smoothed analog voltage ($V_{\text{out}}$) on `ANALOG_PINS[i]` after flushing the charge.
   - Map the voltage value to its resistor classification index `detectedIndex` using the Midpoint Decision boundaries.
   - Apply debounce (Section 3C) to promote `detectedIndex` to `stableIndex[i]`.
   - Compare `stableIndex[i]` with the target `correctAnswers[i]` -- this comparison (`matched[i]`, `allCorrect`) always runs and is always sent in telemetry, regardless of `currentGameState`/`effectiveState`.
   - Update LEDs, gated by `effectiveState`:
     - **Idle:** flickering green (Section 8C), regardless of what's plugged in.
     - **Start:** off, regardless of what's plugged in.
     - **Three / Two / One:** a countdown, independent of actual readings -- one more channel lights red per step (CH1 only → CH1+CH2 → all three).
     - **Gameplay / Results:** reflects the real reading -- green if matched, red if plugged but wrong, off if the socket is empty (unplugged) or the classification is still debouncing.
   - Output Serial telemetry details (measured mV, detected resistor name, match status).
4. **Master Win Assertion:** If `allCorrect` remains `true` after polling all three channels, output the unified game win telemetry (`*** GAME SOLVED: ALL PATCHES CORRECT! ***`).
5. **OSC Telemetry:** Send this board's current state to the game controller (Section 7B).
6. **Throttle Delay:** A final $50\text{ms}$ delay throttles the scanning loop. Originally $300\text{ms}$ (to conserve processing cycles and provide human-readable serial logs), but shortened -- since `checkForStateUpdate()` (step 0) only runs once per loop pass, this delay is also the dominant term in the worst-case latency between TD sending `/game_state`/`/player_active` and this board's LEDs reflecting it. At $50\text{ms}$ (plus ~$45\text{ms}$ of ADC averaging below it, for a ~$95\text{ms}$ total loop period), that latency stays small relative to the 1-second Three/Two/One countdown states on stage. Neither ESP32-S3 processing headroom nor Serial throughput (already non-blocking via `Serial.setTxTimeoutMs(0)`) are a concern at this rate.

### C. Idle Lighting: "Ethernet Switch" Flicker
While a channel is showing Idle lighting (either because `currentGameState` really is Idle, or because `boardIdleOverride` forces it there for an inactive player -- Section 7D), the LED flickers green instead of sitting solid or off, mimicking an Ethernet switch port's link/activity LED.

* `updateIdleFlicker()` runs unconditionally at the top of every `loop()` iteration (not gated by state), so each channel's flicker timing free-runs continuously rather than restarting whenever a board re-enters Idle.
* Each of the 3 channels has its own independent on/off timer (`flickerOn[3]`, `flickerNextToggleMs[3]`) -- they are not synchronized, so the three LEDs flicker at different, random moments, like independent switch ports rather than a single synchronized blink.
* Each toggle rolls a new random duration: ON bursts are short (`40-150ms`), OFF gaps are longer (`150-600ms`) -- mostly-off punctuated by quick flashes, matching the look of intermittent traffic rather than a steady blink.
* The main loop advances roughly every ~95ms (Section 8B step 6), so flicker updates happen at that cadence in practice -- still clearly irregular and non-synchronized across channels, and now close enough to the shortest ON burst (40ms) that the flicker reads as genuinely quick rather than just "irregular."

---

## 9. Operational Instructions for Game Administrators

### A. Changing Questions/Answers for a New Game
Answers are authored once, centrally, and pushed to all six SD cards -- not edited in firmware.

1. Edit `answer_table.tsv` (repo root) -- update the `ohms`/`index` columns for whichever question rows are changing. Use the **Index** column values from the **Adaptive Calibration & Auto-Span Database** (Section 4A).
2. Copy the updated `answer_table.tsv` to the root of **all six** boards' SD cards (it's identical across all of them).
3. No firmware reflash needed -- each board re-reads the table at next boot.

### B. Setting Up a New Board's SD Card
1. Format the microSD card FAT32 (not exFAT).
2. Copy `answer_table.tsv` (repo root) to the card's root, unchanged.
3. Copy the matching template from [`sd_card_templates/`](../sd_card_templates/) (`device_1.cfg` through `device_6.cfg`) to the card's root, **renamed to `device.cfg`**.
4. Optionally add a `controller_ip=` line to that `device.cfg` if this board needs to report telemetry somewhere other than the default `192.168.50.100`.
5. Insert into the board and power on -- check Serial output for `>> Waveshare device index: N` and the resulting question assignments to confirm it loaded correctly.

### C. Flashing Firmware
Same `question_game.ino` sketch for all six boards -- flash over USB-C (native USB CDC on the ESP32-S3; there is no network/OTA flashing capability currently built in). No per-board firmware differences; all board-specific behavior comes from that board's SD card.

### D. Troubleshooting Reference
* **SD card issues** (mount failures, hangs, wrong/missing config): see Section 6E-F. Use `SD_Test/SD_Test.ino` to isolate SD from the rest of the system.
* **Ethernet/network issues** (no link, no OSC data reaching the controller): see Section 7E. Use `ETH_Test/ETH_Test.ino` to isolate Ethernet from the rest of the system.
* **A channel consistently misreads one resistor band off** (not jittering, not a loose cord): see Section 4D -- likely that channel's `R_top`/`R_PD` resistor tolerance, not a firmware bug.
* **A reading sweeps rapidly across many resistor bands while a cord is being handled**: expected behavior (Section 3C), not a fault -- re-check once the cord is fully seated and untouched.
* **Any new GPIO assignment on this board**: cross-check against Section 5's pin tables and the GPIO33-37 reservation note before wiring/coding against it. This board's undocumented pin reservations have caused real, hard-to-diagnose crashes before.
