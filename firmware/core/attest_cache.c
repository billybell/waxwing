#include "core/attest_cache.h"

#include <stdio.h>
#include <string.h>

#include "core/filestore.h"

#define HEADER_BYTES   16
#define BUFFER_BYTES   (HEADER_BYTES + ATTEST_CACHE_CAP * ATTEST_CACHE_SLOT_BYTES)
#define BLOB_NAME      "attest_cache.bin"

static uint8_t g_buf[BUFFER_BYTES];
static bool    g_initialized = false;

static uint8_t       *header_ptr(void)        { return g_buf; }
static uint8_t       *slot_ptr(uint8_t i)     { return g_buf + HEADER_BYTES + (size_t)i * ATTEST_CACHE_SLOT_BYTES; }
static uint16_t       slot_record_len(uint8_t i) {
    const uint8_t *p = slot_ptr(i);
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}
static const uint8_t *slot_record_ptr(uint8_t i) { return slot_ptr(i) + 2; }

static uint8_t  next_slot_get(void)       { return header_ptr()[6]; }
static void     next_slot_set(uint8_t v)  { header_ptr()[6] = v; }
static uint8_t  used_count_get(void)      { return header_ptr()[7]; }
static void     used_count_set(uint8_t v) { header_ptr()[7] = v; }

static void blank_buffer(void) {
    memset(g_buf, 0, sizeof(g_buf));
    uint8_t *h = header_ptr();
    h[0] = ATTEST_CACHE_MAGIC0;
    h[1] = ATTEST_CACHE_MAGIC1;
    h[2] = ATTEST_CACHE_MAGIC2;
    h[3] = ATTEST_CACHE_MAGIC3;
    h[4] = ATTEST_CACHE_VERSION;
}

static bool header_valid(const uint8_t *h) {
    return h[0] == ATTEST_CACHE_MAGIC0 && h[1] == ATTEST_CACHE_MAGIC1 &&
           h[2] == ATTEST_CACHE_MAGIC2 && h[3] == ATTEST_CACHE_MAGIC3 &&
           h[4] == ATTEST_CACHE_VERSION &&
           h[6] < ATTEST_CACHE_CAP && h[7] <= ATTEST_CACHE_CAP;
}

void attest_cache_reset(void) {
    blank_buffer();
    g_initialized = true;
}

void attest_cache_init(void) {
    int n = fs_system_read(BLOB_NAME, g_buf, sizeof(g_buf));
    if (n != (int)sizeof(g_buf) || !header_valid(header_ptr())) {
        blank_buffer();
    }
    g_initialized = true;
}

static bool blob_already_present(const uint8_t *blob, size_t len) {
    uint8_t uc = used_count_get();
    if (uc == 0) return false;
    uint8_t start = (uc < ATTEST_CACHE_CAP) ? 0 : next_slot_get();
    for (uint8_t k = 0; k < uc; k++) {
        uint8_t i = (uint8_t)((start + k) % ATTEST_CACHE_CAP);
        uint16_t slen = slot_record_len(i);
        if (slen == len && memcmp(slot_record_ptr(i), blob, len) == 0) {
            return true;
        }
    }
    return false;
}

int attest_cache_write(const uint8_t *blob, size_t len) {
    if (!g_initialized) return -1;
    if (!blob || len == 0) return -1;
    if (len > ATTEST_CACHE_SLOT_BYTES - 2) return -1;

    if (blob_already_present(blob, len)) return 0;

    uint8_t  ns       = next_slot_get();
    uint8_t *slot     = slot_ptr(ns);
    uint8_t *blob_ptr = slot + 2;

    slot[0] = (uint8_t)(len & 0xFF);
    slot[1] = (uint8_t)((len >> 8) & 0xFF);
    memcpy(blob_ptr, blob, len);
    if (len < (size_t)(ATTEST_CACHE_SLOT_BYTES - 2)) {
        memset(blob_ptr + len, 0, ATTEST_CACHE_SLOT_BYTES - 2 - len);
    }

    next_slot_set((uint8_t)((ns + 1) % ATTEST_CACHE_CAP));
    uint8_t uc = used_count_get();
    if (uc < ATTEST_CACHE_CAP) used_count_set(uc + 1);

    if (fs_system_write(BLOB_NAME, g_buf, sizeof(g_buf)) != 0) {
        printf("[attest_cache] fs_system_write failed\r\n");
        return -1;
    }
    return 1;
}

int attest_cache_count(void) {
    if (!g_initialized) return 0;
    return (int)used_count_get();
}

int attest_cache_for_each(void *ctx, attest_cache_iter_cb cb) {
    if (!g_initialized || !cb) return 0;
    uint8_t uc = used_count_get();
    if (uc == 0) return 0;

    uint8_t start = (uc < ATTEST_CACHE_CAP) ? 0 : next_slot_get();
    int seen = 0;
    for (uint8_t k = 0; k < uc; k++) {
        uint8_t i = (uint8_t)((start + k) % ATTEST_CACHE_CAP);
        uint16_t len = slot_record_len(i);
        if (len == 0 || len > ATTEST_CACHE_SLOT_BYTES - 2) continue;
        int rc = cb(ctx, slot_record_ptr(i), len);
        seen++;
        if (rc != 0) break;
    }
    return seen;
}
