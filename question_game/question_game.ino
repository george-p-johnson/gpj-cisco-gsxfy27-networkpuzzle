#include <Arduino.h>

enum LEDState { LED_OFF, LED_RED, LED_GREEN, LED_YELLOW };

// --- LED BRIGHTNESS ---
// PWM duty cycle per state: 0 (off) - 255 (full brightness). Tweak to taste.
const int LED_BRIGHTNESS_FULL = 255;                    // red / green
const int LED_BRIGHTNESS_YELLOW_RED = LED_BRIGHTNESS_FULL / 4;   // yellow's red die, quarter brightness
const int LED_BRIGHTNESS_YELLOW_GREEN = LED_BRIGHTNESS_FULL / 8; // yellow's green die, eighth brightness

// --- ANALOG PIN DEFINITIONS ---
// CH1: GPIO 1 | CH2: GPIO 2 | CH3: GPIO 3
const int ANALOG_PINS[3] = { 1, 2, 3 }; 

// --- LED GPIO DEFINITIONS ---
// CH1 (Socket 1) - Header Pins 32, 34
const int CH1_RED   = 39;
const int CH1_GREEN = 40;

// CH2 (Socket 2) - Header Pins 14, 12
const int CH2_RED   = 36;
const int CH2_GREEN = 37;

// CH3 (Socket 3) - Header Pins 19, 17
const int CH3_RED   = 33;
const int CH3_GREEN = 34;

struct SocketChannel {
  int analogPin;
  int redPin;
  int greenPin;
};

SocketChannel channels[3] = {
  { ANALOG_PINS[0], CH1_RED, CH1_GREEN },
  { ANALOG_PINS[1], CH2_RED, CH2_GREEN },
  { ANALOG_PINS[2], CH3_RED, CH3_GREEN }
};

struct ResistorRange {
  int minMV;
  int maxMV;
  const char* name;
};

// --- EMPIRICAL MEASURED RANGES (Normalized to nominal 3300mV scale) ---
// You can easily edit these values directly in your IDE if you observe drift!
const ResistorRange nominalRanges[10] = {
  { 112,  186, "470"  },
  { 250,  326, "1k"   },
  { 539,  608, "2.2k" },
  { 967, 1041, "4.7k" },
  { 1525, 1596, "10k"  },
  { 2078, 2131, "22k"  },
  { 2300, 2374, "33k"  },
  { 2401, 2551, "47k"  }, // Adjusted min to 2401 per empirical drift observation
  { 2615, 2682, "68k"  },
  { 2725, 2803, "100k" }
};

// Helper function to print exactly 4 characters for millivolts (e.g. " 205" or "2382")
void print4DigitMV(int mv) {
  if (mv < 1000) Serial.print(" ");
  if (mv < 100) Serial.print(" ");
  if (mv < 10) Serial.print(" ");
  Serial.print(mv);
}

// --- DECISION BOUNDARY THRESHOLD BIAS ---
// Tweak this value to shift how decision thresholds are calculated in gaps:
// 0.5 = Exact midpoint of the gap (50% / 50%)
// 0.3 = Shipped 30% of the gap (closer to lower resistor, giving the higher resistor more downward cushion)
const float THRESHOLD_BIAS = 0.3;

// --- DYNAMIC AUTO-SPAN CORRECTION FACTORS ---
// Continuously self-normalizes each ADC channel individually using empty sockets
float pinScaleFactors[3] = { 1.0, 1.0, 1.0 };

// --- CLASSIFICATION DEBOUNCE ---
// A patch cord passes through transient partial-contact states while it's being
// inserted (or removed), which can momentarily read as a different resistor value
// than the one actually seated. Requiring a classification to hold steady for
// DEBOUNCE_MS before accepting it filters out that insertion/removal noise.
// Timer-based (rather than counting loop passes) so this duration is exact
// regardless of the main loop's throttle delay.
const unsigned long DEBOUNCE_MS = 450;
int stableIndex[3] = { 10, 10, 10 };           // last accepted (debounced) classification per channel
int candidateIndex[3] = { 10, 10, 10 };        // classification currently being confirmed
unsigned long candidateSinceMs[3] = { 0, 0, 0 }; // when candidateIndex[i] first appeared

const char* getResistorName(int idx) {
  if (idx >= 0 && idx < 10) return nominalRanges[idx].name;
  return "NONE";
}

// --- GAME ANSWER CONFIGURATION ---
// Index mappings correspond to nominalRanges above:
// 0=470R, 1=1k, 2=2.2k, 3=4.7k, 4=10k, 5=22k, 6=33k, 7=47k, 8=68k, 9=100k, 10=NONE
// Set the correct answer resistor index for each question socket (CH1, CH2, CH3)
int correctAnswers[3] = {
  3,  // CH1 correct answer is 4.7k Ohm (Index 3)
  7,  // CH2 correct answer is 47k Ohm  (Index 7)
  5   // CH3 correct answer is 22k Ohm  (Index 5)
};

int getComputedThreshold(int idx) {
  if (idx < 0 || idx >= 9) return 2921; // fallback
  int maxLower = nominalRanges[idx].maxMV;
  int minUpper = nominalRanges[idx + 1].minMV;
  int gap = minUpper - maxLower;
  return maxLower + (int)(gap * THRESHOLD_BIAS);
}

int getResistorIndex(int mv) {
  // Discard stray skin touch / ground noise below 75mV
  if (mv < 75) return 10; 

  // The open-circuit (NONE) boundary is computed above the 100k Max range
  // using your board's natural empty socket baseline target of 3040mV.
  int openThreshold = nominalRanges[9].maxMV + (int)((3040 - nominalRanges[9].maxMV) * THRESHOLD_BIAS);
  if (mv > openThreshold) return 10;

  int idx = 0;
  while (idx < 9) {
    int threshold = getComputedThreshold(idx);
    if (mv <= threshold) {
      return idx;
    }
    idx++;
  }
  return 9; // 100k Ohm
}

int readPinMV(int pin) {
  analogReadMilliVolts(pin); // Flush residual charge from ADC multiplexer
  delayMicroseconds(100);

  long sum = 0;
  for (int i = 0; i < 15; i++) {
    sum += analogReadMilliVolts(pin);
    delay(1);
  }
  return sum / 15;
}

void setLEDState(int redPin, int greenPin, LEDState state) {
  // Driving both dies of the bi-color LED at once reads as yellow/amber.
  // PWM (analogWrite) so each state's brightness is independently adjustable.
  int redLevel = 0;
  int greenLevel = 0;
  if (state == LED_RED) {
    redLevel = LED_BRIGHTNESS_FULL;
  } else if (state == LED_GREEN) {
    greenLevel = LED_BRIGHTNESS_FULL;
  } else if (state == LED_YELLOW) {
    redLevel = LED_BRIGHTNESS_YELLOW_RED;
    greenLevel = LED_BRIGHTNESS_YELLOW_GREEN;
  }
  analogWrite(redPin, redLevel);
  analogWrite(greenPin, greenLevel);
}

void printActiveThresholds() {
  Serial.println("\n========================================================================");
  Serial.print("ACTIVE CLASSIFICATION THRESHOLDS (THRESHOLD_BIAS: ");
  Serial.print(THRESHOLD_BIAS, 2);
  Serial.println(")");
  Serial.println("========================================================================");
  Serial.println("Resistor Gap    | Normalized | CH1 Raw Threshold | CH2 Raw   | CH3 Raw");
  Serial.println("------------------------------------------------------------------------");

  const char* gapNames[10] = {
    "470 vs 1k     ",
    "1k vs 2.2k    ",
    "2.2k vs 4.7k  ",
    "4.7k vs 10k   ",
    "10k vs 22k    ",
    "22k vs 33k    ",
    "33k vs 47k    ",
    "47k vs 68k    ",
    "68k vs 100k   ",
    "100k vs NONE  "
  };

  for (int idx = 0; idx < 10; idx++) {
    int normThreshold;
    if (idx < 9) {
      normThreshold = getComputedThreshold(idx);
    } else {
      normThreshold = nominalRanges[9].maxMV + (int)((3040 - nominalRanges[9].maxMV) * THRESHOLD_BIAS);
    }

    Serial.print(gapNames[idx]);
    Serial.print(" | ");
    print4DigitMV(normThreshold);
    Serial.print(" mV     | ");

    // Print raw threshold for each channel based on its calibration
    for (int ch = 0; ch < 3; ch++) {
      int rawThreshold = (int)((float)normThreshold / pinScaleFactors[ch]);
      print4DigitMV(rawThreshold);
      Serial.print(" mV");
      if (ch < 2) {
        Serial.print("         | ");
      }
    }
    Serial.println();
  }
  Serial.println("------------------------------------------------------------------------\n");
}

void setup() {
  Serial.begin(115200);
  Serial.setTxTimeoutMs(0); 

  // Initialize ESP32-S3 ADC
  analogReadResolution(12);       // 12-bit (0 - 4095)
  analogSetAttenuation(ADC_11db); // Full ~0V to 3.3V range

  // Initialize LED Pins
  for (int i = 0; i < 3; i++) {
    pinMode(channels[i].redPin, OUTPUT);
    pinMode(channels[i].greenPin, OUTPUT);
  }

  delay(2000); 
  Serial.println("\n========================================================================");
  Serial.println("         RESISTOR MATCH GAME: MULTI-CHANNEL QUESTION & ANSWER           ");
  Serial.println("========================================================================\n");

  // --- BOOT-TIME GLOBAL ADC SCAN ---
  // If the user starts the board with patch cords already plugged in, we scan for 
  // any SINGLE open pin to determine the chip's global gain scale factor. 
  float globalScaleFactor = 1.0;
  bool foundOpenPin = false;

  Serial.println("Starting Boot-Time Channel Diagnostics...");
  for (int i = 0; i < 3; i++) {
    int rawOpen = readPinMV(channels[i].analogPin);
    Serial.print("CH");
    Serial.print(i + 1);
    Serial.print(" Startup Voltage: ");
    Serial.print(rawOpen);
    Serial.println(" mV");

    if (!foundOpenPin && rawOpen > 2850) {
      globalScaleFactor = 3040.0 / (float)rawOpen;
      foundOpenPin = true;
      Serial.print(">> Empty socket found on CH");
      Serial.print(i + 1);
      Serial.print(". Initializing global scale factor: ");
      Serial.println(globalScaleFactor, 4);
    }
  }

  // Set all channel scale factors using our best calibration info
  for (int i = 0; i < 3; i++) {
    pinScaleFactors[i] = globalScaleFactor;
  }

  if (!foundOpenPin) {
    Serial.println(">> WARNING: All sockets are plugged on boot. Using default nominal scale factors (1.0000).");
  }

  // Display the calculated active midpoints/thresholds
  printActiveThresholds();

  Serial.println("Diagnostics complete. Game loop started!\n");
}

void loop() {
  bool allCorrect = true;

  for (int i = 0; i < 3; i++) {
    int rawMV = readPinMV(channels[i].analogPin);

    // Continuous Auto-Calibration:
    // If raw voltage is in open-circuit range (above 2850mV), dynamically calibrate
    // this channel's scale factor against your board's natural target of 3040mV.
    if (rawMV > 2850) {
      pinScaleFactors[i] = 3040.0 / (float)rawMV;
    }

    // Apply scaling factor to get normalized voltage
    int normalizedMV = (int)(rawMV * pinScaleFactors[i]);

    int detectedIndex = getResistorIndex(normalizedMV);

    // Debounce: only accept a new classification once it has read consistently
    // for DEBOUNCE_MS milliseconds (see declaration above).
    if (detectedIndex != candidateIndex[i]) {
      candidateIndex[i] = detectedIndex;
      candidateSinceMs[i] = millis();
    } else if (millis() - candidateSinceMs[i] >= DEBOUNCE_MS) {
      stableIndex[i] = candidateIndex[i];
    }

    // Channel Connection Check: index 10 means the socket is empty (open circuit)
    bool isConnected = (stableIndex[i] != 10);

    // Channel Match Check: See if detected resistor matches the correct answer for this channel
    bool isMatch = isConnected && (stableIndex[i] == correctAnswers[i]);

    // Still deciding: the live reading hasn't yet been confirmed as the new
    // stable classification, so show solid yellow instead of jumping to red/green.
    bool isDeciding = (detectedIndex != stableIndex[i]);

    LEDState ledState = LED_OFF;
    if (isDeciding) {
      ledState = LED_YELLOW;
    } else if (isConnected) {
      ledState = isMatch ? LED_GREEN : LED_RED;
    }
    setLEDState(channels[i].redPin, channels[i].greenPin, ledState);

    if (!isMatch) {
      allCorrect = false;
    }

    Serial.print("CH");
    Serial.print(i + 1);
    Serial.print(":");
    
    // Print short name (e.g. "4.7k" or "NONE") aligned to exactly 5 characters
    // Uses the debounced stableIndex, not the raw instantaneous detectedIndex.
    const char* name = getResistorName(stableIndex[i]);
    Serial.print(name);
    int nameLen = strlen(name);
    for (int p = nameLen; p < 5; p++) {
      Serial.print(" ");
    }
    
    Serial.print("(");
    print4DigitMV(normalizedMV);
    Serial.print("mV) ");
    
    Serial.print(isMatch ? "[OK]" : "[ERR]");

    // TEMP DEBUG: raw unscaled ADC voltage, before auto-calibration scaling is applied.
    // Useful for confirming a physical connection change is actually reaching the pin.
    Serial.print(" raw:");
    print4DigitMV(rawMV);
    Serial.print("mV");

    if (i < 2) {
      Serial.print(" | ");
    }
  }

  // GAME WIN CONDITION: Triggered only when all three channels match their correct answers simultaneously
  if (allCorrect) {
    Serial.print(" *** GAME SOLVED: ALL PATCHES CORRECT! ***");
  }

  Serial.println();
  delay(300);
}
