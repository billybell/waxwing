#ifndef MOCK_FILESTORE_H
#define MOCK_FILESTORE_H

#include <stdint.h>
#include <stddef.h>

// Add an entry to the RAM-backed user filesystem. Call before each test.
void mock_fs_add_entry(const char *name, const uint8_t *data, size_t len);

// Add an entry to the RAM-backed system filesystem (only reachable via the
// fs_system_* API; never visible to fs_list / fs_read / cmd_ls / cmd_read).
void mock_fs_add_system_entry(const char *name, const uint8_t *data, size_t len);

// Clear all entries (user + system). Call before each test.
void mock_fs_clear(void);

#endif // MOCK_FILESTORE_H
