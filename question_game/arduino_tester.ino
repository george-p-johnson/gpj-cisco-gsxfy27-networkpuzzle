// waveshare ESP32-S3-ETH

const int Q1_RED_PIN = 39;
const int Q1_GREEN_PIN = 40;
const int Q1_ANALOG_PIN = 15;

const int Q2_RED_PIN = 36;
const int Q2_GREEN_PIN = 37;
const int Q2_ANALOG_PIN = 18;

const int Q3_RED_PIN = 33;
const int Q3_GREEN_PIN = 34;
const int Q3_ANALOG_PIN = 16;

const float VREF = 3.3;
const float R_KNOWN = 470.0;    // unknown: 430.0 ohms

void setup() {
  pinMode(Q1_RED_PIN, OUTPUT);
  pinMode(Q1_GREEN_PIN, OUTPUT);
  
  pinMode(Q2_RED_PIN, OUTPUT);
  pinMode(Q2_GREEN_PIN, OUTPUT);

  pinMode(Q3_RED_PIN, OUTPUT);
  pinMode(Q3_GREEN_PIN, OUTPUT);

  Serial.begin(115200);
  analogReadResolution(12);
}

// digitalWrite(RED_PIN, HIGH);
// digitalWrite(GREEN_PIN, LOW);

void loop() {

  int q1_rawADC = analogRead(Q1_ANALOG_PIN);
  float q1_vOut = (q1_rawADC / 4095.0) * 3.3;
  int q1_resistance = R_KNOWN * (q1_vOut / (3.3 - q1_vOut));
  Serial.printf("Resistance: %d Ohms\n", q1_resistance);

  int q2_rawADC = analogRead(Q2_ANALOG_PIN);
  float q2_vOut = (q2_rawADC / 4095.0) * 3.3;
  int q2_resistance = R_KNOWN * (q2_vOut / (3.3 - q2_vOut));
  Serial.printf("Resistance: %d Ohms\n", q2_resistance);
  
  int q3_rawADC = analogRead(Q3_ANALOG_PIN);
  float q3_vOut = (q3_rawADC / 4095.0) * 3.3;
  int q3_resistance = R_KNOWN * (q3_vOut / (3.3 - q3_vOut));
  Serial.printf("Resistance: %d Ohms\n", q3_resistance);

  // question 1
  if (q1_vOut > 0 && q1_vOut < 3.3) {
    if (q1_resistance < 1000) {
      // 430 ohms
      digitalWrite(Q1_RED_PIN, LOW);
      digitalWrite(Q1_GREEN_PIN, HIGH);
    } 
    else if (q1_resistance < 10000) {
      // 5.6K ohms
      digitalWrite(Q1_RED_PIN, HIGH);
      digitalWrite(Q1_GREEN_PIN, LOW);
    } 
    else {
      // no connection
      digitalWrite(Q1_RED_PIN, LOW);
      digitalWrite(Q1_GREEN_PIN, LOW);
    }
  } else {
    Serial.println("Q1 out of range or disconnected");
  }

  // question 2
  if (q2_vOut > 0 && q2_vOut < 3.3) {
    if (q2_resistance < 1000) {
      // 430 ohms
      digitalWrite(Q2_RED_PIN, LOW);
      digitalWrite(Q2_GREEN_PIN, HIGH);
    } 
    else if (q2_resistance < 10000) {
      // 5.6K ohms
      digitalWrite(Q2_RED_PIN, HIGH);
      digitalWrite(Q2_GREEN_PIN, LOW);
    } 
    else {
      // no connection
      digitalWrite(Q2_RED_PIN, LOW);
      digitalWrite(Q2_GREEN_PIN, LOW);
    }
  } else {
    Serial.println("Q2 out of range or disconnected");
  }

  // question 3
  if (q3_vOut > 0 && q3_vOut < 3.3) {
    if (q3_resistance < 1000) {
      // 430 ohms
      digitalWrite(Q3_RED_PIN, LOW);
      digitalWrite(Q3_GREEN_PIN, HIGH);
    } 
    else if (q3_resistance < 10000) {
      // 5.6K ohms
      digitalWrite(Q3_RED_PIN, HIGH);
      digitalWrite(Q3_GREEN_PIN, LOW);
    } 
    else {
      // no connection
      digitalWrite(Q3_RED_PIN, LOW);
      digitalWrite(Q3_GREEN_PIN, LOW);
    }
  } else {
    Serial.println("Q3 out of range or disconnected");
  }

  delay(1000);
}