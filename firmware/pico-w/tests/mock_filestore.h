#ifndef MOCK_FILESTORE_H
#define MOCK_FILESTORE_H

#include <stdint.h>
#include <stddef.h>

// Add an entry to the RAM-backed filesystem. Call before each test.
void mock_fs_add_entry(const char *name, const uint8_t *data, size_t len);

// Clear all entries (call before each test).
void mock_fs_clear(void);

#endif // MOCK_FILESTORE_H
