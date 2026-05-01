#ifndef WAXWING_CBORENCODE_H
#define WAXWING_CBORENCODE_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// All encode functions write into `buf` and return the number of bytes
// written. They never check buffer size — caller must guarantee enough
// room (CBOR header is at most 5 bytes for the encodings we produce).

size_t cborencode_uint(uint8_t *buf, uint32_t value);
size_t cborencode_uint64(uint8_t *buf, uint64_t value);
size_t cborencode_int(uint8_t *buf, int32_t value);
size_t cborencode_text_str(uint8_t *buf, const char *str, size_t len);
size_t cborencode_byte_str(uint8_t *buf, const uint8_t *data, size_t len);
size_t cborencode_bool(uint8_t *buf, int value);
size_t cborencode_array_header(uint8_t *buf, uint32_t len);
size_t cborencode_map_header(uint8_t *buf, uint32_t len);

#ifdef __cplusplus
}
#endif

#endif // WAXWING_CBORENCODE_H
