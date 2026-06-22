#pragma once

// ---------- Serial protocol ----------
#define BENCH_SERIAL_BAUD 115200
#define BENCH_SERIAL_COMMAND_BUFFER_SIZE 192
#define BENCH_HEARTBEAT_INTERVAL_MS 1000UL

// ---------- Shared SPI bus ----------
// Display pins are taken from the supplied working example.
#define BENCH_SPI_SCLK 10
#define BENCH_SPI_MOSI 11

// Set the actual TF-card MISO pin before enabling the card.
#define BENCH_SPI_MISO -1

// ---------- ST7735 0.96" 160x80 display ----------
#define BENCH_DISPLAY_ENABLED 1
#define BENCH_DISPLAY_CS 12
#define BENCH_DISPLAY_DC 13
#define BENCH_DISPLAY_RST 14
#define BENCH_DISPLAY_BACKLIGHT -1
#define BENCH_DISPLAY_ROTATION 1

// ---------- TF / microSD card via SDMMC ----------
#define BENCH_SD_ENABLED 1
#define BENCH_SDMMC_MODE_1BIT 1
#define BENCH_SDMMC_PIN_CLK 17
#define BENCH_SDMMC_PIN_CMD 18
#define BENCH_SDMMC_PIN_D0 16
#define BENCH_SDMMC_PIN_D3 47
#define BENCH_SD_FREQUENCY_HZ 20000000UL
#define BENCH_SD_TEST_DIRECTORY "/loxbench"
#define BENCH_SD_TEST_FILE "/loxbench/session.log"

// ---------- PSRAM ----------
#define BENCH_PSRAM_ENABLED 1
#define BENCH_PSRAM_TEST_BYTES (64U * 1024U)

// ---------- Project adapter ----------
#define BENCH_RUN_PROJECT_TEST_ON_BOOT 0
