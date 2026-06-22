#pragma once

#include <Arduino.h>
#include <stddef.h>
#include <stdint.h>

struct BenchCapabilities {
  bool display_ready;
  bool sd_ready;
  bool psram_ready;
  size_t psram_bytes;
};

void bench_hal_begin();
const BenchCapabilities &bench_capabilities();

void bench_emit_event(const char *event_name);
void bench_emit_field(const char *key, const char *value);
void bench_emit_field_u32(const char *key, uint32_t value);
void bench_emit_field_size(const char *key, size_t value);
void bench_emit_end();
void bench_emit_message(const char *event_name, const char *key, const char *value);

bool bench_display_clear();
bool bench_display_write_line(uint8_t line, const char *text);
bool bench_display_test();

bool bench_sd_append(const char *path, const char *text);
bool bench_sd_dump(const char *path, Print &output);
bool bench_sd_test();

bool bench_psram_test(char *detail, size_t detail_size);
