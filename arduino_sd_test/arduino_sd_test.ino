// Minimal SD card isolation test for the Waveshare ESP32-S3-ETH onboard TF
// slot. Default global SPI object with custom pins, no other peripherals.
// Useful as a standalone sanity check if SD problems come up again.

#include <SPI.h>
#include <SD.h>

const int SD_CS   = 4;
const int SD_MISO = 5;
const int SD_MOSI = 6;
const int SD_SCK  = 7;

void writeFileToSD() {
  File f = SD.open("/waveshare.txt", FILE_WRITE);
  if (!f) {
    Serial.println("writeFileToSD: failed to open for writing");
    return;
  }
  f.println("Hello world from Waveshare");
  f.close();
  Serial.println("writeFileToSD: done");
}

void listFilesOnSD() {
  File root = SD.open("/");
  if (!root) {
    Serial.println("listFilesOnSD: failed to open root");
    return;
  }
  File entry = root.openNextFile();
  while (entry) {
    Serial.print("  ");
    Serial.print(entry.name());
    Serial.print("  ");
    Serial.println(entry.size());
    entry = root.openNextFile();
  }
  root.close();
}

void readFileFromSD(const char *path) {
  File f = SD.open(path);
  if (!f) {
    Serial.println("readFileFromSD: failed to open");
    return;
  }
  Serial.print("Contents of ");
  Serial.print(path);
  Serial.println(":");
  while (f.available()) {
    Serial.write(f.read());
  }
  f.close();
}

void setup() {
  Serial.begin(115200);
  delay(2000);
  Serial.println("\n--- SD isolation test starting ---");

  Serial.println(">> SPI.begin() ...");
  Serial.flush();
  SPI.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);

  Serial.println(">> SD.begin() ...");
  Serial.flush();
  if (!SD.begin(SD_CS)) {
    Serial.println(">> SD.begin() FAILED");
    return;
  }

  Serial.println(">> SD.begin() OK");
  uint8_t cardType = SD.cardType();
  Serial.print("Card type: ");
  Serial.println(cardType);
  Serial.print("Card size (MB): ");
  Serial.println((uint32_t)(SD.cardSize() / (1024 * 1024)));

  writeFileToSD();
  Serial.println("Root directory listing:");
  listFilesOnSD();
  readFileFromSD("/waveshare.txt");

  Serial.println("--- SD isolation test complete ---");
}

void loop() {
}
