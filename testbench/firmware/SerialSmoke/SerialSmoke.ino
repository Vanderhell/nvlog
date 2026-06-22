#include <Arduino.h>

#if defined(ARDUINO_USB_MODE) && (ARDUINO_USB_MODE == 0)
#include <USB.h>
#include <HWCDC.h>
HWCDC AppSerial;
#else
#define AppSerial Serial
#endif

void setup() {
  AppSerial.begin(115200);
  delay(1000);
  AppSerial.println("HELLO_FROM_APP");
}

void loop() {
  AppSerial.println("APP_COUNTER");
  delay(1000);

  if (AppSerial.available()) {
    String line = AppSerial.readStringUntil('\n');
    line.trim();
    if (line == "PING") {
      AppSerial.println("NVLOG|ACK|command=PING");
    }
    if (line == "STATUS") {
      AppSerial.println("NVLOG|STATUS|mode=serial_minimal");
    }
  }
}
