#include <Arduino.h>
#include <SPI.h>
#include <SD.h>
#include <ETH.h>

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
// CH1 (Socket 1)
// NOTE: was originally GPIO35/34, which sit inside Waveshare's documented
// "GPIO33-37 internally occupied" range (see ESP32-S3-ETH wiki FAQ) -- driving
// those as outputs was corrupting state that later crashed SD.begin() with an
// Interrupt WDT panic (confirmed independently on both 34 and 35). Moved to
// GPIO42/45 -- neither overlaps SD, Ethernet, analog inputs, the other
// channels' LEDs, or the bad 33-37 range.
const int CH1_RED   = 45;
const int CH1_GREEN = 42;

// CH2 (Socket 2) 
const int CH2_RED   = 48;
const int CH2_GREEN = 47;

// CH3 (Socket 3)
const int CH3_RED   = 39;
const int CH3_GREEN = 38;

// --- SD CARD PIN DEFINITIONS ---
// Waveshare ESP32-S3-ETH onboard TF card slot. Uses the default global SPI
// object (not a separately-hosted SPIClass) -- an explicit SPIClass(FSPI)
// instance was found to hang SD.begin() on this board, even though the pins
// and card were confirmed good via an isolated test with the default object.
const int SD_SCK  = 7;
const int SD_MISO = 5;
const int SD_MOSI = 6;
const int SD_CS   = 4;

// --- MULTI-DEVICE CONFIG (SAME FIRMWARE, SIX PHYSICAL BOARDS) ---
// Each board's SD card carries a small device.cfg naming which of the six
// Waveshare units it is (1-6). That index selects which 3 rows of
// answer_table.tsv (also copied onto the SD card) become this board's
// correctAnswers[]. Devices 1/2 serve panel P1 (questions 1-3 / 4-6),
// devices 3/4 serve panel P2, devices 5/6 serve panel P3.
const char* DEVICE_CONFIG_FILE = "/device.cfg";
const char* ANSWER_TABLE_FILE  = "/answer_table.tsv";
int deviceIndex = 1; // fallback if SD card / config is missing

// --- ETHERNET / OSC TELEMETRY ---
// W5500 pins confirmed from Waveshare's own ESP32-S3-ETH wiki. Runs on its
// own SPI bus (HSPI) -- confirmed working standalone via ETH_Test.ino --
// separate from the SD card, which owns the default SPI object on GPIO4-7.
#define ETH_PHY_TYPE ETH_PHY_W5500
#define ETH_PHY_ADDR 1
#define ETH_PHY_CS   14
#define ETH_PHY_IRQ  10
#define ETH_PHY_RST  9
#define ETH_SPI_SCK  13
#define ETH_SPI_MISO 12
#define ETH_SPI_MOSI 11
SPIClass ethSPI(HSPI);
NetworkUDP udp;
uint32_t heartbeatCounter = 0; // increments every telemetry send, regardless of game state, so a listener can detect a live board even when its readings aren't changing
bool ethConnected = false;

// Static IP scheme: this board = 192.168.50.(10 + deviceIndex), e.g. device 4
// -> 192.168.50.14. Matches the addressing used when validating ETH_Test.ino.
IPAddress ethGateway(192, 168, 50, 1);
IPAddress ethSubnet(255, 255, 255, 0);
IPAddress ethDNS(192, 168, 50, 1);

// Where OSC telemetry gets sent. Defaults to the game controller PC's static
// IP; overridable per-board via an optional "controller_ip=" line in
// device.cfg if a board ever needs to report somewhere else.
IPAddress controllerIP(192, 168, 50, 100);
const uint16_t OSC_PORT = 9000;

// Incoming game-state channel (TD -> boards). TD broadcasts the current
// game state to 192.168.50.255 on this port; every board listens and reacts
// with lighting effects (see applyGameStateLighting() below).
const uint16_t STATE_LISTEN_PORT = 9003;
int currentGameState = 1; // 1 = Idle, matches the same 1-7 convention used elsewhere; safe default until TD sends real state

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
const unsigned long DEBOUNCE_MS = 225;
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
// These are only the fallback/bench-test values used when no SD card (or no
// answer_table.tsv match) is found. Normally this array is overwritten at
// boot by loadAnswersFromSD() based on this board's device.cfg index.
int correctAnswers[3] = {
  3,  // CH1 correct answer is 4.7k Ohm (Index 3)
  7,  // CH2 correct answer is 47k Ohm  (Index 7)
  5   // CH3 correct answer is 22k Ohm  (Index 5)
};

// Works out which 3 question IDs (e.g. "P1q1", "P1q2", "P1q3") this board
// is responsible for, given its device.cfg index (1-6).
void computeQuestionIDs(int devIdx, String qIDs[3]) {
  int zeroBased = devIdx - 1;           // 0..5
  int panel = zeroBased / 2 + 1;        // 1,1,2,2,3,3
  int startQ = (zeroBased % 2) * 3 + 1; // 1 or 4
  for (int i = 0; i < 3; i++) {
    qIDs[i] = "P" + String(panel) + "q" + String(startQ + i);
  }
}

// Reads /device.cfg (key=value, e.g. "waveshare_index=3") from the SD card.
// Falls back to 1 and logs a warning if the file, key, or value is invalid.
int loadDeviceIndex() {
  File f = SD.open(DEVICE_CONFIG_FILE);
  if (!f) {
    Serial.print(">> WARNING: Could not open ");
    Serial.print(DEVICE_CONFIG_FILE);
    Serial.println(". Defaulting to waveshare_index=1.");
    return 1;
  }

  int idx = -1;
  while (f.available()) {
    String line = f.readStringUntil('\n');
    line.trim();
    if (line.startsWith("waveshare_index")) {
      int eq = line.indexOf('=');
      if (eq >= 0) {
        idx = line.substring(eq + 1).toInt();
      }
    }
  }
  f.close();

  if (idx < 1 || idx > 6) {
    Serial.println(">> WARNING: waveshare_index missing or out of range [1-6] in device.cfg. Defaulting to 1.");
    return 1;
  }
  return idx;
}

// Reads an optional "controller_ip=a.b.c.d" line from device.cfg to override
// where OSC telemetry gets sent. Leaves controllerIP at its default
// (192.168.50.100) if the file, key, or value is missing/invalid.
void loadControllerIP() {
  File f = SD.open(DEVICE_CONFIG_FILE);
  if (!f) return; // already warned about this in loadDeviceIndex()

  while (f.available()) {
    String line = f.readStringUntil('\n');
    line.trim();
    if (line.startsWith("controller_ip")) {
      int eq = line.indexOf('=');
      if (eq >= 0) {
        IPAddress parsed;
        String value = line.substring(eq + 1);
        value.trim();
        if (parsed.fromString(value)) {
          controllerIP = parsed;
        } else {
          Serial.print(">> WARNING: could not parse controller_ip value \"");
          Serial.print(value);
          Serial.println("\" in device.cfg. Keeping default.");
        }
      }
    }
  }
  f.close();
}

// Reads /answer_table.tsv from the SD card and fills correctAnswers[] with
// the "index" column of the 3 rows matching this device's question IDs.
// Returns true only if all 3 rows were found.
bool loadAnswersFromSD(int devIdx) {
  String qIDs[3];
  computeQuestionIDs(devIdx, qIDs);
  bool found[3] = { false, false, false };

  File f = SD.open(ANSWER_TABLE_FILE);
  if (!f) {
    Serial.print(">> WARNING: Could not open ");
    Serial.print(ANSWER_TABLE_FILE);
    Serial.println(" on SD card.");
    return false;
  }

  f.readStringUntil('\n'); // skip header row (question, answer, ohms, index)

  while (f.available()) {
    String line = f.readStringUntil('\n');
    line.trim();
    if (line.length() == 0) continue;

    int tab1 = line.indexOf('\t');
    int tab2 = line.indexOf('\t', tab1 + 1);
    int tab3 = line.indexOf('\t', tab2 + 1);
    if (tab1 < 0 || tab2 < 0 || tab3 < 0) continue; // malformed row, skip

    String question = line.substring(0, tab1);
    String idxStr = line.substring(tab3 + 1);
    idxStr.trim();

    for (int i = 0; i < 3; i++) {
      if (!found[i] && question == qIDs[i]) {
        correctAnswers[i] = idxStr.toInt();
        found[i] = true;
      }
    }
  }
  f.close();

  Serial.println(">> Device question assignments:");
  for (int i = 0; i < 3; i++) {
    Serial.print("   CH");
    Serial.print(i + 1);
    Serial.print(" = ");
    Serial.print(qIDs[i]);
    if (found[i]) {
      Serial.print("  -> answer index ");
      Serial.print(correctAnswers[i]);
      Serial.print(" (");
      Serial.print(getResistorName(correctAnswers[i]));
      Serial.println(")");
    } else {
      Serial.println("  -> NOT FOUND in answer_table.tsv (keeping fallback default)");
    }
  }

  return found[0] && found[1] && found[2];
}

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

// --- OSC MESSAGE BUILDING ---
// Minimal hand-rolled OSC packet encoder (address + int32 args only) so we
// don't need a third-party OSC library across six boards. OSC wire format:
// null-padded address string, null-padded ",iii..." type tag string, then
// each int32 argument big-endian, back to back.
static size_t oscPad4(size_t len) {
  return (len + 3) & ~((size_t)3);
}

static size_t oscAppendString(uint8_t *buf, size_t offset, const char *s) {
  size_t len = strlen(s);
  memcpy(buf + offset, s, len);
  size_t padded = oscPad4(len + 1); // +1 for at least one null terminator
  for (size_t i = len; i < padded; i++) buf[offset + i] = 0;
  return offset + padded;
}

void sendOSCInts(const char *address, const int *values, int count) {
  uint8_t buf[128];
  size_t offset = oscAppendString(buf, 0, address);

  char typeTags[16];
  typeTags[0] = ',';
  for (int i = 0; i < count; i++) typeTags[1 + i] = 'i';
  typeTags[1 + count] = '\0';
  offset = oscAppendString(buf, offset, typeTags);

  for (int i = 0; i < count; i++) {
    uint32_t v = (uint32_t)values[i];
    buf[offset++] = (v >> 24) & 0xFF;
    buf[offset++] = (v >> 16) & 0xFF;
    buf[offset++] = (v >> 8) & 0xFF;
    buf[offset++] = v & 0xFF;
  }

  udp.beginPacket(controllerIP, OSC_PORT);
  udp.write(buf, offset);
  udp.endPacket();
}

// Sends this board's current state as one OSC message:
// address "/waveshare/<deviceIndex>", args [ch1Index, ch1Match, ch2Index,
// ch2Match, ch3Index, ch3Match, allCorrect, heartbeat] -- all ints, matches
// are 0/1. `heartbeat` increments on every send (wrapping is fine) so a
// listener can tell the board is alive even during stretches where none of
// the other values change.
void sendGameStateOSC(const int stableIdx[3], const bool matched[3], bool allCorrect) {
  if (!ethConnected) return; // fire-and-forget; skip while link/IP isn't up

  String address = "/waveshare/" + String(deviceIndex);
  int values[8] = {
    stableIdx[0], matched[0] ? 1 : 0,
    stableIdx[1], matched[1] ? 1 : 0,
    stableIdx[2], matched[2] ? 1 : 0,
    allCorrect ? 1 : 0,
    (int)(heartbeatCounter++)
  };
  sendOSCInts(address.c_str(), values, 8);
}

// Checks for an incoming "/game_state" OSC message (one int32 arg, the same
// 1-7 convention as everything else: 1=Idle 2=Three 3=Two 4=One 5=Start
// 6=Gameplay 7=Results) and updates currentGameState if one arrived.
// Minimal parse to match the minimal encoder above -- no third-party OSC lib.
void checkForStateUpdate() {
  int packetSize = udp.parsePacket();
  if (packetSize <= 0) return;

  uint8_t inBuf[64];
  int len = udp.read(inBuf, sizeof(inBuf) - 1);
  if (len <= 0) return;
  inBuf[len] = 0;

  if (strcmp((char*)inBuf, "/game_state") != 0) return;
  size_t offset = oscPad4(strlen((char*)inBuf) + 1);
  if (offset + 4 > (size_t)len) return;

  const char* typeTag = (char*)(inBuf + offset);
  if (typeTag[0] != ',' || typeTag[1] != 'i') return;
  offset += oscPad4(strlen(typeTag) + 1);
  if (offset + 4 > (size_t)len) return;

  uint32_t v = ((uint32_t)inBuf[offset] << 24) | ((uint32_t)inBuf[offset + 1] << 16) |
               ((uint32_t)inBuf[offset + 2] << 8) | (uint32_t)inBuf[offset + 3];
  currentGameState = (int)v;
}

// Decides this channel's LED for the current game state:
// - Idle / Start: always off, regardless of what's plugged in.
// - Three / Two / One: a countdown -- lights up one more channel red per
//   step (CH1 only -> CH1+CH2 -> all three), independent of actual readings.
// - Gameplay / Results: reflects real connection data (handled by the
//   caller, which already has isConnected/isMatch/isDeciding computed).
LEDState countdownOrIdleLEDState(int channelIndex) {
  if (currentGameState == 2 || currentGameState == 3 || currentGameState == 4) {
    int litCount = currentGameState - 1; // Three->1, Two->2, One->3
    return (channelIndex < litCount) ? LED_RED : LED_OFF;
  }
  return LED_OFF; // Idle, Start, or anything unrecognized
}

void onEthEvent(arduino_event_id_t event, arduino_event_info_t info) {
  switch (event) {
    case ARDUINO_EVENT_ETH_START:
      Serial.println(">> ETH Started");
      ETH.setHostname(("waveshare-" + String(deviceIndex)).c_str());
      break;
    case ARDUINO_EVENT_ETH_CONNECTED:
      Serial.println(">> ETH Connected (link up)");
      break;
    case ARDUINO_EVENT_ETH_GOT_IP:
      Serial.print(">> ETH Got IP: ");
      Serial.println(ETH.localIP());
      ethConnected = true;
      break;
    case ARDUINO_EVENT_ETH_LOST_IP:
      Serial.println(">> ETH Lost IP");
      ethConnected = false;
      break;
    case ARDUINO_EVENT_ETH_DISCONNECTED:
      Serial.println(">> ETH Disconnected (link down)");
      ethConnected = false;
      break;
    case ARDUINO_EVENT_ETH_STOP:
      Serial.println(">> ETH Stopped");
      ethConnected = false;
      break;
    default:
      break;
  }
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

  // --- SD CARD CONFIG LOAD ---
  // Same firmware runs on all six boards; each board's SD card tells it
  // which device it is (device.cfg) and supplies the shared answer_table.tsv
  // it reads its 3 correct answers from.
  Serial.println(">> Initializing SD SPI bus (SCK=7 MISO=5 MOSI=6 CS=4)...");
  Serial.flush();
  SPI.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);

  Serial.println(">> Mounting SD card...");
  Serial.flush();
  if (SD.begin(SD_CS)) {
    deviceIndex = loadDeviceIndex();
    Serial.print(">> Waveshare device index: ");
    Serial.println(deviceIndex);

    if (!loadAnswersFromSD(deviceIndex)) {
      Serial.println(">> WARNING: Using fallback default correctAnswers[] for any channel not found above.");
    }

    loadControllerIP();
  } else {
    Serial.println(">> WARNING: SD card not found or failed to mount. Using hardcoded default correctAnswers[].");
  }
  Serial.println();

  // --- ETHERNET INIT ---
  // Own SPI bus (HSPI), separate from SD's default SPI object -- confirmed
  // this combination works via ETH_Test.ino before integrating here. Static
  // IP is deviceIndex-based, so it must run after the SD block above.
  Network.onEvent(onEthEvent);

  Serial.println(">> Initializing ETH SPI bus (SCK=13 MISO=12 MOSI=11 CS=14)...");
  Serial.flush();
  ethSPI.begin(ETH_SPI_SCK, ETH_SPI_MISO, ETH_SPI_MOSI, ETH_PHY_CS);

  Serial.println(">> ETH.begin() ...");
  Serial.flush();
  if (ETH.begin(ETH_PHY_TYPE, ETH_PHY_ADDR, ETH_PHY_CS, ETH_PHY_IRQ, ETH_PHY_RST, ethSPI)) {
    IPAddress ethIP(192, 168, 50, 10 + deviceIndex);
    ETH.config(ethIP, ethGateway, ethSubnet, ethDNS);
    Serial.print(">> ETH static IP configured: ");
    Serial.println(ethIP);
    Serial.print(">> OSC telemetry target: ");
    Serial.print(controllerIP);
    Serial.print(":");
    Serial.println(OSC_PORT);
    udp.begin(STATE_LISTEN_PORT);
    Serial.print(">> Listening for game-state broadcasts on UDP ");
    Serial.println(STATE_LISTEN_PORT);
  } else {
    Serial.println(">> WARNING: ETH.begin() FAILED. Running without network telemetry.");
  }
  Serial.println();

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
  checkForStateUpdate();

  bool allCorrect = true;
  bool matched[3] = { false, false, false };

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
    // stable classification, so keep the LED off instead of jumping to red/green.
    bool isDeciding = (detectedIndex != stableIndex[i]);

    LEDState ledState;
    if (currentGameState == 6 || currentGameState == 7) {
      // Gameplay / Results: reflect actual connection data.
      ledState = LED_OFF;
      if (!isDeciding && isConnected) {
        ledState = isMatch ? LED_GREEN : LED_RED;
      }
    } else {
      // Idle / Three / Two / One / Start: lighting is state-driven, not reading-driven.
      ledState = countdownOrIdleLEDState(i);
    }
    setLEDState(channels[i].redPin, channels[i].greenPin, ledState);

    matched[i] = isMatch;
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
    
    Serial.print(isMatch ? "[ OK ]" : "[ERR ]");
    
    if (i < 2) {
      Serial.print(" | ");
    }
  }

  // GAME WIN CONDITION: Triggered only when all three channels match their correct answers simultaneously
  if (allCorrect) {
    Serial.print(" *** GAME SOLVED: ALL PATCHES CORRECT! ***");
  }

  Serial.println();

  sendGameStateOSC(stableIndex, matched, allCorrect);

  delay(300);
}
