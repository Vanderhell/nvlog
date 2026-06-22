#include "project_adapter.h"

extern "C" {
#include "../../../include/nvlog.h"
#include "../../../include/nvlog_hal_flash.h"
}

#include "bench_config.h"
#include "bench_hal.h"

#include <esp_heap_caps.h>
#include <SD_MMC.h>
#include <Preferences.h>
#include <esp_partition.h>
#include <esp_system.h>
#include <driver/gpio.h>
#include <driver/rtc_io.h>
#include <setjmp.h>
#include <stdio.h>
#include <string.h>

namespace {

static const char kNamespace[] = "nvlog_bench";
static const char kStateKey[] = "state";
static const char kModeKey[] = "mode";
static const char kMechanismKey[] = "mech";
// Use the existing data partition in the Arduino partition table as the nvlog
// flash region for the board-level recovery tests.
static const char kPartitionLabel[] = "spiffs";
static const uint32_t kStateMagic = 0x4E564C42u; /* NVLB */
static const uint32_t kStateVersion = 3u;
static const uint32_t kScenarioCount = 13u;
static const uint32_t kRestartDelayMs = 700u;

enum ScenarioStatus : uint8_t {
  kPending = 0,
  kPass = 1,
  kFail = 2,
  kSkip = 3,
};

struct PersistedState {
  uint32_t magic;
  uint32_t version;
  uint32_t boot_count;
  uint32_t scenario_index;
  uint32_t phase_index;
  uint32_t iteration;
  uint32_t writes;
  uint32_t reads;
  uint32_t recovered;
  uint32_t errors;
  uint32_t pass_count;
  uint32_t skip_count;
  uint32_t fail_count;
  uint32_t last_result;
  uint32_t endurance_target;
  uint32_t fault_mode_enabled;
  uint32_t restart_at_ms;
  uint32_t pending_restart;
  uint32_t reserved[8];
};

struct App {
  Preferences prefs;
  nvlog_ctx_t log_ctx;
  nvlog_hal_flash_t flash = {};
  const esp_partition_t *partition = nullptr;
  PersistedState state = {};
  bool state_dirty = false;
  bool ready = false;
  bool session_done = false;
  bool mode_received = false;
  bool fault_mode = false;
  bool progress_received = false;
  uint32_t progress_cycle = 0u;
  uint32_t progress_target = 0u;
  char fault_mechanism[32] = "USB_LOGICAL_DISCONNECT";
  char armed_failpoint[32] = {0};
  bool failpoint_enabled = false;
  jmp_buf failpoint_jump;
  char progress_label[32] = "waiting";
  char last_detail[96] = {0};
};

static App g_app;

struct ScratchRegion {
  uint8_t *storage = nullptr;
  uint32_t size = 0u;
};

static void emit_field(const char *key, const char *value)
{
  Serial.print('|');
  Serial.print(key);
  Serial.print('=');
  Serial.print(value != nullptr ? value : "");
}

static void emit_field_u32(const char *key, uint32_t value)
{
  Serial.print('|');
  Serial.print(key);
  Serial.print('=');
  Serial.print(value);
}

static void emit_field_hex_ptr(const char *key, const void *value)
{
  char text[24];
  snprintf(text, sizeof(text), "0x%08lX", static_cast<unsigned long>(reinterpret_cast<uintptr_t>(value)));
  emit_field(key, text);
}

static void emit_event(const char *event_name)
{
  Serial.print("NVLOG|");
  Serial.print(event_name);
}

static void emit_line(const char *event_name)
{
  emit_event(event_name);
  Serial.println();
}

static void sanitize(char *text)
{
  if (text == nullptr) return;
  for (; *text != '\0'; ++text) {
    if (*text == '\r' || *text == '\n' || *text == '|') {
      *text = '_';
    }
  }
}

static const char *nvlog_status_name(nvlog_status_t status)
{
  switch (status) {
    case NVLOG_OK: return "NVLOG_OK";
    case NVLOG_ERR_PARAM: return "NVLOG_ERR_PARAM";
    case NVLOG_ERR_FULL: return "NVLOG_ERR_FULL";
    case NVLOG_ERR_IO: return "NVLOG_ERR_IO";
    case NVLOG_ERR_CORRUPT: return "NVLOG_ERR_CORRUPT";
    case NVLOG_ERR_NO_DATA: return "NVLOG_ERR_NO_DATA";
    case NVLOG_ERR_TOO_LARGE: return "NVLOG_ERR_TOO_LARGE";
    case NVLOG_ERR_NOT_MOUNTED: return "NVLOG_ERR_NOT_MOUNTED";
    case NVLOG_ERR_STALE: return "NVLOG_ERR_STALE";
    case NVLOG_ERR_BOUNDS: return "NVLOG_ERR_BOUNDS";
    case NVLOG_ERR_UNSUPPORTED: return "NVLOG_ERR_UNSUPPORTED";
    case NVLOG_ERR_END: return "NVLOG_ERR_END";
    case NVLOG_ERR_INCOMPLETE: return "NVLOG_ERR_INCOMPLETE";
    case NVLOG_ERR_VERSION: return "NVLOG_ERR_VERSION";
    case NVLOG_ERR_TYPE: return "NVLOG_ERR_TYPE";
    case NVLOG_ERR_FLAGS: return "NVLOG_ERR_FLAGS";
    case NVLOG_ERR_RESERVED: return "NVLOG_ERR_RESERVED";
    case NVLOG_ERR_MODE_MISMATCH: return "NVLOG_ERR_MODE_MISMATCH";
    case NVLOG_ERR_GENERATION_MISMATCH: return "NVLOG_ERR_GENERATION_MISMATCH";
    case NVLOG_ERR_SIZE_MISMATCH: return "NVLOG_ERR_SIZE_MISMATCH";
    case NVLOG_ERR_MEDIA_MISMATCH: return "NVLOG_ERR_MEDIA_MISMATCH";
    default: return "NVLOG_ERR_UNKNOWN";
  }
}

static void display_clear()
{
  bench_display_clear();
}

static void display_line(uint8_t line, const char *text)
{
  bench_display_write_line(line, text);
}

static int scratch_read(uint32_t addr, void *buf, uint32_t len, void *user)
{
  ScratchRegion *region = static_cast<ScratchRegion *>(user);
  if (region == nullptr || buf == nullptr || addr > region->size || len > region->size - addr) {
    return -1;
  }
  memcpy(buf, region->storage + addr, len);
  return 0;
}

static int scratch_write(uint32_t addr, const void *buf, uint32_t len, void *user)
{
  ScratchRegion *region = static_cast<ScratchRegion *>(user);
  if (region == nullptr || buf == nullptr || addr > region->size || len > region->size - addr) {
    return -1;
  }
  memcpy(region->storage + addr, buf, len);
  return 0;
}

static void copy_token_value(const char *source, const char *prefix, char *dest, size_t dest_size)
{
  if (source == nullptr || prefix == nullptr || dest == nullptr || dest_size == 0U) {
    return;
  }

  const char *value = strstr(source, prefix);
  if (value == nullptr) {
    return;
  }

  value += strlen(prefix);
  size_t i = 0U;
  while (value[i] != '\0' && value[i] != ' ' && i + 1U < dest_size) {
    char ch = value[i];
    if (ch == '\r' || ch == '\n' || ch == '|') {
      ch = '_';
    }
    dest[i++] = ch;
  }
  dest[i] = '\0';
}

static uint32_t parse_u32_token(const char *source, const char *prefix, uint32_t fallback)
{
  const char *value = strstr(source, prefix);
  if (value == nullptr) {
    return fallback;
  }
  value += strlen(prefix);
  return static_cast<uint32_t>(strtoul(value, nullptr, 10));
}

static bool scratch_alloc(ScratchRegion *region, uint32_t size)
{
  if (region == nullptr || size == 0u) {
    return false;
  }
  // Keep the scratch buffer aligned so PSRAM-backed ring tests are stable
  // across allocators and board variants.
  region->storage = static_cast<uint8_t *>(
      heap_caps_aligned_alloc(32u, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (region->storage == nullptr) {
    region->storage = static_cast<uint8_t *>(
        heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  }
  if (region->storage == nullptr) {
    region->size = 0u;
    return false;
  }
  region->size = size;
  memset(region->storage, 0xFF, region->size);
  return true;
}

static void scratch_free(ScratchRegion *region)
{
  if (region == nullptr || region->storage == nullptr) {
    return;
  }
  heap_caps_free(region->storage);
  region->storage = nullptr;
  region->size = 0u;
}

static void set_progress_from_command(const char *command)
{
  g_app.progress_received = true;
  g_app.progress_cycle = parse_u32_token(command, "cycle=", g_app.progress_cycle);
  g_app.progress_target = parse_u32_token(command, "target=", g_app.progress_target);
  copy_token_value(command, "label=", g_app.progress_label, sizeof(g_app.progress_label));
  sanitize(g_app.progress_label);
  snprintf(g_app.last_detail, sizeof(g_app.last_detail), "progress=%lu/%lu/%s",
           static_cast<unsigned long>(g_app.progress_cycle),
           static_cast<unsigned long>(g_app.progress_target),
           g_app.progress_label);
}

static void refresh_display()
{
  char line[64];
  const char *scenario_names[kScenarioCount] = {
    "empty_init",
    "single_append",
    "reboot_recovery",
    "ordering",
    "payload_sizes",
    "capacity_boundary",
    "interrupted_append",
    "repeated_reboot",
    "disconnect_stress",
    "ring_failpoint_smoke",
    "psram_api_smoke",
    "sd_api_smoke",
    "endurance_smoke",
  };
  const char *result = "WAIT_MODE";
  if (g_app.state.last_result == kPass) {
    result = "PASS";
  } else if (g_app.state.last_result == kFail) {
    result = "FAIL";
  } else if (g_app.state.last_result == kSkip) {
    result = "SKIPPED";
  } else if (g_app.mode_received) {
    result = g_app.session_done ? "DONE" : "RUNNING";
  }

  display_clear();
  display_line(0, "NVLOG HW TEST");
  snprintf(line, sizeof(line), "SCN:%s", scenario_names[g_app.state.scenario_index < kScenarioCount ? g_app.state.scenario_index : (kScenarioCount - 1u)]);
  display_line(1, line);
  snprintf(line, sizeof(line), "PH:%lu IT:%lu",
           static_cast<unsigned long>(g_app.state.phase_index),
           static_cast<unsigned long>(g_app.state.iteration));
  display_line(2, line);
  snprintf(line, sizeof(line), "W:%lu R:%lu REC:%lu",
           static_cast<unsigned long>(g_app.state.writes),
           static_cast<unsigned long>(g_app.state.reads),
           static_cast<unsigned long>(g_app.state.recovered));
  display_line(3, line);
  snprintf(line, sizeof(line), "ERR:%lu BOOT:%lu",
           static_cast<unsigned long>(g_app.state.errors),
           static_cast<unsigned long>(g_app.state.boot_count));
  display_line(4, line);
  snprintf(line, sizeof(line), "HOST:%lu/%lu %s",
           static_cast<unsigned long>(g_app.progress_cycle),
           static_cast<unsigned long>(g_app.progress_target),
           g_app.progress_received ? g_app.progress_label : "waiting");
  display_line(5, line);
  snprintf(line, sizeof(line), "FAULT:%s", g_app.fault_mode ? "ON" : "OFF");
  display_line(6, line);
  snprintf(line, sizeof(line), "RES:%s", result);
  display_line(7, line);
  snprintf(line, sizeof(line), "STEP:%.50s", g_app.last_detail[0] != '\0' ? g_app.last_detail : "idle");
  display_line(8, line);
  snprintf(line, sizeof(line), "MODE:%s", g_app.mode_received ? "RUN" : "WAIT");
  display_line(9, line);
}

static void show_final_pass()
{
  display_clear();
  display_line(0, "SESSION PASS");
  display_line(1, "NVLog");
}

static void show_final_fail()
{
  display_clear();
  display_line(0, "SESSION FAIL");
  display_line(1, "NVLog");
}

static void mark_dirty()
{
  g_app.state_dirty = true;
}

static void save_state()
{
  if (!g_app.state_dirty) return;
  g_app.prefs.putBytes(kStateKey, &g_app.state, sizeof(g_app.state));
  g_app.state_dirty = false;
}

static void load_state()
{
  memset(&g_app.state, 0, sizeof(g_app.state));
  g_app.state.magic = kStateMagic;
  g_app.state.version = kStateVersion;
  g_app.state.endurance_target = 8u;
  g_app.state.fault_mode_enabled = 0u;
  g_app.state.restart_at_ms = 0u;
  g_app.state.pending_restart = 0u;
  size_t read = g_app.prefs.getBytes(kStateKey, &g_app.state, sizeof(g_app.state));
  if (read != sizeof(g_app.state) ||
      g_app.state.magic != kStateMagic ||
      g_app.state.version != kStateVersion) {
    memset(&g_app.state, 0, sizeof(g_app.state));
    g_app.state.magic = kStateMagic;
    g_app.state.version = kStateVersion;
    g_app.state.endurance_target = 8u;
    g_app.state.fault_mode_enabled = 0u;
  }
}

static const esp_partition_t *find_partition()
{
  return esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, kPartitionLabel);
}

static int part_read(uint32_t addr, void *buf, uint32_t len, void *user)
{
  const esp_partition_t *part = static_cast<const esp_partition_t *>(user);
  return esp_partition_read(part, addr, buf, len) == ESP_OK ? 0 : -1;
}

static int part_write(uint32_t addr, const void *buf, uint32_t len, void *user)
{
  const esp_partition_t *part = static_cast<const esp_partition_t *>(user);
  return esp_partition_write(part, addr, buf, len) == ESP_OK ? 0 : -1;
}

static int part_erase(uint32_t addr, uint32_t len, void *user)
{
  const esp_partition_t *part = static_cast<const esp_partition_t *>(user);
  return esp_partition_erase_range(part, addr, len) == ESP_OK ? 0 : -1;
}

static void prepare_flash()
{
  g_app.partition = find_partition();
  g_app.flash.base.read = part_read;
  g_app.flash.base.write = part_write;
  g_app.flash.base.user = const_cast<esp_partition_t *>(g_app.partition);
  g_app.flash.erase = part_erase;
  g_app.flash.erase_size = g_app.partition != nullptr ? g_app.partition->erase_size : 0u;
  g_app.flash.prog_size = 32u;
  g_app.flash.user = const_cast<esp_partition_t *>(g_app.partition);
}

static void emit_boot()
{
  emit_event("BOOT");
  emit_field_u32("boot", g_app.state.boot_count);
  Serial.println();
}

static void emit_ready()
{
  emit_line("READY");
}

static void emit_status()
{
  const char *scenario_name = "unknown";
  if (g_app.state.scenario_index < kScenarioCount) {
  const char *scenario_names[kScenarioCount] = {
    "empty_init",
    "single_append",
    "reboot_recovery",
      "ordering",
      "payload_sizes",
      "capacity_boundary",
      "interrupted_append",
    "repeated_reboot",
    "disconnect_stress",
    "ring_failpoint_smoke",
    "psram_api_smoke",
    "sd_api_smoke",
    "endurance_smoke",
  };
    scenario_name = scenario_names[g_app.state.scenario_index];
  }
  const char *result = "WAIT_MODE";
  if (g_app.state.last_result == kPass) result = "PASS";
  else if (g_app.state.last_result == kFail) result = "FAIL";
  else if (g_app.state.last_result == kSkip) result = "SKIPPED";
  else if (g_app.mode_received) result = g_app.session_done ? "DONE" : "RUNNING";

  emit_event("STATUS");
  emit_field("scenario", scenario_name);
  emit_field_u32("iteration", g_app.state.iteration);
  emit_field_u32("writes", g_app.state.writes);
  emit_field_u32("reads", g_app.state.reads);
  emit_field_u32("recovered", g_app.state.recovered);
  emit_field_u32("errors", g_app.state.errors);
  emit_field_u32("boot_count", g_app.state.boot_count);
  emit_field_u32("progress_cycle", g_app.progress_cycle);
  emit_field_u32("progress_target", g_app.progress_target);
  emit_field("fault", g_app.fault_mode ? "ON" : "OFF");
  emit_field("result", result);
  Serial.println();
}

static void set_mode_from_command(const char *command)
{
  g_app.fault_mode = strstr(command, "faults=enabled") != nullptr;
  g_app.mode_received = true;
  g_app.state.fault_mode_enabled = g_app.fault_mode ? 1u : 0u;
  snprintf(g_app.last_detail, sizeof(g_app.last_detail), "mode=%s", g_app.fault_mode ? "enabled" : "disabled");
  const char *mech = strstr(command, "mechanism=");
  if (mech != nullptr) {
    mech += 10;
    size_t i = 0;
    while (mech[i] != '\0' && mech[i] != ' ' && i + 1 < sizeof(g_app.fault_mechanism)) {
      g_app.fault_mechanism[i] = mech[i];
      ++i;
    }
    g_app.fault_mechanism[i] = '\0';
    sanitize(g_app.fault_mechanism);
  }
  mark_dirty();
  save_state();
}

static void arm_failpoint(const char *name)
{
  if (name == nullptr) {
    g_app.armed_failpoint[0] = '\0';
    g_app.failpoint_enabled = false;
    return;
  }
  snprintf(g_app.armed_failpoint, sizeof(g_app.armed_failpoint), "%s", name);
  g_app.failpoint_enabled = true;
}

static void disarm_failpoint()
{
  g_app.armed_failpoint[0] = '\0';
  g_app.failpoint_enabled = false;
}

extern "C" void nvlog_test_failpoint(const char *name)
{
  if (!g_app.failpoint_enabled || name == nullptr) {
    return;
  }
  if (strcmp(name, g_app.armed_failpoint) != 0) {
    return;
  }

  emit_event("FAILPOINT");
  emit_field("name", name);
  Serial.println();
  disarm_failpoint();
  longjmp(g_app.failpoint_jump, 1);
}

static void fill_payload(uint8_t *buf, size_t len, uint32_t scenario, uint32_t salt)
{
  for (size_t i = 0; i < len; ++i) {
    buf[i] = static_cast<uint8_t>((scenario * 17u + salt * 31u + i) & 0xFFu);
  }
}

static bool append_record_to(nvlog_ctx_t *ctx, uint32_t scenario, uint32_t salt, size_t len)
{
  if (ctx == nullptr) {
    return false;
  }
  uint8_t payload[1024];
  if (len > sizeof(payload)) {
    return false;
  }
  fill_payload(payload, len, scenario, salt);
  nvlog_status_t st = nvlog_append(ctx, payload, len);
  if (st != NVLOG_OK) {
    snprintf(g_app.last_detail, sizeof(g_app.last_detail), "append_%d", (int)st);
    g_app.state.errors++;
    mark_dirty();
    return false;
  }
  g_app.state.writes++;
  g_app.state.iteration++;
  mark_dirty();
  return true;
}

static bool append_record(uint32_t scenario, uint32_t salt, size_t len)
{
  return append_record_to(&g_app.log_ctx, scenario, salt, len);
}

static bool append_payload_to_ctx(nvlog_ctx_t *ctx, uint32_t scenario, uint32_t salt, size_t len)
{
  if (ctx == nullptr) {
    return false;
  }
  uint8_t payload[4096];
  if (len > sizeof(payload)) {
    return false;
  }
  fill_payload(payload, len, scenario, salt);
  return nvlog_append(ctx, payload, len) == NVLOG_OK;
}

static bool verify_records_from(nvlog_ctx_t *ctx, uint32_t scenario, const uint32_t *sizes, size_t count)
{
  if (ctx == nullptr) {
    return false;
  }
  nvlog_iter_t it;
  nvlog_record_t rec;
  if (nvlog_iter_init(&it, ctx) != NVLOG_OK) {
    g_app.state.errors++;
    mark_dirty();
    return false;
  }
  for (size_t i = 0; i < count; ++i) {
    if (nvlog_iter_next(&it, &rec) != NVLOG_OK) {
      g_app.state.errors++;
      mark_dirty();
      return false;
    }
    if (rec.len != sizes[i]) {
      g_app.state.errors++;
      mark_dirty();
      return false;
    }
    uint8_t payload[1024];
    uint8_t expected[1024];
    if (rec.len > sizeof(payload)) {
      return false;
    }
    if (nvlog_read_payload(ctx, &rec, payload, sizeof(payload)) != NVLOG_OK) {
      g_app.state.errors++;
      mark_dirty();
      return false;
    }
    fill_payload(expected, rec.len, scenario, static_cast<uint32_t>(i));
    if (memcmp(payload, expected, rec.len) != 0) {
      g_app.state.errors++;
      mark_dirty();
      return false;
    }
    g_app.state.reads++;
    g_app.state.recovered++;
  }
  mark_dirty();
  return nvlog_iter_next(&it, &rec) == NVLOG_ERR_NO_DATA;
}

static bool verify_records(uint32_t scenario, const uint32_t *sizes, size_t count)
{
  return verify_records_from(&g_app.log_ctx, scenario, sizes, count);
}

static bool reset_flash_log();

static void emit_scenario_start(const char *id)
{
  emit_event("SCENARIO_START");
  emit_field("id", id);
  Serial.println();
}

static void emit_phase(const char *name)
{
  emit_event("PHASE");
  emit_field("name", name);
  Serial.println();
}

static void emit_scenario_pass(const char *id)
{
  emit_event("SCENARIO_PASS");
  emit_field("id", id);
  Serial.println();
}

static void emit_scenario_fail(const char *id, const char *code, const char *reason)
{
  emit_event("SCENARIO_FAIL");
  emit_field("id", id);
  emit_field("code", code);
  if (reason != nullptr && reason[0] != '\0') {
    emit_field("reason", reason);
  }
  Serial.println();
}

static void emit_scenario_skip(const char *id, const char *reason)
{
  emit_event("SCENARIO_SKIPPED");
  emit_field("id", id);
  emit_field("reason", reason);
  Serial.println();
}

static void schedule_restart()
{
  g_app.state.pending_restart = 1u;
  g_app.state.restart_at_ms = millis() + kRestartDelayMs;
  mark_dirty();
  save_state();
}

static void enter_bootloader_mode()
{
#if defined(GPIO_NUM_0)
  rtc_gpio_init(GPIO_NUM_0);
  rtc_gpio_set_direction(GPIO_NUM_0, RTC_GPIO_MODE_OUTPUT_ONLY);
  rtc_gpio_set_level(GPIO_NUM_0, 0);
  rtc_gpio_hold_en(GPIO_NUM_0);
#endif
}

static bool scenario_empty()
{
  emit_scenario_start("empty_initialization");
  if (!reset_flash_log()) {
    emit_scenario_fail("empty_initialization", "FORMAT", "flash reset failed");
    g_app.state.fail_count++;
    g_app.state.last_result = kFail;
    return false;
  }
  emit_phase("verify_empty");
  nvlog_iter_t it;
  nvlog_record_t rec;
  if (nvlog_iter_init(&it, &g_app.log_ctx) != NVLOG_OK) {
    emit_scenario_fail("empty_initialization", "ITER_INIT", "iterator init failed");
    g_app.state.fail_count++;
    g_app.state.last_result = kFail;
    return false;
  }
  if (nvlog_iter_next(&it, &rec) != NVLOG_ERR_NO_DATA) {
    emit_scenario_fail("empty_initialization", "NOT_EMPTY", "log not empty");
    g_app.state.fail_count++;
    g_app.state.last_result = kFail;
    return false;
  }
  emit_scenario_pass("empty_initialization");
  g_app.state.pass_count++;
  g_app.state.last_result = kPass;
  return true;
}

static bool scenario_single_append()
{
  emit_scenario_start("single_append");
  if (!reset_flash_log()) {
    emit_scenario_fail("single_append", "FORMAT", "flash reset failed");
    g_app.state.fail_count++;
    g_app.state.last_result = kFail;
    return false;
  }
  const uint32_t sizes[] = { 16u };
  emit_phase("append");
  if (!append_record(1u, 0u, sizes[0])) {
    emit_scenario_fail("single_append", "APPEND", "append failed");
    g_app.state.fail_count++;
    g_app.state.last_result = kFail;
    return false;
  }
  emit_phase("readback");
  if (!verify_records(1u, sizes, 1u)) {
    emit_scenario_fail("single_append", "READ_MISMATCH", "readback failed");
    g_app.state.fail_count++;
    g_app.state.last_result = kFail;
    return false;
  }
  emit_scenario_pass("single_append");
  g_app.state.pass_count++;
  g_app.state.last_result = kPass;
  return true;
}

static bool scenario_reboot_recovery()
{
  emit_scenario_start("reboot_recovery");
  const uint32_t sizes[] = { 24u, 24u };
  if (g_app.state.phase_index == 0u) {
    if (!reset_flash_log()) {
      emit_scenario_fail("reboot_recovery", "FORMAT", "flash reset failed");
      g_app.state.fail_count++;
      g_app.state.last_result = kFail;
      return false;
    }
    emit_phase("append_before_reboot");
    if (!append_record(2u, 0u, sizes[0])) {
      emit_scenario_fail("reboot_recovery", "APPEND", "initial append failed");
      g_app.state.fail_count++;
      g_app.state.last_result = kFail;
      return false;
    }
    g_app.state.phase_index = 1u;
    schedule_restart();
    emit_status();
    return true;
  }
  emit_phase("recovery");
  if (!verify_records(2u, sizes, 1u)) {
    emit_scenario_fail("reboot_recovery", "RECOVERY", "missing committed record");
    g_app.state.fail_count++;
    g_app.state.last_result = kFail;
    return false;
  }
  emit_phase("append_after_reboot");
  if (!append_record(2u, 1u, sizes[1])) {
    emit_scenario_fail("reboot_recovery", "APPEND", "post reboot append failed");
    g_app.state.fail_count++;
    g_app.state.last_result = kFail;
    return false;
  }
  if (!verify_records(2u, sizes, 2u)) {
    emit_scenario_fail("reboot_recovery", "ORDER", "ordering failed after reboot");
    g_app.state.fail_count++;
    g_app.state.last_result = kFail;
    return false;
  }
  emit_scenario_pass("reboot_recovery");
  g_app.state.pass_count++;
  g_app.state.last_result = kPass;
  g_app.state.phase_index = 0u;
  return true;
}

static bool scenario_ordering()
{
  emit_scenario_start("sequential_records");
  if (!reset_flash_log()) {
    emit_scenario_fail("sequential_records", "FORMAT", "flash reset failed");
    g_app.state.fail_count++;
    g_app.state.last_result = kFail;
    return false;
  }
  const uint32_t sizes[] = { 8u, 16u, 24u };
  for (uint32_t i = 0; i < 3u; ++i) {
    char phase[32];
    snprintf(phase, sizeof(phase), "append_%lu", static_cast<unsigned long>(i));
    emit_phase(phase);
    if (!append_record(3u, i, sizes[i])) {
      emit_scenario_fail("sequential_records", "APPEND", "append failed");
      g_app.state.fail_count++;
      g_app.state.last_result = kFail;
      return false;
    }
  }
  emit_phase("verify");
  if (!verify_records(3u, sizes, 3u)) {
    emit_scenario_fail("sequential_records", "ORDER", "record ordering mismatch");
    g_app.state.fail_count++;
    g_app.state.last_result = kFail;
    return false;
  }
  emit_scenario_pass("sequential_records");
  g_app.state.pass_count++;
  g_app.state.last_result = kPass;
  return true;
}

static bool scenario_payload_sizes()
{
  emit_scenario_start("payload_sizes");
  if (!reset_flash_log()) {
    emit_scenario_fail("payload_sizes", "FORMAT", "flash reset failed");
    g_app.state.fail_count++;
    g_app.state.last_result = kFail;
    return false;
  }
  const uint32_t sizes[] = { 1u, 4u, 8u, 32u, 63u, 128u, 256u };
  for (uint32_t i = 0; i < (sizeof(sizes) / sizeof(sizes[0])); ++i) {
    char phase[32];
    snprintf(phase, sizeof(phase), "size_%lu", static_cast<unsigned long>(sizes[i]));
    emit_phase(phase);
    if (!append_record(4u, i, sizes[i])) {
      emit_scenario_fail("payload_sizes", "APPEND", "boundary append failed");
      g_app.state.fail_count++;
      g_app.state.last_result = kFail;
      return false;
    }
  }
  if (!verify_records(4u, sizes, sizeof(sizes) / sizeof(sizes[0]))) {
    emit_scenario_fail("payload_sizes", "VERIFY", "boundary verify failed");
    g_app.state.fail_count++;
    g_app.state.last_result = kFail;
    return false;
  }
  emit_scenario_pass("payload_sizes");
  g_app.state.pass_count++;
  g_app.state.last_result = kPass;
  return true;
}

static bool scenario_capacity_boundary()
{
  emit_scenario_start("capacity_boundary");
  if (!reset_flash_log()) {
    emit_scenario_fail("capacity_boundary", "FORMAT", "flash reset failed");
    g_app.state.fail_count++;
    g_app.state.last_result = kFail;
    return false;
  }
  emit_phase("fill_until_full");
  uint32_t count = 0u;
  while (count < 32u) {
    if (!append_record(5u, count, 64u)) {
      break;
    }
    ++count;
  }
  if (count == 0u) {
    emit_scenario_fail("capacity_boundary", "EMPTY", "could not fill log");
    g_app.state.fail_count++;
    g_app.state.last_result = kFail;
    return false;
  }
  emit_scenario_pass("capacity_boundary");
  g_app.state.pass_count++;
  g_app.state.last_result = kPass;
  return true;
}

static bool scenario_interrupted_append()
{
  emit_scenario_start("interrupted_append");
  if (!g_app.fault_mode) {
    emit_scenario_skip("interrupted_append", "faults_disabled");
    g_app.state.skip_count++;
    g_app.state.last_result = kSkip;
    return true;
  }
  const uint32_t sizes[] = { 48u };
  if (g_app.state.phase_index == 0u) {
    if (!reset_flash_log()) {
      emit_scenario_fail("interrupted_append", "FORMAT", "flash reset failed");
      g_app.state.fail_count++;
      g_app.state.last_result = kFail;
      return false;
    }
    emit_phase("arm_fault_window");
    g_app.state.phase_index = 1u;
    schedule_restart();
    return true;
  }
  emit_phase("append");
  if (!append_record(6u, 0u, sizes[0])) {
    emit_scenario_fail("interrupted_append", "APPEND", "append failed");
    g_app.state.fail_count++;
    g_app.state.last_result = kFail;
    return false;
  }
  emit_phase("recovery");
  if (!verify_records(6u, sizes, 1u)) {
    emit_scenario_fail("interrupted_append", "RECOVERY", "record recovery failed");
    g_app.state.fail_count++;
    g_app.state.last_result = kFail;
    return false;
  }
  emit_scenario_pass("interrupted_append");
  g_app.state.pass_count++;
  g_app.state.last_result = kPass;
  g_app.state.phase_index = 0u;
  return true;
}

static bool scenario_repeated_reboot()
{
  emit_scenario_start("repeated_reboot");
  const uint32_t sizes[] = { 24u, 24u };
  if (g_app.state.phase_index == 0u) {
    if (!reset_flash_log()) {
      emit_scenario_fail("repeated_reboot", "FORMAT", "flash reset failed");
      g_app.state.fail_count++;
      g_app.state.last_result = kFail;
      return false;
    }
    emit_phase("boot_0");
    if (!append_record(7u, 0u, sizes[0])) {
      emit_scenario_fail("repeated_reboot", "APPEND0", "append failed");
      g_app.state.fail_count++;
      g_app.state.last_result = kFail;
      return false;
    }
    g_app.state.phase_index = 1u;
    schedule_restart();
    return true;
  }
  if (g_app.state.phase_index == 1u) {
    emit_phase("boot_1");
    if (!verify_records(7u, sizes, 1u)) {
      emit_scenario_fail("repeated_reboot", "VERIFY0", "missing first record");
      g_app.state.fail_count++;
      g_app.state.last_result = kFail;
      return false;
    }
    if (!append_record(7u, 1u, sizes[1])) {
      emit_scenario_fail("repeated_reboot", "APPEND1", "append failed");
      g_app.state.fail_count++;
      g_app.state.last_result = kFail;
      return false;
    }
    g_app.state.phase_index = 2u;
    schedule_restart();
    return true;
  }
  emit_phase("boot_2");
  if (!verify_records(7u, sizes, 2u)) {
    emit_scenario_fail("repeated_reboot", "VERIFY1", "second recovery failed");
    g_app.state.fail_count++;
    g_app.state.last_result = kFail;
    return false;
  }
  emit_scenario_pass("repeated_reboot");
  g_app.state.pass_count++;
  g_app.state.last_result = kPass;
  g_app.state.phase_index = 0u;
  return true;
}

static bool scenario_disconnect_stress()
{
  emit_scenario_start("disconnect_stress");
  if (!g_app.fault_mode) {
    emit_scenario_skip("disconnect_stress", "faults_disabled");
    g_app.state.skip_count++;
    g_app.state.last_result = kSkip;
    return true;
  }
  if (!reset_flash_log()) {
    emit_scenario_fail("disconnect_stress", "FORMAT", "flash reset failed");
    g_app.state.fail_count++;
    g_app.state.last_result = kFail;
    return false;
  }
  for (uint32_t i = 0u; i < 4u; ++i) {
    char phase[32];
    snprintf(phase, sizeof(phase), "stress_%lu", static_cast<unsigned long>(i));
    emit_phase(phase);
    if (!append_record(8u, i, 32u + (i * 8u))) {
      emit_scenario_fail("disconnect_stress", "APPEND", "stress append failed");
      g_app.state.fail_count++;
      g_app.state.last_result = kFail;
      return false;
    }
    delay(250u);
  }
  emit_scenario_pass("disconnect_stress");
  g_app.state.pass_count++;
  g_app.state.last_result = kPass;
  return true;
}

static bool scenario_corruption_detection()
{
  emit_scenario_start("ring_failpoint_smoke");
  if (!g_app.fault_mode) {
    emit_scenario_skip("ring_failpoint_smoke", "faults_disabled");
    g_app.state.skip_count++;
    g_app.state.last_result = kSkip;
    return true;
  }
  if (!bench_capabilities().psram_ready) {
    emit_scenario_skip("ring_failpoint_smoke", "psram_unavailable");
    g_app.state.skip_count++;
    g_app.state.last_result = kSkip;
    return true;
  }

  emit_phase("alloc_psram");
  ScratchRegion region;
  if (!scratch_alloc(&region, 96u * 1024u)) {
    emit_scenario_fail("ring_failpoint_smoke", "ALLOC", "psram allocation failed");
    g_app.state.fail_count++;
    g_app.state.last_result = kFail;
    return false;
  }

  nvlog_hal_t hal{};
  hal.read = scratch_read;
  hal.write = scratch_write;
  hal.user = &region;

  bool ok = true;
  const uint32_t ring_region = 80u * 1024u;
  const uint32_t baseline_size = 2048u;
  const uint32_t wrap_fail_size = 4096u;
  const uint32_t wrap_sizes[] = {
    baseline_size, baseline_size, baseline_size, baseline_size,
    baseline_size, baseline_size, baseline_size
  };
  const uint32_t commit_sizes[] = { baseline_size };
  const uint32_t publish_sizes[] = { baseline_size, baseline_size };

  emit_phase("wrap_failpoint");
  memset(region.storage, 0xFF, region.size);
  {
    nvlog_ctx_t ring_ctx;
    nvlog_ctx_init(&ring_ctx);
    ok = nvlog_ring_format(&ring_ctx, &hal, ring_region) == NVLOG_OK;
    for (uint32_t i = 0u; ok && i < (sizeof(wrap_sizes) / sizeof(wrap_sizes[0])); ++i) {
      ok = append_payload_to_ctx(&ring_ctx, 30u, i, baseline_size);
    }
    if (ok) {
      volatile int failpoint_hit = 0;
      arm_failpoint("wrap");
      if (setjmp(g_app.failpoint_jump) == 0) {
        if (append_payload_to_ctx(&ring_ctx, 30u, 7u, wrap_fail_size)) {
          failpoint_hit = -1;
        }
      } else {
        failpoint_hit = 1;
      }
      disarm_failpoint();
      if (failpoint_hit != 1) {
        ok = false;
        snprintf(g_app.last_detail, sizeof(g_app.last_detail), "wrap_failpoint_missing");
      }
    }
    if (ok) {
      nvlog_ctx_t mounted;
      nvlog_ctx_init(&mounted);
      ok = nvlog_ring_mount(&mounted, &hal, ring_region) == NVLOG_OK &&
           verify_records_from(&mounted, 30u, wrap_sizes, sizeof(wrap_sizes) / sizeof(wrap_sizes[0]));
      if (!ok) {
        snprintf(g_app.last_detail, sizeof(g_app.last_detail), "wrap_recovery_fail");
      }
    }
  }

  if (ok) {
    emit_phase("data_commit_failpoint");
    memset(region.storage, 0xFF, region.size);
    nvlog_ctx_t ring_ctx;
    nvlog_ctx_init(&ring_ctx);
    ok = nvlog_ring_format(&ring_ctx, &hal, ring_region) == NVLOG_OK;
    if (ok) {
      ok = append_payload_to_ctx(&ring_ctx, 31u, 0u, baseline_size);
    }
    if (ok) {
      volatile int failpoint_hit = 0;
      arm_failpoint("data_commit");
      if (setjmp(g_app.failpoint_jump) == 0) {
        if (append_payload_to_ctx(&ring_ctx, 31u, 1u, baseline_size)) {
          failpoint_hit = -1;
        }
      } else {
        failpoint_hit = 1;
      }
      disarm_failpoint();
      if (failpoint_hit != 1) {
        ok = false;
        snprintf(g_app.last_detail, sizeof(g_app.last_detail), "data_commit_failpoint_missing");
      }
    }
    if (ok) {
      nvlog_ctx_t mounted;
      nvlog_ctx_init(&mounted);
      ok = nvlog_ring_mount(&mounted, &hal, ring_region) == NVLOG_OK &&
           verify_records_from(&mounted, 31u, commit_sizes, sizeof(commit_sizes) / sizeof(commit_sizes[0]));
      if (!ok) {
        snprintf(g_app.last_detail, sizeof(g_app.last_detail), "data_commit_recovery_fail");
      }
    }
  }

  if (ok) {
    emit_phase("superblock_publish_failpoint");
    memset(region.storage, 0xFF, region.size);
    nvlog_ctx_t ring_ctx;
    nvlog_ctx_init(&ring_ctx);
    ok = nvlog_ring_format(&ring_ctx, &hal, ring_region) == NVLOG_OK;
    if (ok) {
      ok = append_payload_to_ctx(&ring_ctx, 32u, 0u, baseline_size);
    }
    if (ok) {
      volatile int failpoint_hit = 0;
      arm_failpoint("superblock_publish");
      if (setjmp(g_app.failpoint_jump) == 0) {
        if (append_payload_to_ctx(&ring_ctx, 32u, 1u, baseline_size)) {
          failpoint_hit = -1;
        }
      } else {
        failpoint_hit = 1;
      }
      disarm_failpoint();
      if (failpoint_hit != 1) {
        ok = false;
        snprintf(g_app.last_detail, sizeof(g_app.last_detail), "superblock_publish_missing");
      }
    }
    if (ok) {
      nvlog_ctx_t mounted;
      nvlog_ctx_init(&mounted);
      ok = nvlog_ring_mount(&mounted, &hal, ring_region) == NVLOG_OK &&
           verify_records_from(&mounted, 32u, publish_sizes, sizeof(publish_sizes) / sizeof(publish_sizes[0]));
      if (!ok) {
        snprintf(g_app.last_detail, sizeof(g_app.last_detail), "superblock_publish_recovery_fail");
      }
    }
  }

  scratch_free(&region);

  if (!ok) {
    emit_scenario_fail("ring_failpoint_smoke", "FAILPOINT", g_app.last_detail[0] != '\0' ? g_app.last_detail : "ring failpoint smoke failed");
    g_app.state.fail_count++;
    g_app.state.last_result = kFail;
    return false;
  }

  emit_scenario_pass("ring_failpoint_smoke");
  g_app.state.pass_count++;
  g_app.state.last_result = kPass;
  return true;
}

static bool scenario_psram_api_smoke()
{
  emit_scenario_start("psram_api_smoke");
  emit_phase("init_psram");

  if (!bench_capabilities().psram_ready) {
    emit_scenario_skip("psram_api_smoke", "psram_unavailable");
    g_app.state.skip_count++;
    g_app.state.last_result = kSkip;
    return true;
  }

  ScratchRegion region;
  if (!scratch_alloc(&region, 96u * 1024u)) {
    emit_scenario_fail("psram_api_smoke", "ALLOC", "psram allocation failed");
    g_app.state.fail_count++;
    g_app.state.last_result = kFail;
    return false;
  }

  nvlog_hal_t hal{};
  hal.read = scratch_read;
  hal.write = scratch_write;
  hal.user = &region;

  bool ok = true;
  const uint32_t linear_region = 48u * 1024u;
  const uint32_t ring_region = 80u * 1024u;

  nvlog_ctx_t linear_ctx;
  nvlog_ctx_init(&linear_ctx);
  if (nvlog_format(&linear_ctx, &hal, linear_region) != NVLOG_OK) {
    ok = false;
    snprintf(g_app.last_detail, sizeof(g_app.last_detail), "psram_format_fail");
  }

  if (ok) {
    emit_phase("linear_api");
    const uint32_t sizes[] = {0u, 1u, 4u, 8u, 16u, 31u, 63u};
    for (uint32_t i = 0u; ok && i < (sizeof(sizes) / sizeof(sizes[0])); ++i) {
      ok = append_record_to(&linear_ctx, 20u, i, sizes[i]);
      nvlog_stats_t stats;
      if (ok && nvlog_stats(&linear_ctx, &stats) == NVLOG_OK) {
        snprintf(g_app.last_detail, sizeof(g_app.last_detail), "linear %lu/%lu rec=%lu free=%lu",
                 static_cast<unsigned long>(i + 1u),
                 static_cast<unsigned long>(sizeof(sizes) / sizeof(sizes[0])),
                 static_cast<unsigned long>(stats.record_count),
                 static_cast<unsigned long>(stats.free_bytes));
      } else if (ok) {
        ok = false;
        snprintf(g_app.last_detail, sizeof(g_app.last_detail), "stats_fail");
      }
      delay(20u);
    }
    nvlog_ctx_t mounted;
    nvlog_ctx_init(&mounted);
    if (ok && nvlog_mount(&mounted, &hal, linear_region) == NVLOG_OK) {
      nvlog_stats_t stats;
      ok = nvlog_stats(&mounted, &stats) == NVLOG_OK &&
           stats.record_count == (sizeof(sizes) / sizeof(sizes[0])) &&
           stats.next_seq == (sizeof(sizes) / sizeof(sizes[0]));
      if (ok) {
        ok = verify_records_from(&mounted, 20u, sizes, sizeof(sizes) / sizeof(sizes[0]));
      }
    } else if (ok) {
      ok = false;
      snprintf(g_app.last_detail, sizeof(g_app.last_detail), "linear_mount_fail");
    }
  }

  if (ok) {
    emit_phase("ring_api");
    memset(region.storage, 0xFF, region.size);
    nvlog_ctx_t ring_ctx;
    nvlog_ctx_init(&ring_ctx);
    const uint32_t ring_reserve_bytes = (uint32_t)NVLOG_RECORD_OVERHEAD + (uint32_t)NVLOG_MAX_PAYLOAD;
    const uint32_t ring_min_region = (uint32_t)NVLOG_REGION_HEADER_SIZE + ring_reserve_bytes + 1u;
    const uint32_t ring_alignment = 32u;
    const uintptr_t buffer_addr = reinterpret_cast<uintptr_t>(region.storage);
    const bool buffer_external_ram = esp_ptr_external_ram(region.storage) != 0;
    const uint8_t original_byte = region.storage[0];
    region.storage[0] = (uint8_t)(original_byte ^ 0x5Au);
    const bool buffer_writable = region.storage[0] == (uint8_t)(original_byte ^ 0x5Au);
    region.storage[0] = original_byte;

    emit_event("RING_FORMAT_DIAG");
    emit_field_u32("region_size", ring_region);
    emit_field_u32("hal_read", hal.read != nullptr ? 1u : 0u);
    emit_field_u32("hal_write", hal.write != nullptr ? 1u : 0u);
    emit_field_hex_ptr("buffer", region.storage);
    emit_field_u32("buffer_external_ram", buffer_external_ram ? 1u : 0u);
    emit_field_u32("buffer_writable", buffer_writable ? 1u : 0u);
    emit_field_u32("buffer_mod_alignment", static_cast<uint32_t>(buffer_addr % ring_alignment));
    emit_field_u32("region_header_size", (uint32_t)NVLOG_REGION_HEADER_SIZE);
    emit_field_u32("record_overhead", (uint32_t)NVLOG_RECORD_OVERHEAD);
    emit_field_u32("max_payload", (uint32_t)NVLOG_MAX_PAYLOAD);
    emit_field_u32("ring_reserve_bytes", ring_reserve_bytes);
    emit_field_u32("min_ring_region", ring_min_region);
    Serial.println();

    nvlog_status_t ring_rc = nvlog_ring_format(&ring_ctx, &hal, ring_region);
    if (ring_rc != NVLOG_OK) {
      char rc_value_text[16];
      snprintf(rc_value_text, sizeof(rc_value_text), "%d", static_cast<int>(ring_rc));
      snprintf(g_app.last_detail, sizeof(g_app.last_detail), "ring_format_fail rc=%s(%d)", nvlog_status_name(ring_rc), static_cast<int>(ring_rc));
      emit_event("RING_FORMAT_FAIL");
      emit_field("rc_name", nvlog_status_name(ring_rc));
      emit_field("rc_value", rc_value_text);
      emit_field("detail", g_app.last_detail);
      Serial.println();
      ok = false;
    } else {
      const uint32_t sizes[] = {8u, 12u, 16u, 24u, 32u, 40u, 48u, 56u, 64u};
      for (uint32_t i = 0u; ok && i < (sizeof(sizes) / sizeof(sizes[0])); ++i) {
        ok = append_record_to(&ring_ctx, 21u, i, sizes[i]);
        if (ok) {
          nvlog_stats_t stats;
          ok = nvlog_stats(&ring_ctx, &stats) == NVLOG_OK &&
               stats.record_count >= 1u;
        }
        delay(20u);
      }
      if (ok) {
        nvlog_ctx_t mounted;
        nvlog_ctx_init(&mounted);
        ok = nvlog_ring_mount(&mounted, &hal, ring_region) == NVLOG_OK;
        if (ok) {
          uint32_t ring_count = nvlog_ring_count(&mounted);
          nvlog_stats_t stats;
          ok = nvlog_stats(&mounted, &stats) == NVLOG_OK && stats.record_count == ring_count && ring_count > 0u;
          if (ok) {
            nvlog_iter_t it;
            nvlog_record_t rec;
            if (nvlog_iter_init(&it, &mounted) == NVLOG_OK) {
              uint32_t prev = UINT32_MAX;
              uint32_t idx = 0u;
              while (ok && idx < ring_count) {
                if (nvlog_iter_next(&it, &rec) != NVLOG_OK) {
                  ok = false;
                  break;
                }
                if (prev != UINT32_MAX && rec.seq != prev + 1u) {
                  ok = false;
                  break;
                }
                prev = rec.seq;
                ++idx;
              }
              if (ok && nvlog_iter_next(&it, &rec) != NVLOG_ERR_NO_DATA) {
                ok = false;
              }
            } else {
              ok = false;
            }
          }
        }
      }
    }
  }

  scratch_free(&region);

  if (!ok) {
    emit_scenario_fail("psram_api_smoke", "PSRAM_API", g_app.last_detail[0] != '\0' ? g_app.last_detail : "psram api smoke failed");
    g_app.state.fail_count++;
    g_app.state.last_result = kFail;
    return false;
  }

  emit_scenario_pass("psram_api_smoke");
  g_app.state.pass_count++;
  g_app.state.last_result = kPass;
  return true;
}

static bool scenario_sd_api_smoke()
{
  emit_scenario_start("sd_api_smoke");
  emit_phase("sd_ready");

  if (!bench_capabilities().sd_ready) {
    emit_scenario_skip("sd_api_smoke", "sd_unavailable");
    g_app.state.skip_count++;
    g_app.state.last_result = kSkip;
    return true;
  }

  if (!bench_sd_test()) {
    emit_scenario_fail("sd_api_smoke", "SD_TEST", "sd test append failed");
    g_app.state.fail_count++;
    g_app.state.last_result = kFail;
    return false;
  }

  emit_phase("sd_append");
  char marker[64];
  snprintf(marker, sizeof(marker), "sd_api boot=%lu iter=%lu",
           static_cast<unsigned long>(g_app.state.boot_count),
           static_cast<unsigned long>(g_app.state.iteration));
  if (!bench_sd_append(BENCH_SD_TEST_FILE, marker)) {
    emit_scenario_fail("sd_api_smoke", "SD_APPEND", "append failed");
    g_app.state.fail_count++;
    g_app.state.last_result = kFail;
    return false;
  }

  emit_phase("sd_read");
  File file = SD_MMC.open(BENCH_SD_TEST_FILE, FILE_READ);
  if (!file) {
    emit_scenario_fail("sd_api_smoke", "SD_READ", "open failed");
    g_app.state.fail_count++;
    g_app.state.last_result = kFail;
    return false;
  }

  const size_t file_size = file.size();
  const size_t tail_offset = file_size > 256u ? (file_size - 256u) : 0u;
  if (!file.seek(tail_offset)) {
    file.close();
    emit_scenario_fail("sd_api_smoke", "SD_SEEK", "seek failed");
    g_app.state.fail_count++;
    g_app.state.last_result = kFail;
    return false;
  }

  char tail[257];
  const size_t bytes_read = file.readBytes(tail, sizeof(tail) - 1u);
  tail[bytes_read] = '\0';
  file.close();

  if (strstr(tail, marker) == nullptr) {
    emit_scenario_fail("sd_api_smoke", "SD_VERIFY", "marker missing from tail");
    g_app.state.fail_count++;
    g_app.state.last_result = kFail;
    return false;
  }

  snprintf(g_app.last_detail, sizeof(g_app.last_detail), "size=%lu tail_ok",
           static_cast<unsigned long>(file_size));
  emit_scenario_pass("sd_api_smoke");
  g_app.state.pass_count++;
  g_app.state.last_result = kPass;
  return true;
}

static bool scenario_endurance()
{
  emit_scenario_start("endurance_smoke");
  if (!reset_flash_log()) {
    emit_scenario_fail("endurance_smoke", "FORMAT", "flash reset failed");
    g_app.state.fail_count++;
    g_app.state.last_result = kFail;
    return false;
  }
  const uint32_t limit = g_app.state.endurance_target == 0u ? 8u : g_app.state.endurance_target;
  for (uint32_t i = 0u; i < limit; ++i) {
    char phase[32];
    snprintf(phase, sizeof(phase), "iter_%lu", static_cast<unsigned long>(i));
    emit_phase(phase);
    if (!append_record(9u, i, 24u)) {
      emit_scenario_fail("endurance_smoke", "APPEND", "endurance append failed");
      g_app.state.fail_count++;
      g_app.state.last_result = kFail;
      return false;
    }
  }
  emit_scenario_pass("endurance_smoke");
  g_app.state.pass_count++;
  g_app.state.last_result = kPass;
  return true;
}

static bool run_current_scenario()
{
  switch (g_app.state.scenario_index) {
    case 0: return scenario_empty();
    case 1: return scenario_single_append();
    case 2: return scenario_reboot_recovery();
    case 3: return scenario_ordering();
    case 4: return scenario_payload_sizes();
    case 5: return scenario_capacity_boundary();
    case 6: return scenario_interrupted_append();
    case 7: return scenario_repeated_reboot();
    case 8: return scenario_disconnect_stress();
    case 9: return scenario_corruption_detection();
    case 10: return scenario_psram_api_smoke();
    case 11: return scenario_sd_api_smoke();
    case 12: return scenario_endurance();
    default: return true;
  }
}

static void maybe_restart()
{
  if (g_app.state.pending_restart == 0u) return;
  if (millis() < g_app.state.restart_at_ms) return;
  g_app.state.pending_restart = 0u;
  mark_dirty();
  save_state();
  ESP.restart();
}

static void maybe_finish()
{
  if (g_app.state.scenario_index < kScenarioCount) return;
  g_app.session_done = true;
  if (g_app.state.fail_count == 0u) {
    emit_line("SESSION_PASS");
    show_final_pass();
  } else {
    emit_event("SESSION_FAIL");
    emit_field_u32("failed", g_app.state.fail_count);
    Serial.println();
    show_final_fail();
  }
}

static bool format_or_mount()
{
  if (g_app.partition == nullptr) return false;
  nvlog_ctx_init(&g_app.log_ctx);
  const uint32_t region_size = g_app.partition->size;
  nvlog_status_t st = nvlog_mount(&g_app.log_ctx, &g_app.flash.base, region_size);
  if (st == NVLOG_OK) return true;
  if (nvlog_flash_format(&g_app.log_ctx, &g_app.flash, region_size) != NVLOG_OK) return false;
  nvlog_ctx_init(&g_app.log_ctx);
  return nvlog_mount(&g_app.log_ctx, &g_app.flash.base, region_size) == NVLOG_OK;
}

static bool reset_flash_log()
{
  if (g_app.partition == nullptr) return false;
  nvlog_ctx_init(&g_app.log_ctx);
  const uint32_t region_size = g_app.partition->size;
  if (nvlog_flash_format(&g_app.log_ctx, &g_app.flash, region_size) != NVLOG_OK) {
    return false;
  }
  nvlog_ctx_init(&g_app.log_ctx);
  return nvlog_mount(&g_app.log_ctx, &g_app.flash.base, region_size) == NVLOG_OK;
}

}  // namespace

bool project_test_setup(char *detail, size_t detail_size)
{
  if (detail != nullptr && detail_size > 0U) {
    detail[0] = '\0';
  }

  g_app.prefs.begin(kNamespace, false);
  load_state();
  g_app.state.boot_count++;
  mark_dirty();
  save_state();

  g_app.fault_mode = g_app.state.fault_mode_enabled != 0u;
  g_app.mode_received = false;
  g_app.ready = false;
  g_app.session_done = false;
  g_app.progress_received = false;
  g_app.progress_cycle = 0u;
  g_app.progress_target = 0u;
  disarm_failpoint();
  g_app.progress_label[0] = '\0';
  snprintf(g_app.progress_label, sizeof(g_app.progress_label), "waiting");

  g_app.partition = find_partition();
  prepare_flash();

  if (g_app.partition == nullptr) {
    if (detail != nullptr && detail_size > 0U) {
      snprintf(detail, detail_size, "partition_not_found");
    }
    g_app.state.errors++;
    mark_dirty();
    save_state();
    return false;
  }

  if (!format_or_mount()) {
    if (detail != nullptr && detail_size > 0U) {
      snprintf(detail, detail_size, "nvlog_mount_failed");
    }
    g_app.state.errors++;
    mark_dirty();
    save_state();
    return false;
  }

  emit_boot();
  emit_ready();
  emit_status();
  refresh_display();
  g_app.ready = true;

  if (detail != nullptr && detail_size > 0U) {
    snprintf(detail, detail_size, "ready");
  }
  return true;
}

void project_test_tick()
{
  if (!g_app.ready) return;
  if (!g_app.session_done && g_app.state.pending_restart == 0u && g_app.mode_received) {
    char detail[96];
    (void)project_test_run_once(detail, sizeof(detail));
  }
  refresh_display();
  maybe_restart();
}

bool project_test_run_once(char *detail, size_t detail_size)
{
  if (detail != nullptr && detail_size > 0U) {
    detail[0] = '\0';
  }
  if (!g_app.ready) {
    if (detail != nullptr && detail_size > 0U) {
      snprintf(detail, detail_size, "not_ready");
    }
    return false;
  }
  if (g_app.session_done) {
    if (detail != nullptr && detail_size > 0U) {
      snprintf(detail, detail_size, "session_done");
    }
    return false;
  }
  if (!g_app.mode_received) {
    if (detail != nullptr && detail_size > 0U) {
      snprintf(detail, detail_size, "awaiting_mode");
    }
    return true;
  }

  const bool ok = run_current_scenario();
  if (ok && g_app.state.scenario_index < kScenarioCount &&
      (g_app.state.last_result == kPass || g_app.state.last_result == kSkip)) {
    g_app.state.scenario_index++;
    g_app.state.phase_index = 0u;
    mark_dirty();
    save_state();
  }

  maybe_finish();
  refresh_display();

  if (detail != nullptr && detail_size > 0U) {
    if (!g_app.mode_received) {
      snprintf(detail, detail_size, "awaiting_mode");
    } else {
      snprintf(detail, detail_size, g_app.session_done ? "session_done" : "running");
    }
  }
  return ok;
}

bool project_test_handle_command(const char *command, Print &output)
{
  if (command == nullptr) return false;
  if (strncmp(command, "SESSION_MODE ", 13) == 0) {
    set_mode_from_command(command + 13);
    output.println("NVLOG|ACK|command=SESSION_MODE");
    return true;
  }
  if (strncmp(command, "SESSION_PROGRESS ", 17) == 0) {
    set_progress_from_command(command + 17);
    output.println("NVLOG|ACK|command=SESSION_PROGRESS");
    return true;
  }
  if (strcmp(command, "REBOOT") == 0) {
    output.println("NVLOG|ACK|command=REBOOT");
    output.flush();
    delay(100);
    ESP.restart();
    return true;
  }
  if (strcmp(command, "BOOTLOADER") == 0) {
    output.println("NVLOG|ACK|command=BOOTLOADER");
    output.flush();
    enter_bootloader_mode();
    delay(100);
    ESP.restart();
    return true;
  }
  if (strcmp(command, "NVLOG_STATUS") == 0) {
    emit_status();
    return true;
  }
  return false;
}
