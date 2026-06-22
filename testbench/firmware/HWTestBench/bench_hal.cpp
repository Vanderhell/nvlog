#if !defined(ESP32) && defined(ARDUINO_ARCH_ESP32)
#define ESP32 1
#endif

#include "bench_hal.h"
#include "bench_config.h"

#include <SPI.h>
#include <SD_MMC.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7735.h>
#include <esp_heap_caps.h>
#include <esp_system.h>
#include <string.h>

namespace {
BenchCapabilities g_capabilities = {false, false, false, 0};

#if BENCH_DISPLAY_ENABLED
Adafruit_ST7735 g_display(BENCH_DISPLAY_CS, BENCH_DISPLAY_DC, BENCH_DISPLAY_RST);
#endif

void print_sanitized(const char *value) {
  if (value == nullptr) {
    Serial.print("null");
    return;
  }

  while (*value != '\0') {
    const char ch = *value++;
    if (ch == '|' || ch == '\r' || ch == '\n') {
      Serial.print('_');
    } else if (static_cast<uint8_t>(ch) < 0x20U) {
      Serial.print('?');
    } else {
      Serial.print(ch);
    }
  }
}

#if BENCH_SD_ENABLED
bool ensure_sd_directory() {
  if (!g_capabilities.sd_ready) {
    return false;
  }
  if (SD_MMC.exists(BENCH_SD_TEST_DIRECTORY)) {
    return true;
  }
  return SD_MMC.mkdir(BENCH_SD_TEST_DIRECTORY);
}
#endif
}  // namespace

void bench_hal_begin() {
  SPI.begin(BENCH_SPI_SCLK, BENCH_SPI_MISO, BENCH_SPI_MOSI);

#if BENCH_DISPLAY_ENABLED
  g_display.initR(INITR_MINI160x80);
  g_display.setRotation(BENCH_DISPLAY_ROTATION);
  g_display.fillScreen(ST77XX_BLACK);
  g_display.setTextColor(ST77XX_WHITE, ST77XX_BLACK);
  g_display.setTextSize(1);
  g_display.setTextWrap(false);
  g_capabilities.display_ready = true;

#if BENCH_DISPLAY_BACKLIGHT >= 0
  pinMode(BENCH_DISPLAY_BACKLIGHT, OUTPUT);
  digitalWrite(BENCH_DISPLAY_BACKLIGHT, HIGH);
#endif
#endif

#if BENCH_SD_ENABLED
  g_capabilities.sd_ready = SD_MMC.setPins(
      BENCH_SDMMC_PIN_CLK,
      BENCH_SDMMC_PIN_CMD,
      BENCH_SDMMC_PIN_D0,
      -1,
      -1,
      BENCH_SDMMC_PIN_D3);
  if (g_capabilities.sd_ready) {
    g_capabilities.sd_ready = SD_MMC.begin(
        "/sdcard",
        BENCH_SDMMC_MODE_1BIT != 0,
        false,
        BOARD_MAX_SDMMC_FREQ,
        5);
  }
  if (g_capabilities.sd_ready) {
    ensure_sd_directory();
  }
#endif

#if BENCH_PSRAM_ENABLED
  const bool psram_inited = psramInit();
  g_capabilities.psram_ready = psram_inited || psramFound();
  g_capabilities.psram_bytes = g_capabilities.psram_ready ? ESP.getPsramSize() : 0U;
#endif
}

const BenchCapabilities &bench_capabilities() {
  return g_capabilities;
}

void bench_emit_event(const char *event_name) {
  Serial.print("NVLOG|");
  print_sanitized(event_name);
}

void bench_emit_field(const char *key, const char *value) {
  Serial.print('|');
  print_sanitized(key);
  Serial.print('=');
  print_sanitized(value);
}

void bench_emit_field_u32(const char *key, uint32_t value) {
  Serial.print('|');
  print_sanitized(key);
  Serial.print('=');
  Serial.print(value);
}

void bench_emit_field_size(const char *key, size_t value) {
  Serial.print('|');
  print_sanitized(key);
  Serial.print('=');
  Serial.print(static_cast<unsigned long>(value));
}

void bench_emit_end() {
  Serial.println();
}

void bench_emit_message(const char *event_name, const char *key, const char *value) {
  bench_emit_event(event_name);
  bench_emit_field(key, value);
  bench_emit_end();
}

bool bench_display_clear() {
#if BENCH_DISPLAY_ENABLED
  if (!g_capabilities.display_ready) {
    return false;
  }
  g_display.fillScreen(ST77XX_BLACK);
  return true;
#else
  return false;
#endif
}

bool bench_display_write_line(uint8_t line, const char *text) {
#if BENCH_DISPLAY_ENABLED
  if (!g_capabilities.display_ready || line >= 10U) {
    return false;
  }

  const int16_t y = static_cast<int16_t>(line) * 8;
  g_display.fillRect(0, y, 160, 8, ST77XX_BLACK);
  g_display.setCursor(0, y);
  g_display.setTextColor(ST77XX_WHITE, ST77XX_BLACK);
  g_display.print(text != nullptr ? text : "");
  return true;
#else
  (void)line;
  (void)text;
  return false;
#endif
}

bool bench_display_test() {
#if BENCH_DISPLAY_ENABLED
  if (!g_capabilities.display_ready) {
    return false;
  }

  g_display.fillScreen(ST77XX_RED);
  delay(120);
  g_display.fillScreen(ST77XX_GREEN);
  delay(120);
  g_display.fillScreen(ST77XX_BLUE);
  delay(120);
  g_display.fillScreen(ST77XX_BLACK);
  g_display.setTextColor(ST77XX_WHITE, ST77XX_BLACK);
  g_display.setCursor(0, 0);
  g_display.print("HW TESTBENCH");
  g_display.setCursor(0, 12);
  g_display.print("DISPLAY: PASS");
  return true;
#else
  return false;
#endif
}

bool bench_sd_append(const char *path, const char *text) {
#if BENCH_SD_ENABLED
  if (!g_capabilities.sd_ready || path == nullptr || text == nullptr) {
    return false;
  }
  if (!ensure_sd_directory()) {
    return false;
  }

  File file = SD_MMC.open(path, FILE_APPEND);
  if (!file) {
    return false;
  }
  const size_t written = file.println(text);
  file.flush();
  file.close();
  return written > 0U;
#else
  (void)path;
  (void)text;
  return false;
#endif
}

bool bench_sd_dump(const char *path, Print &output) {
#if BENCH_SD_ENABLED
  if (!g_capabilities.sd_ready || path == nullptr) {
    return false;
  }

  File file = SD_MMC.open(path, FILE_READ);
  if (!file) {
    return false;
  }
  while (file.available()) {
    output.write(static_cast<uint8_t>(file.read()));
  }
  file.close();
  return true;
#else
  (void)path;
  (void)output;
  return false;
#endif
}

bool bench_sd_test() {
#if BENCH_SD_ENABLED
  static const char marker[] = "HW_SD_TEST";
  return bench_sd_append(BENCH_SD_TEST_FILE, marker);
#else
  return false;
#endif
}

bool bench_psram_test(char *detail, size_t detail_size) {
  if (detail != nullptr && detail_size > 0U) {
    detail[0] = '\0';
  }

#if BENCH_PSRAM_ENABLED
  if (!g_capabilities.psram_ready) {
    if (detail != nullptr && detail_size > 0U) {
      snprintf(detail, detail_size, "psram_not_found");
    }
    return false;
  }

  uint8_t *buffer = static_cast<uint8_t *>(
      heap_caps_malloc(BENCH_PSRAM_TEST_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (buffer == nullptr) {
    if (detail != nullptr && detail_size > 0U) {
      snprintf(detail, detail_size, "allocation_failed");
    }
    return false;
  }

  for (size_t i = 0; i < BENCH_PSRAM_TEST_BYTES; ++i) {
    buffer[i] = static_cast<uint8_t>((i * 131U + 17U) & 0xFFU);
  }

  bool passed = true;
  size_t failed_offset = 0U;
  for (size_t i = 0; i < BENCH_PSRAM_TEST_BYTES; ++i) {
    const uint8_t expected = static_cast<uint8_t>((i * 131U + 17U) & 0xFFU);
    if (buffer[i] != expected) {
      passed = false;
      failed_offset = i;
      break;
    }
  }
  heap_caps_free(buffer);

  if (detail != nullptr && detail_size > 0U) {
    if (passed) {
      snprintf(detail, detail_size, "checked=%u", BENCH_PSRAM_TEST_BYTES);
    } else {
      snprintf(detail, detail_size, "failed_offset=%u", static_cast<unsigned>(failed_offset));
    }
  }
  return passed;
#else
  if (detail != nullptr && detail_size > 0U) {
    snprintf(detail, detail_size, "psram_test_disabled");
  }
  return false;
#endif
}
