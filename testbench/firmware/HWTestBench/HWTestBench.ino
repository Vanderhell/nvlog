#include <Arduino.h>
#include <esp_system.h>
#include <string.h>

#if !ARDUINO_USB_MODE
#include <USB.h>
#endif

#include "bench_config.h"
#include "bench_hal.h"
#include "project_adapter.h"

namespace {
char g_command_buffer[BENCH_SERIAL_COMMAND_BUFFER_SIZE];
size_t g_command_length = 0U;
uint32_t g_boot_id = 0U;
uint32_t g_loop_count = 0U;
uint32_t g_last_heartbeat_ms = 0U;

const char *reset_reason_name(esp_reset_reason_t reason) {
  switch (reason) {
    case ESP_RST_POWERON: return "power_on";
    case ESP_RST_EXT: return "external";
    case ESP_RST_SW: return "software";
    case ESP_RST_PANIC: return "panic";
    case ESP_RST_INT_WDT: return "interrupt_watchdog";
    case ESP_RST_TASK_WDT: return "task_watchdog";
    case ESP_RST_WDT: return "watchdog";
    case ESP_RST_DEEPSLEEP: return "deep_sleep";
    case ESP_RST_BROWNOUT: return "brownout";
    case ESP_RST_SDIO: return "sdio";
    default: return "unknown";
  }
}

void emit_test_result(const char *name, bool passed, const char *detail) {
  bench_emit_event(passed ? "TEST_PASS" : "TEST_FAIL");
  bench_emit_field("name", name);
  bench_emit_field("detail", detail != nullptr ? detail : "");
  bench_emit_end();
}

void emit_status() {
  const BenchCapabilities &caps = bench_capabilities();
  bench_emit_event("STATUS");
  bench_emit_field_u32("boot_id", g_boot_id);
  bench_emit_field_u32("uptime_ms", millis());
  bench_emit_field_u32("loop_count", g_loop_count);
  bench_emit_field_size("free_heap", ESP.getFreeHeap());
  bench_emit_field_size("psram_bytes", caps.psram_bytes);
  bench_emit_field("display", caps.display_ready ? "ready" : "unavailable");
  bench_emit_field("sd", caps.sd_ready ? "ready" : "unavailable");
  bench_emit_field("psram", caps.psram_ready ? "ready" : "unavailable");
  bench_emit_end();
}

void command_help() {
  Serial.println("Commands: PING, STATUS, DISPLAY_TEST, DISPLAY <text>, SD_TEST,");
  Serial.println("SD_APPEND <text>, SD_READ, PSRAM_TEST, PROJECT_TEST,");
  Serial.println("SESSION_PROGRESS <fields>, REBOOT, BOOTLOADER, HELP");
}

void handle_command(char *command) {
  while (*command == ' ') {
    ++command;
  }

  if (strcmp(command, "PING") == 0) {
    bench_emit_event("ACK");
    bench_emit_field("command", "PING");
    bench_emit_field_u32("boot_id", g_boot_id);
    bench_emit_end();
    return;
  }

  if (strcmp(command, "STATUS") == 0) {
    emit_status();
    return;
  }

  if (strcmp(command, "DISPLAY_TEST") == 0) {
    emit_test_result("display", bench_display_test(), "color_and_text_test");
    return;
  }

  static const char display_prefix[] = "DISPLAY ";
  if (strncmp(command, display_prefix, sizeof(display_prefix) - 1U) == 0) {
    const bool passed = bench_display_write_line(4U, command + sizeof(display_prefix) - 1U);
    emit_test_result("display_write", passed, passed ? "written" : "unavailable");
    return;
  }

  if (strcmp(command, "SD_TEST") == 0) {
    emit_test_result("sd", bench_sd_test(), BENCH_SD_TEST_FILE);
    return;
  }

  static const char sd_append_prefix[] = "SD_APPEND ";
  if (strncmp(command, sd_append_prefix, sizeof(sd_append_prefix) - 1U) == 0) {
    const bool passed = bench_sd_append(
        BENCH_SD_TEST_FILE, command + sizeof(sd_append_prefix) - 1U);
    emit_test_result("sd_append", passed, BENCH_SD_TEST_FILE);
    return;
  }

  if (strcmp(command, "SD_READ") == 0) {
    bench_emit_event("SD_DUMP_BEGIN");
    bench_emit_end();
    const bool passed = bench_sd_dump(BENCH_SD_TEST_FILE, Serial);
    if (passed) {
      Serial.println();
    }
    bench_emit_event("SD_DUMP_END");
    bench_emit_field("status", passed ? "PASS" : "FAIL");
    bench_emit_end();
    return;
  }

  if (strcmp(command, "PSRAM_TEST") == 0) {
    char detail[64];
    emit_test_result("psram", bench_psram_test(detail, sizeof(detail)), detail);
    return;
  }

  if (strcmp(command, "PROJECT_TEST") == 0) {
    char detail[96];
    emit_test_result(
        "project", project_test_run_once(detail, sizeof(detail)), detail);
    return;
  }

  if (strcmp(command, "HELP") == 0) {
    command_help();
    return;
  }

  if (project_test_handle_command(command, Serial)) {
    return;
  }

  bench_emit_event("ERROR");
  bench_emit_field("code", "unknown_command");
  bench_emit_field("command", command);
  bench_emit_end();
}

void poll_serial_commands() {
  while (Serial.available() > 0) {
    const int value = Serial.read();
    if (value < 0) {
      return;
    }

    const char ch = static_cast<char>(value);
    if (ch == '\r') {
      continue;
    }

    if (ch == '\n') {
      g_command_buffer[g_command_length] = '\0';
      if (g_command_length > 0U) {
        handle_command(g_command_buffer);
      }
      g_command_length = 0U;
      continue;
    }

    if (g_command_length + 1U < sizeof(g_command_buffer)) {
      g_command_buffer[g_command_length++] = ch;
    } else {
      g_command_length = 0U;
      bench_emit_message("ERROR", "code", "command_too_long");
    }
  }
}

void emit_heartbeat() {
  const uint32_t now = millis();
  if (now - g_last_heartbeat_ms < BENCH_HEARTBEAT_INTERVAL_MS) {
    return;
  }

  g_last_heartbeat_ms = now;
  bench_emit_event("HEARTBEAT");
  bench_emit_field_u32("boot_id", g_boot_id);
  bench_emit_field_u32("uptime_ms", now);
  bench_emit_field_u32("loop_count", g_loop_count);
  bench_emit_field_size("free_heap", ESP.getFreeHeap());
  bench_emit_end();
}
}  // namespace

void setup() {
#if !ARDUINO_USB_MODE
  USBSerial.begin();
  USB.begin();
#endif
  Serial.begin(BENCH_SERIAL_BAUD);
  delay(250);
  const uint32_t serial_ready_deadline = millis() + 3000U;
  while (!Serial && millis() < serial_ready_deadline) {
    delay(10);
  }
  bench_emit_message("BOOT_STAGE", "name", "after_serial_begin");

  g_boot_id = esp_random();
  bench_emit_message("BOOT_STAGE", "name", "before_hal_begin");
  bench_hal_begin();
  bench_emit_message("BOOT_STAGE", "name", "after_hal_begin");

  const esp_reset_reason_t reset_reason = esp_reset_reason();
  const BenchCapabilities &caps = bench_capabilities();

  bench_emit_event("BOOT");
  bench_emit_field_u32("boot_id", g_boot_id);
  bench_emit_field("reset_reason", reset_reason_name(reset_reason));
  bench_emit_field_u32("reset_code", static_cast<uint32_t>(reset_reason));
  bench_emit_field_size("psram_bytes", caps.psram_bytes);
  bench_emit_field("display", caps.display_ready ? "ready" : "unavailable");
  bench_emit_field("sd", caps.sd_ready ? "ready" : "unavailable");
  bench_emit_end();

  bench_display_clear();
  bench_display_write_line(0U, "HW TESTBENCH");
  bench_display_write_line(1U, "BOOT OK");

  char detail[96];
  bench_emit_message("BOOT_STAGE", "name", "before_project_setup");
  const bool setup_passed = project_test_setup(detail, sizeof(detail));
  bench_emit_message("BOOT_STAGE", "name", "after_project_setup");
  emit_test_result("project_setup", setup_passed, detail);

#if BENCH_RUN_PROJECT_TEST_ON_BOOT
  const bool project_passed = project_test_run_once(detail, sizeof(detail));
  emit_test_result("project_boot", project_passed, detail);
#endif

  emit_status();
}

void loop() {
  ++g_loop_count;
  poll_serial_commands();
  project_test_tick();
  emit_heartbeat();
  delay(1);
}
