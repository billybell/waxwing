#include "cborencode.h"
#include <string.h>

// ---------------------------------------------------------------------------
// Generic header encoding: major type in the top 3 bits, length argument
// encoded according to RFC 8949 §3.
// ---------------------------------------------------------------------------

static size_t encode_header(uint8_t *buf, uint8_t major, uint32_t arg) {
    uint8_t mt = (uint8_t)(major << 5);
    if (arg <= 23) {
        buf[0] = mt | (uint8_t)arg;
        return 1;
    }
    if (arg <= 0xFF) {
        buf[0] = mt | 24;
        buf[1] = (uint8_t)arg;
        return 2;
    }
    if (arg <= 0xFFFF) {
        buf[0] = mt | 25;
        buf[1] = (uint8_t)(arg >> 8);
        buf[2] = (uint8_t)arg;
        return 3;
    }
    buf[0] = mt | 26;
    buf[1] = (uint8_t)(arg >> 24);
    buf[2] = (uint8_t)(arg >> 16);
    buf[3] = (uint8_t)(arg >> 8);
    buf[4] = (uint8_t)arg;
    return 5;
}

size_t cborencode_uint(uint8_t *buf, uint32_t value) {
    return encode_header(buf, 0, value);
}

size_t cborencode_int(uint8_t *buf, int32_t value) {
    if (value >= 0) return encode_header(buf, 0, (uint32_t)value);
    uint32_t n = (uint32_t)(-(value + 1));
    return encode_header(buf, 1, n);
}

size_t cborencode_text_str(uint8_t *buf, const char *str, size_t len) {
    size_t hdr = encode_header(buf, 3, (uint32_t)len);
    memcpy(buf + hdr, str, len);
    return hdr + len;
}

size_t cborencode_byte_str(uint8_t *buf, const uint8_t *data, size_t len) {
    size_t hdr = encode_header(buf, 2, (uint32_t)len);
    memcpy(buf + hdr, data, len);
    return hdr + len;
}

size_t cborencode_bool(uint8_t *buf, int value) {
    buf[0] = value ? 0xF5 : 0xF4;
    return 1;
}

size_t cborencode_array_header(uint8_t *buf, uint32_t len) {
    return encode_header(buf, 4, len);
}

size_t cborencode_map_header(uint8_t *buf, uint32_t len) {
    return encode_header(buf, 5, len);
}
