#include "mock_filestore.h"
#include "core/filestore.h"

#include <string.h>
#include <stdio.h>

#define MAX_FILES 64

static char file_names[MAX_FILES][FS_MAX_NAME_LEN];
static uint8_t file_data[MAX_FILES][1024]; // max inline data cap from commands.c
static size_t file_lens[MAX_FILES];
static uint8_t meta_data[MAX_FILES][256]; // sidecar meta storage
static size_t meta_lens[MAX_FILES];
static int file_count = 0;

// Chunked write state + in-memory accumulator for data being streamed via chunks
static char chunk_name[FS_MAX_NAME_LEN];
static size_t chunk_expected = 0;
static int chunk_active = 0;
static size_t chunk_pos = 0;
static uint8_t chunk_buf[512 * 1024]; // matches MAX_CHUNKED_SIZE from real impl

// Simple hash: sum all bytes modulo 2^64 (not crypto, just fills the wire format)
static void simple_hash(const uint8_t *data, size_t len, uint8_t out[8]) {
    uint64_t h = 0;
    for (size_t i = 0; i < len; i++) {
        h += data[i] + i + 1;
    }
    memcpy(out, &h, 8);
}

static int find_file(const char *name) {
    for (int i = 0; i < file_count; i++) {
        if (strcmp(file_names[i], name) == 0) return i;
    }
    return -1;
}

static void free_file(int idx) {
    if (idx < 0 || idx >= file_count) return;
    int tail = file_count - 1 - idx;
    memmove(&file_names[idx], &file_names[idx + 1], tail * FS_MAX_NAME_LEN);
    memmove(&file_data[idx], &file_data[idx + 1], tail * sizeof(file_data[0]));
    memmove(&meta_data[idx], &meta_data[idx + 1], tail * sizeof(meta_data[0]));
    memmove(&file_lens[idx], &file_lens[idx + 1], tail * sizeof(file_lens[0]));
    memmove(&meta_lens[idx], &meta_lens[idx + 1], tail * sizeof(meta_lens[0]));
    file_count--;
}

static void sort_files(void) {
    // Insertion sort over five parallel arrays (names/data/lens + meta/meta_lens).
    // All five must move in lockstep — earlier versions only sorted file_*,
    // which left meta sidecars associated with the wrong file after sort.
    for (int i = 1; i < file_count; i++) {
        char tmp_n[FS_MAX_NAME_LEN];
        uint8_t tmp_d[sizeof(file_data[0])];
        uint8_t tmp_m[sizeof(meta_data[0])];
        size_t tmp_dl, tmp_ml;
        memcpy(tmp_n, file_names[i], FS_MAX_NAME_LEN);
        memcpy(tmp_d, file_data[i], sizeof(file_data[0]));
        memcpy(tmp_m, meta_data[i], sizeof(meta_data[0]));
        tmp_dl = file_lens[i];
        tmp_ml = meta_lens[i];
        int j = i;
        while (j > 0 && strcmp(file_names[j - 1], tmp_n) > 0) {
            memcpy(file_names[j], file_names[j - 1], FS_MAX_NAME_LEN);
            memcpy(file_data[j], file_data[j - 1], sizeof(file_data[0]));
            memcpy(meta_data[j], meta_data[j - 1], sizeof(meta_data[0]));
            file_lens[j] = file_lens[j - 1];
            meta_lens[j] = meta_lens[j - 1];
            j--;
        }
        memcpy(file_names[j], tmp_n, FS_MAX_NAME_LEN);
        memcpy(file_data[j], tmp_d, sizeof(file_data[0]));
        memcpy(meta_data[j], tmp_m, sizeof(meta_data[0]));
        file_lens[j] = tmp_dl;
        meta_lens[j] = tmp_ml;
    }
}

void mock_fs_add_entry(const char *name, const uint8_t *data, size_t len) {
    if (file_count >= MAX_FILES) return;
    strncpy(file_names[file_count], name, FS_MAX_NAME_LEN - 1);
    file_names[file_count][FS_MAX_NAME_LEN - 1] = '\0';
    memcpy(file_data[file_count], data, len);
    file_lens[file_count] = len;
    meta_lens[file_count] = 0;
    file_count++;
    sort_files();
}

void mock_fs_clear(void) {
    file_count = 0;
    memset(chunk_buf, 0, sizeof(chunk_buf));
    chunk_active = 0;
    memset(meta_data, 0, sizeof(meta_data));
    memset(meta_lens, 0, sizeof(meta_lens));
}

int fs_init(void) { return 0; }

int fs_list(char (*out_names)[FS_MAX_NAME_LEN], uint32_t *out_sizes,
            uint8_t (*out_hash)[8], int out_max, int offset, int limit,
            int *next_offset) {
    int cap = limit < out_max ? limit : out_max;
    int seen = 0;     // non-meta entries scanned so far
    int written = 0;  // entries copied into out_*
    for (int i = 0; i < file_count; i++) {
        size_t len = strlen(file_names[i]);
        if (len > 5 && strcmp(file_names[i] + len - 5, ".meta") == 0) continue;
        if (seen >= offset && written < cap) {
            memcpy(out_names[written], file_names[i], FS_MAX_NAME_LEN);
            out_sizes[written] = (uint32_t)file_lens[i];
            simple_hash(file_data[i], file_lens[i], out_hash[written]);
            written++;
        }
        seen++;
    }
    if (next_offset) *next_offset = (offset + written < seen) ? offset + written : 0;
    return written;
}

int fs_file_size(const char *name) {
    int idx = find_file(name);
    return idx >= 0 ? (int)file_lens[idx] : -1;
}

int fs_read(const char *name, uint8_t *buf, size_t buf_size) {
    int idx = find_file(name);
    if (idx < 0) return -1;
    size_t n = file_lens[idx] > buf_size ? buf_size : file_lens[idx];
    memcpy(buf, file_data[idx], n);
    return (int)n;
}

int fs_write(const char *name, const uint8_t *data, size_t len) {
    if (len > 1024) return -1; // match MAX_INLINE_DATA from commands.c
    int idx = find_file(name);
    if (idx >= 0) {
        memcpy(file_data[idx], data, len);
        file_lens[idx] = len;
    } else {
        if (file_count >= MAX_FILES) return -1;
        strncpy(file_names[file_count], name, FS_MAX_NAME_LEN - 1);
        file_names[file_count][FS_MAX_NAME_LEN - 1] = '\0';
        memcpy(file_data[file_count], data, len);
        file_lens[file_count] = len;
        meta_lens[file_count] = 0; // fresh slot, no sidecar yet
        file_count++;
        sort_files();
    }
    return 0;
}

int fs_chunked_start(const char *name, uint32_t total_size) {
    if (chunk_active) return -1;
    strncpy(chunk_name, name, FS_MAX_NAME_LEN - 1);
    chunk_name[FS_MAX_NAME_LEN - 1] = '\0';
    chunk_expected = total_size;
    chunk_pos = 0;
    memset(chunk_buf, 0, sizeof(chunk_buf));
    chunk_active = 1;
    return 0;
}

int fs_chunked_append(const uint8_t *data, size_t len) {
    if (!chunk_active) return -1;
    if (chunk_pos + len > chunk_expected) {
        chunk_active = 0;
        return -1;
    }
    memcpy(chunk_buf + chunk_pos, data, len);
    chunk_pos += len;
    return (int)len;
}

int fs_chunked_finish(const char *name) {
    if (!chunk_active) return -1;
    if (strcmp(name, chunk_name) != 0) return -1;
    if (chunk_pos != chunk_expected) {
        memset(&chunk_buf, 0, sizeof(chunk_buf));
        chunk_active = 0;
        return -1;
    }
    int idx = find_file(chunk_name);
    if (idx < 0) {
        // New file — create it
        if (file_count >= MAX_FILES) {
            chunk_active = 0;
            return -1;
        }
        strncpy(file_names[file_count], chunk_name, FS_MAX_NAME_LEN - 1);
        file_names[file_count][FS_MAX_NAME_LEN - 1] = '\0';
        memcpy(file_data[file_count], chunk_buf, chunk_pos);
        file_lens[file_count] = chunk_pos;
        meta_lens[file_count] = 0; // fresh slot, no sidecar yet
        file_count++;
        sort_files();
    } else {
        memcpy(file_data[idx], chunk_buf, chunk_pos);
        file_lens[idx] = chunk_pos;
    }
    memset(&chunk_buf, 0, sizeof(chunk_buf));
    chunk_active = 0;
    return (int)chunk_pos;
}

void fs_chunked_abort(const char *name) {
    if (!chunk_active) return;
    if (name && strcmp(name, chunk_name) != 0) return;
    memset(&chunk_buf, 0, sizeof(chunk_buf));
    chunk_active = 0;
}

const char *fs_chunked_in_progress(void) {
    return chunk_active ? chunk_name : NULL;
}

int fs_read_start(const char *name) {
    int idx = find_file(name);
    return idx >= 0 ? (int)file_lens[idx] : -1;
}

int fs_read_chunk(const char *name, uint32_t offset, uint32_t size,
                  uint8_t *buf, size_t buf_size) {
    int idx = find_file(name);
    if (idx < 0) return -1;
    size_t n = file_lens[idx];
    if (offset >= n) return -1;
    size_t available = n - offset;
    size_t to_read = size < available ? size : available;
    to_read = buf_size < to_read ? buf_size : to_read;
    memcpy(buf, file_data[idx] + offset, to_read);
    return (int)to_read;
}

int fs_delete(const char *name) {
    int idx = find_file(name);
    if (idx < 0) return -1;
    free_file(idx);
    // Also delete .meta sidecar
    char meta_name[FS_MAX_NAME_LEN];
    snprintf(meta_name, sizeof(meta_name), "%s.meta", name);
    int midx = find_file(meta_name);
    if (midx >= 0) free_file(midx);
    return 0;
}

void fs_storage_info(uint32_t *free_out, uint32_t *used_out,
                     uint32_t *reserve_out, uint32_t *count_out) {
    (void)free_out; // not meaningful for RAM mock
    uint32_t used = 0, count = 0;
    for (int i = 0; i < file_count; i++) {
        size_t len = strlen(file_names[i]);
        if (len > 5 && strcmp(file_names[i] + len - 5, ".meta") == 0) {
            used += (uint32_t)file_lens[i]; // .meta counts as used bytes but not files
            continue;
        }
        count++;
        used += (uint32_t)file_lens[i];
    }
    if (free_out) *free_out = 0x10000000; // fake large free space
    if (used_out) *used_out = used;
    if (reserve_out) *reserve_out = 32768;
    if (count_out) *count_out = count;
}

int fs_write_meta(const char *name, const uint8_t *data, size_t len) {
    if (find_file(name) < 0) return -1;
    char meta_name[FS_MAX_NAME_LEN];
    snprintf(meta_name, sizeof(meta_name), "%s.meta", name);
    int midx = find_file(meta_name);
    if (midx < 0) {
        if (file_count >= MAX_FILES) return -1;
        strncpy(file_names[file_count], meta_name, FS_MAX_NAME_LEN - 1);
        file_names[file_count][FS_MAX_NAME_LEN - 1] = '\0';
        meta_lens[file_count] = 0;
        file_lens[file_count] = 0;
        file_count++;
        sort_files();
        // sort_files may have moved the new entry — re-locate it.
        midx = find_file(meta_name);
        if (midx < 0) return -1;
    }
    if (len > sizeof(meta_data[midx])) return -1;
    memcpy(meta_data[midx], data, len);
    meta_lens[midx] = len;
    return 0;
}

int fs_read_meta(const char *name, uint8_t *buf, size_t buf_size) {
    char meta_name[FS_MAX_NAME_LEN];
    snprintf(meta_name, sizeof(meta_name), "%s.meta", name);
    int midx = find_file(meta_name);
    if (midx < 0) return -1;
    size_t n = meta_lens[midx] > buf_size ? buf_size : meta_lens[midx];
    memcpy(buf, meta_data[midx], n);
    return (int)n;
}
