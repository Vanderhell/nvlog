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

#endif /* NVLOG_WIRE_H */
