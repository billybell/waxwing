#ifndef WAXWING_CBOR_DECODE_H
#define WAXWING_CBOR_DECODE_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// CBOR major types we understand.
typedef enum {
    CBOR_TYPE_UINT  = 0,
    CBOR_TYPE_INT   = 1,
    CBOR_TYPE_BSTR  = 2,
    CBOR_TYPE_TSTR  = 3,
    CBOR_TYPE_ARRAY = 4,
    CBOR_TYPE_MAP   = 5,
    CBOR_TYPE_BOOL  = 7,    // also covers float/simple values (major type 7).
    CBOR_TYPE_FLOAT = 8,    // synthetic: half/single/double float (major 7, AI 25/26/27).
    CBOR_TYPE_INVALID = 0xFF,
} cbor_type_t;

// One parsed CBOR item.
typedef struct {
    cbor_type_t    type;
    uint64_t       arg;     // int value (UINT/INT/BOOL) or length (BSTR/TSTR/ARRAY/MAP)
    const uint8_t *data;    // BSTR/TSTR: payload start. ARRAY/MAP: first child.
    const uint8_t *start;   // pointer to header byte
    const uint8_t *next;    // pointer past the entire item
} cbor_item_t;

// Parse the item starting at `buf`. Returns true on success and fills *out.
// `end` is one past the last valid byte.
bool cbor_parse(const uint8_t *buf, const uint8_t *end, cbor_item_t *out);

// Find the value associated with a text-string key inside a map. `map_data`
// must point at the first key/value pair (i.e. just past the map header) and
// `count` is the number of key/value pairs. Returns true on hit.
bool cbor_map_find(const uint8_t *map_data, const uint8_t *end,
                   uint64_t pair_count, const char *key,
                   cbor_item_t *out_value);

// Convenience helpers — call cbor_map_find under the hood and unpack one
// expected type. Return false if missing or wrong type.
bool cbor_map_get_uint(const uint8_t *map_data, const uint8_t *end,
                      uint64_t pair_count, const char *key, uint64_t *out_val);

bool cbor_map_get_text(const uint8_t *map_data, const uint8_t *end,
                       uint64_t pair_count, const char *key,
                       char *out_buf, size_t out_buf_size, size_t *out_len);

bool cbor_map_get_bytes(const uint8_t *map_data, const uint8_t *end,
                        uint64_t pair_count, const char *key,
                        const uint8_t **out_ptr, size_t *out_len);

#ifdef __cplusplus
}
#endif

#endif // WAXWING_CBOR_DECODE_H
