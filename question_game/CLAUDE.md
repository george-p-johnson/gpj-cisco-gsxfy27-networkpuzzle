# Arduino Resistor Match Question/Answer Game: System Reference & Guide

This file provides the comprehensive architectural, electrical, mathematical, and programmatic specification for the 3-Channel Resistor Match Question/Answer Game powered by the **Waveshare ESP32-S3** microcontroller. It is fully self-contained and serves as the single source of truth for understanding, modifying, or reproducing this system.

---

## 1. System Overview & Game Mechanics

The project implements a hardware-based, tactile matching game featuring **three question sockets** (channels) and corresponding **answer sockets**. 

* **The Patch Cord Interface:** A physical patch cord (carrying an embedded test resistor representing the selected answer) is plugged between a question socket and an answer socket.
* **Continuous Scanning:** The microcontroller continuously scans the three question sockets to identify which answer resistor (if any) is currently connected.
* **Match Indicator LEDs:** Each channel has a dedicated bi-color LED:
  - **Green LED:** Illuminates when the patch cord connects a question socket to its **correct** answer socket (as defined in software).
  - **Red LED:** Illuminates if the patch cord is connected to an **incorrect** answer socket, or is **unplugged** (open circuit).
* **Game Victory State:** A game-wide success signal is triggered only when all three channels have correct matches simultaneously.
* **Variable Configurations:** To allow questions to change dynamically from game to game, correct answers are stored in a simple, editable software array. This enables rapid game reconfigurability without any hardware rewiring.

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

---

## 5. Hardware Pin Mapping

The system relies on three discrete analog inputs and six digital output pins mapped to physical bi-color LEDs.

| Channel | physical Socket | ESP32-S3 Pin | LED RED GPIO | LED GREEN GPIO | Physical Header Pins & Location |
| :---: | :---: | :---: | :---: | :---: | :--- |
| **CH1** | Socket 1 | **GPIO 1** | **GPIO 16** | **GPIO 17** | Left Header Pins 32, 34 |
| **CH2** | Socket 2 | **GPIO 2** | **GPIO 38** | **GPIO 39** | Right Header Pins 14, 12 |
| **CH3** | Socket 3 | **GPIO 3** | **GPIO 34** | **GPIO 35** | Right Header Pins 19, 17 |

*Note: LED pins output digital `HIGH` to switch the selected color ON, and `LOW` to switch it OFF. Red and Green pins are operated mutually-exclusively per channel.*

---

## 6. Software Architecture & Game Flow

The game is structured using a simple polling-loop state machine. 

### A. Variable Array Structure
The core flexibility requirement is achieved through the global configuration array `correctAnswers[3]`. Each element stores the target resistor index corresponding to a specific channel:

```cpp
// Target resistor index matching table (0 = 470, 1 = 1k, ... 9 = 100k)
int correctAnswers[3] = {
  3,  // CH1 expects 4.7k (Index 3)
  7,  // CH2 expects 47k  (Index 7)
  5   // CH3 expects 22k  (Index 5)
};
```

### B. Logic Processing Flow
On every iteration of the `loop()`:
1. **Reset Victory Tracker:** A boolean flag `allCorrect` is initialized to `true`.
2. **Channel Polling:** For each channel `i` from `0` to `2`:
   - Measure the smoothed analog voltage ($V_{\text{out}}$) on `ANALOG_PINS[i]` after flushing the charge.
   - Map the voltage value to its resistor classification index `detectedIndex` using the Midpoint Decision boundaries.
   - Compare `detectedIndex` with the target `correctAnswers[i]`.
   - Update LEDs:
     - If matched (`detectedIndex == correctAnswers[i]`), set Green Pin `HIGH` and Red Pin `LOW`.
     - If mismatched or unplugged, set Red Pin `HIGH` and Green Pin `LOW`, and toggle `allCorrect = false`.
   - Output Serial telemetry details (measured mV, detected resistor name, match status).
3. **Master Win Assertion:** If `allCorrect` remains `true` after polling all three channels, output the unified game win telemetry (`*** GAME SOLVED: ALL PATCHES CORRECT! ***`).
4. **Throttle Delay:** A final $300\text{ms}$ delay throttles the scanning loop to conserve processing cycles and provide human-readable serial logs.

---

## 7. Operational Instructions for Game Administrators

### Changing Answers for a New Game
To re-index answers for a brand new game session:
1. Choose which physical answer resistor value should match each physical question socket (CH1, CH2, CH3).
2. Look up each resistor's **Index** from the **Adaptive Calibration & Auto-Span Database** (Section 4).
3. Locate the `correctAnswers[3]` array near the top of `question_game.ino`.
4. Update the indices in the array. For example, if you want:
   - CH1 to match $1\text{ k}\Omega$ (Index `1`)
   - CH2 to match $100\text{ k}\Omega$ (Index `9`)
   - CH3 to match $2.2\text{ k}\Omega$ (Index `2`)
   
   Modify the line to:
   ```cpp
   int correctAnswers[3] = { 1, 9, 2 };
   ```
5. Upload the sketch to the ESP32-S3 Dev Board. No physical circuit changes are necessary.
