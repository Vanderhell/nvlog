#ifndef NVLOG_WIRE_H
#define NVLOG_WIRE_H

#include <stdint.h>

static inline void nvlog_store_u16le(uint8_t *dst, uint16_t value)
{
    dst[0] = (uint8_t)(value & 0xFFu);
    dst[1] = (uint8_t)((value >> 8) & 0xFFu);
}

static inline void nvlog_store_u32le(uint8_t *dst, uint32_t value)
{
    dst[0] = (uint8_t)(value & 0xFFu);
    dst[1] = (uint8_t)((value >> 8) & 0xFFu);
    dst[2] = (uint8_t)((value >> 16) & 0xFFu);
    dst[3] = (uint8_t)((value >> 24) & 0xFFu);
}

static inline uint16_t nvlog_load_u16le(const uint8_t *src)
{
    return (uint16_t)((uint16_t)src[0] | ((uint16_t)src[1] << 8));
}

static inline uint32_t nvlog_load_u32le(const uint8_t *src)
{
    return (uint32_t)src[0]
         | ((uint32_t)src[1] << 8)
         | ((uint32_t)src[2] << 16)
         | ((uint32_t)src[3] << 24);
}

static inline int nvlog_u32_add_checked(uint32_t a, uint32_t b, uint32_t *out)
{
    if (!out || UINT32_MAX - a < b) return 0;
    *out = a + b;
    return 1;
}

static inline int nvlog_u32_sub_checked(uint32_t a, uint32_t b, uint32_t *out)
{
    if (!out || a < b) return 0;
    *out = a - b;
    return 1;
}

static inline uint32_t nvlog_crc32_step(uint32_t crc, uint8_t byte)
{
    crc ^= byte;
    for (int i = 0; i < 8; i++) {
        uint32_t mask = 0u - (crc & 1u);
        crc = (crc >> 1) ^ (0xEDB88320u & mask);
    }
    return crc;
}

static inline uint32_t nvlog_crc32_bytes(const uint8_t *data, uint32_t len)
{
    uint32_t crc = 0xFFFFFFFFu;
    for (uint32_t i = 0; i < len; i++)
        crc = nvlog_crc32_step(crc, data[i]);
    return ~crc;
}

#endif /* NVLOG_WIRE_H */
