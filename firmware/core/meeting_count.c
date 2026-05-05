#include "core/meeting_count.h"
#include "core/filestore.h"

#include <stdio.h>
#include <string.h>

#define COUNTER_BLOB_NAME "meeting_count.bin"
#define COUNTER_BLOB_SIZE 8

static uint64_t g_counter = 0;

static void encode_le64(uint64_t v, uint8_t out[8]) {
    for (int i = 0; i < 8; i++) {
        out[i] = (uint8_t)(v >> (i * 8));
    }
}

static uint64_t decode_le64(const uint8_t in[8]) {
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) {
        v |= ((uint64_t)in[i]) << (i * 8);
    }
    return v;
}

void meeting_count_init(void) {
    // Read into a buffer one byte larger than expected so we can detect
    // an over-sized (corrupt) blob — see manifest_counter for the same
    // pattern. Anything other than exactly 8 bytes resets to zero.
    uint8_t buf[COUNTER_BLOB_SIZE + 1];
    memset(buf, 0, sizeof(buf));
    int n = fs_system_read(COUNTER_BLOB_NAME, buf, sizeof(buf));
    g_counter = (n == COUNTER_BLOB_SIZE) ? decode_le64(buf) : 0;
}

void meeting_count_reset(void) {
    g_counter = 0;
}

uint64_t meeting_count_bump(void) {
    // Saturate at UINT64_MAX. Reaching it requires ~5×10^11 encounters
    // per second for the lifetime of the universe, but the explicit
    // guard costs nothing and avoids the wrap-to-zero footgun.
    if (g_counter < UINT64_MAX) g_counter++;

    uint8_t buf[COUNTER_BLOB_SIZE];
    encode_le64(g_counter, buf);
    if (fs_system_write(COUNTER_BLOB_NAME, buf, sizeof(buf)) != 0) {
        printf("[meeting_count] WARN: persist failed; counter is RAM-only "
               "until reboot (value=%llu)\r\n", (unsigned long long)g_counter);
    }
    return g_counter;
}

uint64_t meeting_count_get(void) {
    return g_counter;
}
