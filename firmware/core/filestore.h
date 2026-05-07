#ifndef WAXWING_FILESTORE_H
#define WAXWING_FILESTORE_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Max length for file names used in API calls (excluding /files/ prefix). */
#define FS_MAX_NAME_LEN 32

/**
 * Initialise the filesystem.  Creates /files/ directory and formats an
 * unformatted volume on first boot.
 * Returns 0 on success, -1 on error.
 */
int fs_init(void);

/* ---------------------------------------------------------------------------
 * Queries
 * --------------------------------------------------------------------------- */

/**
 * List files in the /files/ directory, sorted alphabetically (excludes .meta sidecars).
 * out_names: array of char[FS_MAX_NAME_LEN] buffers for file names.
 * out_sizes: parallel array of file sizes.
 * out_hash:  parallel array of 8-byte truncated SHA-256 digests.
 * out_max:   maximum number of entries to fill.
 * offset:    skip this many leading entries (for pagination).
 * limit:     maximum number of entries to return.
 * next_offset: if non-NULL, set to offset+limit if more entries remain.
 * Returns actual number of entries written.
 */
int fs_list(char (*out_names)[FS_MAX_NAME_LEN], uint32_t *out_sizes,
            uint8_t (*out_hash)[8], int out_max, int offset, int limit,
            int *next_offset);

/** Get file size (bytes), or -1 if not found. */
int fs_file_size(const char *name);

/** Get truncated SHA-256 digest of a file, or -1 if error. */
int fs_get_hash(const char *name, uint8_t out_hash[8]);

/* ---------------------------------------------------------------------------
 * Single-shot reads / writes
 * --------------------------------------------------------------------------- */

/**
 * Read entire contents of a file into buffer.
 * Returns bytes read, -1 if not found or error.
 */
int fs_read(const char *name, uint8_t *buf, size_t buf_size);

/**
 * Single-shot write (text or binary).  Max ~2 KB per call.
 * Returns 0 on success, -1 on error (space, name too long, etc.).
 */
int fs_write(const char *name, const uint8_t *data, size_t len);

/* ---------------------------------------------------------------------------
 * Chunked write (images / large files, up to 512 KB)
 * --------------------------------------------------------------------------- */

/**
 * Begin a chunked write.
 * Returns 0 on success, -1 if error or another chunked write already in progress.
 */
int fs_chunked_start(const char *name, uint32_t total_size);

/**
 * Append data to the current chunked write.
 * Validated against declared total_size.
 * Returns bytes appended, -1 on error.
 */
int fs_chunked_append(const uint8_t *data, size_t len);

/**
 * Finalise the chunked write — close and sync the file.
 * On success returns bytes written; on mismatch removes partial file and returns -1.
 */
int fs_chunked_finish(const char *name);

/**
 * Abort and discard the current chunked write.
 */
void fs_chunked_abort(const char *name);

/**
 * Get the name of the file currently being written via chunked API,
 * or NULL if no chunked write is in progress.
 */
const char *fs_chunked_in_progress(void);

/* ---------------------------------------------------------------------------
 * Chunked read (images / large files)
 * --------------------------------------------------------------------------- */

/**
 * Begin a chunked read — return file size in bytes.
 * Returns -1 if not found or error.
 */
int fs_read_start(const char *name);

/**
 * Read `size` bytes starting at `offset` from the named file.
 * Copied into buf; returns bytes actually read, -1 on error.
 */
int fs_read_chunk(const char *name, uint32_t offset, uint32_t size,
                  uint8_t *buf, size_t buf_size);

/* ---------------------------------------------------------------------------
 * Delete
 * --------------------------------------------------------------------------- */

/**
 * Delete a file and its .meta sidecar.
 * Returns 0 on success, -1 if not found or error.
 */
int fs_delete(const char *name);

/* ---------------------------------------------------------------------------
 * Storage info
 * --------------------------------------------------------------------------- */

/**
 * Get storage statistics.
 * free_out:   bytes free on the volume (avail * frsize).
 * used_out:   bytes used by files (.meta excluded from count but included in used).
 * reserve_out: reserved bytes kept free (32 KB).
 * count_out:  number of non-meta files.
 */
void fs_storage_info(uint32_t *free_out, uint32_t *used_out,
                     uint32_t *reserve_out, uint32_t *count_out);

/* ---------------------------------------------------------------------------
 * Metadata sidecars
 * --------------------------------------------------------------------------- */

/** Write a metadata sidecar for the given filename. */
int fs_write_meta(const char *name, const uint8_t *data, size_t len);

/** Read a metadata sidecar. Returns bytes read, -1 if not found. */
int fs_read_meta(const char *name, uint8_t *buf, size_t buf_size);

/* ---------------------------------------------------------------------------
 * System blobs
 *
 * Persistent files used by the firmware itself: transport identity, future
 * encounter records, signed attestations, etc. They live in a separate
 * directory (/system/) and are NEVER reachable from the BLE file-command
 * surface — fs_list, fs_read, fs_write, fs_delete only see /files/. Any
 * exposure of system data to peers must go through a deliberate API.
 * --------------------------------------------------------------------------- */

/** Read a system blob. Returns bytes read, -1 if not found or error. */
int fs_system_read(const char *name, uint8_t *buf, size_t buf_size);

/**
 * Write a system blob (creates or overwrites). Bypasses the user-files
 * single-shot size limit so identity/attestation blobs aren't artificially
 * capped, but still subject to free-space checks.
 * Returns 0 on success, -1 on error.
 */
int fs_system_write(const char *name, const uint8_t *data, size_t len);

/** Delete a system blob. Returns 0 on success, -1 if not found or error. */
int fs_system_delete(const char *name);

#ifdef __cplusplus
}
#endif

#endif // WAXWING_FILESTORE_H
