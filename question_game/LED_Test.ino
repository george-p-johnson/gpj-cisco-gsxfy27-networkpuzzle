// --- Pin Definitions ---
// LED 1
const int RED_1   = 35;
const int GREEN_1 = 34;

// LED 2
const int RED_2   = 48;
const int GREEN_2 = 47;

// LED 3
const int RED_3   = 39;
const int GREEN_3 = 38;

// Arrays for easy looping
const int redPins[3]   = {RED_1, RED_2, RED_3};
const int greenPins[3] = {GREEN_1, GREEN_2, GREEN_3};

void setup() {
  Serial.begin(115200);
  
  // Set all LED pins as outputs and ensure they start off
  for (int i = 0; i < 3; i++) {
    pinMode(redPins[i], OUTPUT);
    pinMode(greenPins[i], OUTPUT);
    digitalWrite(redPins[i], LOW);
    digitalWrite(greenPins[i], LOW);
  }
  
  Serial.println("ESP32-S3 Dual-Color LED Test Started!");
}

// Turn off all LEDs across all channels
void clearAll() {
  for (int i = 0; i < 3; i++) {
    digitalWrite(redPins[i], LOW);
    digitalWrite(greenPins[i], LOW);
  }
}

void loop() {
  // --- Test Pattern: Sequence Red & Green across LED 1, 2, and 3 ---
  
  // Round 1: Red 1 -> Green 1 -> Red 2 -> Green 2 -> Red 3 -> Green 3
  for (int i = 0; i < 3; i++) {
    // Red on
    clearAll();
    digitalWrite(redPins[i], HIGH);
    Serial.printf("LED %d: RED\n", i + 1);
    delay(400);

    // Green on
    clearAll();
    digitalWrite(greenPins[i], HIGH);
    Serial.printf("LED %d: GREEN\n", i + 1);
    delay(400);
  }

  // Brief pause before repeating
  clearAll();
  delay(1000);
}
