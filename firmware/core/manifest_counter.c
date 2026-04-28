#include "core/manifest_counter.h"
#include "core/filestore.h"

#include <stdio.h>

#define COUNTER_BLOB_NAME "manifest_version.bin"
#define COUNTER_BLOB_SIZE 1

static uint8_t g_counter = 0;

void manifest_counter_init(void) {
    // Read into a buffer one byte larger than expected so we can detect
    // an over-sized (corrupt) blob. fs_system_read returns the actual
    // bytes read up to the buffer size; if the persisted file is the
    // expected single byte we get back exactly 1, otherwise we get
    // either 0 (missing), -1 (error), or 2 (corrupt — file is bigger
    // than we ever wrote).
    uint8_t buf[COUNTER_BLOB_SIZE + 1] = { 0, 0 };
    int n = fs_system_read(COUNTER_BLOB_NAME, buf, sizeof(buf));
    g_counter = (n == COUNTER_BLOB_SIZE) ? buf[0] : 0;
}

uint8_t manifest_counter_bump(void) {
    g_counter = (uint8_t)(g_counter + 1);
    if (fs_system_write(COUNTER_BLOB_NAME, &g_counter, sizeof(g_counter)) != 0) {
        printf("[manifest] WARN: persist failed; counter is RAM-only "
               "until reboot (value=%u)\r\n", (unsigned)g_counter);
    }
    return g_counter;
}

uint8_t manifest_counter_get(void) {
    return g_counter;
}
