#include <Arduino.h>

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("HELLO_FROM_APP");
}

void loop() {
  Serial.println("APP_COUNTER");
  delay(1000);

  if (Serial.available()) {
    String line = Serial.readStringUntil('\n');
    line.trim();
    if (line == "PING") {
      Serial.println("NVLOG|ACK|command=PING");
    }
    if (line == "STATUS") {
      Serial.println("NVLOG|STATUS|mode=serial_minimal");
    }
  }
}
