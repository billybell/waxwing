#include "core/filestore.h"
#include "core/hal_crypto.h"

#include <string.h>
#include <stdio.h>

#include "thirdparty/fatfs/ff.h"
#include "diskio.h"

// ---------------------------------------------------------------------------
// Configuration
// ---------------------------------------------------------------------------

#define FILES_DIR_NAME      "/files"
#define SYSTEM_DIR_NAME     "/system"
#define META_SUFFIX         ".meta"
#define HASH_SUFFIX         ".h8"       // 8-byte truncated SHA-256 sidecar
#define MAX_SHOT_FILE_SIZE  2048        // single-shot write limit
#define MAX_CHUNKED_SIZE    (512 * 1024)
#define STORAGE_RESERVE     (32 * 1024)

// ---------------------------------------------------------------------------
// Volume + chunked-write state
// ---------------------------------------------------------------------------

static FATFS fs_obj;
static bool  fs_mounted = false;

typedef struct {
    char     name[FS_MAX_NAME_LEN];
    FIL      fil;
    uint32_t expected;
    uint32_t written;
    bool     active;
} chunk_state_t;

static chunk_state_t chunk_state;

// ---------------------------------------------------------------------------
// Path helpers
// ---------------------------------------------------------------------------

static bool has_suffix(const char *name, const char *suffix) {
    size_t nl = strlen(name);
    size_t sl = strlen(suffix);
    return nl > sl && strcmp(name + nl - sl, suffix) == 0;
}

static bool name_is_meta(const char *name) {
    return has_suffix(name, META_SUFFIX);
}

static bool name_is_hash_sidecar(const char *name) {
    return has_suffix(name, HASH_SUFFIX);
}

// True for any sidecar that should be invisible to listings and to the
// user-file count: .meta and .h8 both qualify.
static bool name_is_sidecar(const char *name) {
    return name_is_meta(name) || name_is_hash_sidecar(name);
}

// Build "/files/<name>" into the supplied buffer. Returns false if the
// name is too long or contains illegal characters (we don't allow paths,
// just bare filenames).
static bool build_full_path(const char *name, char *out, size_t out_size) {
    if (!name || !*name) return false;
    if (strchr(name, '/') != NULL) return false;
    int n = snprintf(out, out_size, FILES_DIR_NAME "/%s", name);
    return n > 0 && (size_t)n < out_size;
}

static bool build_meta_path(const char *name, char *out, size_t out_size) {
    if (!name || !*name) return false;
    if (strchr(name, '/') != NULL) return false;
    int n = snprintf(out, out_size, FILES_DIR_NAME "/%s" META_SUFFIX, name);
    return n > 0 && (size_t)n < out_size;
}

static bool build_hash_path(const char *name, char *out, size_t out_size) {
    if (!name || !*name) return false;
    if (strchr(name, '/') != NULL) return false;
    int n = snprintf(out, out_size, FILES_DIR_NAME "/%s" HASH_SUFFIX, name);
    return n > 0 && (size_t)n < out_size;
}

// ---------------------------------------------------------------------------
// Hash sidecar helpers
//
// Each user file at /files/<name> has an optional /files/<name>.h8 sidecar
// that holds the first 8 bytes of its SHA-256. The sidecar is a pure
// cache: it is invalidated by deletion on every mutation path (single-
// shot write, chunked-write start, file delete) and lazily regenerated
// by fs_get_hash on the next call. Single-shot writes also eagerly
// repopulate it since the data is already in RAM.
// ---------------------------------------------------------------------------

static int read_hash_sidecar(const char *name, uint8_t out_hash[8]) {
    char path[64];
    if (!build_hash_path(name, path, sizeof(path))) return -1;
    FIL fil;
    if (f_open(&fil, path, FA_READ) != FR_OK) return -1;
    UINT br = 0;
    FRESULT res = f_read(&fil, out_hash, 8, &br);
    f_close(&fil);
    return (res == FR_OK && br == 8) ? 0 : -1;
}

// Best-effort write — failures are tolerated because the sidecar is a
// cache. On any failure subsequent fs_get_hash falls back to compute.
static void write_hash_sidecar(const char *name, const uint8_t *hash_8plus) {
    char path[64];
    if (!build_hash_path(name, path, sizeof(path))) return;
    FIL fil;
    if (f_open(&fil, path, FA_WRITE | FA_CREATE_ALWAYS) != FR_OK) return;
    UINT bw = 0;
    f_write(&fil, hash_8plus, 8, &bw);
    f_sync(&fil);
    f_close(&fil);
}

static void delete_hash_sidecar(const char *name) {
    char path[64];
    if (build_hash_path(name, path, sizeof(path))) f_unlink(path);
}

// /system/<name>. Same name-validation rules as user files (no slashes,
// non-empty). System paths are only resolved by fs_system_* — there is no
// command in commands.c that reaches this directory.
static bool build_system_path(const char *name, char *out, size_t out_size) {
    if (!name || !*name) return false;
    if (strchr(name, '/') != NULL) return false;
    int n = snprintf(out, out_size, SYSTEM_DIR_NAME "/%s", name);
    return n > 0 && (size_t)n < out_size;
}

// ---------------------------------------------------------------------------
// Init / mount
// ---------------------------------------------------------------------------

static int mount_and_ensure_dir(void) {
    FRESULT res = f_mount(&fs_obj, "0:", 1);
    if (res == FR_NO_FILESYSTEM) {
        printf("[filestore] No FAT volume; formatting...\r\n");
        BYTE work[FF_MAX_SS];
        MKFS_PARM parm = { .fmt = FM_FAT | FM_SFD, .au_size = 4096 };
        res = f_mkfs("0:", &parm, work, sizeof(work));
        if (res != FR_OK) {
            printf("[filestore] f_mkfs failed: %d\r\n", res);
            return -1;
        }
        res = f_mount(&fs_obj, "0:", 1);
    }
    if (res != FR_OK) {
        printf("[filestore] f_mount failed: %d\r\n", res);
        return -1;
    }

    res = f_stat(FILES_DIR_NAME, NULL);
    if (res == FR_NO_FILE || res == FR_NO_PATH) {
        res = f_mkdir(FILES_DIR_NAME);
        if (res != FR_OK) {
            printf("[filestore] f_mkdir(" FILES_DIR_NAME ") failed: %d\r\n", res);
            return -1;
        }
        printf("[filestore] Created " FILES_DIR_NAME "\r\n");
    } else if (res != FR_OK) {
        printf("[filestore] f_stat(" FILES_DIR_NAME ") failed: %d\r\n", res);
        return -1;
    }

    res = f_stat(SYSTEM_DIR_NAME, NULL);
    if (res == FR_NO_FILE || res == FR_NO_PATH) {
        res = f_mkdir(SYSTEM_DIR_NAME);
        if (res != FR_OK) {
            printf("[filestore] f_mkdir(" SYSTEM_DIR_NAME ") failed: %d\r\n", res);
            return -1;
        }
        printf("[filestore] Created " SYSTEM_DIR_NAME "\r\n");
    } else if (res != FR_OK) {
        printf("[filestore] f_stat(" SYSTEM_DIR_NAME ") failed: %d\r\n", res);
        return -1;
    }
    return 0;
}

int fs_init(void) {
    if (mount_and_ensure_dir() != 0) return -1;
    fs_mounted = true;
    return 0;
}

// ---------------------------------------------------------------------------
// Listing
// ---------------------------------------------------------------------------

int fs_list(char (*out_names)[FS_MAX_NAME_LEN], uint32_t *out_sizes,
            uint8_t (*out_hash)[8], int out_max, int offset, int limit,
            int *next_offset) {
    if (!fs_mounted) return -1;

    DIR dir;
    if (f_opendir(&dir, FILES_DIR_NAME) != FR_OK) return -1;

    // Stage all non-meta entries up to a fixed cap, then sort and page.
    enum { STAGE_MAX = 64 };
    char stage_names[STAGE_MAX][FS_MAX_NAME_LEN];
    uint32_t stage_sizes[STAGE_MAX];
    int total = 0;

    FILINFO finfo;
    while (total < STAGE_MAX) {
        if (f_readdir(&dir, &finfo) != FR_OK) break;
        if (finfo.fname[0] == '\0') break;
        if (finfo.fattrib & AM_DIR) continue;
        if (name_is_sidecar(finfo.fname)) continue;
        strncpy(stage_names[total], finfo.fname, FS_MAX_NAME_LEN - 1);
        stage_names[total][FS_MAX_NAME_LEN - 1] = '\0';
        stage_sizes[total] = (uint32_t)finfo.fsize;
        total++;
    }
    f_closedir(&dir);

    // Insertion sort (alphabetical, ascii). Tiny dataset.
    for (int i = 1; i < total; i++) {
        char tmp_n[FS_MAX_NAME_LEN];
        uint32_t tmp_s;
        memcpy(tmp_n, stage_names[i], FS_MAX_NAME_LEN);
        tmp_s = stage_sizes[i];
        int j = i;
        while (j > 0 && strcmp(stage_names[j - 1], tmp_n) > 0) {
            memcpy(stage_names[j], stage_names[j - 1], FS_MAX_NAME_LEN);
            stage_sizes[j] = stage_sizes[j - 1];
            j--;
        }
        memcpy(stage_names[j], tmp_n, FS_MAX_NAME_LEN);
        stage_sizes[j] = tmp_s;
    }

    int start = offset < total ? offset : total;
    int end = start + limit;
    if (end > total) end = total;
    if (end - start > out_max) end = start + out_max;
    int count = end - start;

    for (int i = 0; i < count; i++) {
        strncpy(out_names[i], stage_names[start + i], FS_MAX_NAME_LEN - 1);
        out_names[i][FS_MAX_NAME_LEN - 1] = '\0';
        out_sizes[i] = stage_sizes[start + i];
        if (fs_get_hash(out_names[i], out_hash[i]) != 0) {
            memset(out_hash[i], 0, 8);
        }
    }

    if (next_offset) *next_offset = end < total ? end : 0;
    return count;
}

int fs_file_size(const char *name) {
    if (!fs_mounted) return -1;
    char path[64];
    if (!build_full_path(name, path, sizeof(path))) return -1;
    FILINFO finfo;
    return f_stat(path, &finfo) == FR_OK ? (int)finfo.fsize : -1;
}

// Stream-hashes the file at `path`. Caller is responsible for path
// validation. Returns 0 on success with the first 8 bytes of the digest
// in `out_hash`.
static int compute_file_hash_8(const char *path, uint8_t out_hash[8]) {
    FIL fil;
    if (f_open(&fil, path, FA_READ) != FR_OK) return -1;

    hal_sha256_ctx_t *s = hal_sha256_init();
    if (!s) { f_close(&fil); return -1; }

    uint8_t buf[256];
    UINT br = 0;
    for (;;) {
        FRESULT res = f_read(&fil, buf, sizeof(buf), &br);
        if (res != FR_OK) {
            hal_sha256_free(s);
            f_close(&fil);
            return -1;
        }
        if (br == 0) break;
        hal_sha256_update(s, buf, br);
        if (br < sizeof(buf)) break;
    }

    uint8_t digest[32];
    hal_sha256_final(s, digest);
    hal_sha256_free(s);
    f_close(&fil);

    memcpy(out_hash, digest, 8);
    return 0;
}

int fs_get_hash(const char *name, uint8_t out_hash[8]) {
    if (!fs_mounted) return -1;

    // Sidecar fast path. Any successful mutation of the file invalidates
    // (deletes) its sidecar, so a present sidecar is trustworthy.
    if (read_hash_sidecar(name, out_hash) == 0) return 0;

    char path[64];
    if (!build_full_path(name, path, sizeof(path))) return -1;
    if (compute_file_hash_8(path, out_hash) != 0) return -1;
    write_hash_sidecar(name, out_hash);
    return 0;
}

// ---------------------------------------------------------------------------
// Single-shot read / write
// ---------------------------------------------------------------------------

int fs_read(const char *name, uint8_t *buf, size_t buf_size) {
    if (!fs_mounted) return -1;
    char path[64];
    if (!build_full_path(name, path, sizeof(path))) return -1;
    FIL fil;
    if (f_open(&fil, path, FA_READ) != FR_OK) return -1;
    UINT br = 0;
    FRESULT res = f_read(&fil, buf, (UINT)buf_size, &br);
    f_close(&fil);
    return (res == FR_OK) ? (int)br : -1;
}

int fs_write(const char *name, const uint8_t *data, size_t len) {
    if (!fs_mounted) return -1;
    if (len > MAX_SHOT_FILE_SIZE) return -1;

    char path[64];
    if (!build_full_path(name, path, sizeof(path))) return -1;

    FIL fil;
    if (f_open(&fil, path, FA_WRITE | FA_CREATE_ALWAYS) != FR_OK) return -1;
    UINT bw = 0;
    FRESULT res = f_write(&fil, data, (UINT)len, &bw);
    f_sync(&fil);
    f_close(&fil);
    if (res != FR_OK || (size_t)bw != len) {
        delete_hash_sidecar(name);
        return -1;
    }

    // Eager sidecar refresh: data is already in RAM, so we can hash it
    // here for free (no extra disk read).
    uint8_t digest[32];
    if (hal_sha256_blob(data, len, digest)) {
        write_hash_sidecar(name, digest);
    } else {
        delete_hash_sidecar(name);
    }

    printf("[filestore] Wrote %s (%zu bytes)\r\n", path, len);
    return 0;
}

// ---------------------------------------------------------------------------
// Chunked write
// ---------------------------------------------------------------------------

int fs_chunked_start(const char *name, uint32_t total_size) {
    if (!fs_mounted) return -1;
    if (total_size == 0 || total_size > MAX_CHUNKED_SIZE) return -1;

    // If a previous chunked write is still active (the client gave up
    // mid-transfer, or a CBOR parse error stranded the state), abandon
    // it before opening the new file. Without this, a single failed
    // upload would lock out all subsequent uploads until reboot.
    if (chunk_state.active) {
        printf("[filestore] Aborting stale chunked write '%s' (%u/%u) for new '%s'\r\n",
               chunk_state.name, (unsigned)chunk_state.written,
               (unsigned)chunk_state.expected, name);
        fs_chunked_abort(NULL);
    }

    char path[64];
    if (!build_full_path(name, path, sizeof(path))) return -1;
    if (f_open(&chunk_state.fil, path, FA_WRITE | FA_CREATE_ALWAYS) != FR_OK) return -1;

    // File was just truncated; any pre-existing hash sidecar is stale.
    // Sidecar is regenerated lazily on the next fs_get_hash call.
    delete_hash_sidecar(name);

    strncpy(chunk_state.name, name, FS_MAX_NAME_LEN - 1);
    chunk_state.name[FS_MAX_NAME_LEN - 1] = '\0';
    chunk_state.expected = total_size;
    chunk_state.written = 0;
    chunk_state.active = true;
    printf("[filestore] Chunked write started: %s (%u bytes)\r\n", name, (unsigned)total_size);
    return 0;
}

int fs_chunked_append(const uint8_t *data, size_t len) {
    if (!chunk_state.active) return -1;
    if (chunk_state.written + len > chunk_state.expected) {
        // Overflow vs declared size — abandon and report.
        fs_chunked_abort(chunk_state.name);
        return -1;
    }
    UINT bw = 0;
    FRESULT res = f_write(&chunk_state.fil, data, (UINT)len, &bw);
    if (res != FR_OK || (size_t)bw != len) return -1;
    chunk_state.written += len;
    return (int)len;
}

int fs_chunked_finish(const char *name) {
    if (!chunk_state.active) return -1;
    if (strcmp(name, chunk_state.name) != 0) return -1;

    f_sync(&chunk_state.fil);
    f_close(&chunk_state.fil);
    int written = (int)chunk_state.written;

    if (chunk_state.written != chunk_state.expected) {
        char path[64];
        if (build_full_path(name, path, sizeof(path))) f_unlink(path);
        memset(&chunk_state, 0, sizeof(chunk_state));
        return -1;
    }
    memset(&chunk_state, 0, sizeof(chunk_state));
    printf("[filestore] Chunked write complete: %s (%d bytes)\r\n", name, written);
    return written;
}

void fs_chunked_abort(const char *name) {
    if (!chunk_state.active) return;
    if (name && strcmp(name, chunk_state.name) != 0) return;
    f_close(&chunk_state.fil);
    char path[64];
    if (build_full_path(chunk_state.name, path, sizeof(path))) f_unlink(path);
    printf("[filestore] Chunked write aborted: %s\r\n", chunk_state.name);
    memset(&chunk_state, 0, sizeof(chunk_state));
}

const char *fs_chunked_in_progress(void) {
    return chunk_state.active ? chunk_state.name : NULL;
}

// ---------------------------------------------------------------------------
// Chunked read
// ---------------------------------------------------------------------------

int fs_read_start(const char *name) {
    if (!fs_mounted) return -1;
    char path[64];
    if (!build_full_path(name, path, sizeof(path))) return -1;
    FILINFO finfo;
    return f_stat(path, &finfo) == FR_OK ? (int)finfo.fsize : -1;
}

int fs_read_chunk(const char *name, uint32_t offset, uint32_t size,
                  uint8_t *buf, size_t buf_size) {
    if (!fs_mounted) return -1;
    char path[64];
    if (!build_full_path(name, path, sizeof(path))) return -1;
    FIL fil;
    if (f_open(&fil, path, FA_READ) != FR_OK) return -1;
    if (f_lseek(&fil, offset) != FR_OK) { f_close(&fil); return -1; }
    UINT to_read = (size < buf_size) ? (UINT)size : (UINT)buf_size;
    UINT br = 0;
    FRESULT res = f_read(&fil, buf, to_read, &br);
    f_close(&fil);
    return (res == FR_OK) ? (int)br : -1;
}

// ---------------------------------------------------------------------------
// Delete
// ---------------------------------------------------------------------------

int fs_delete(const char *name) {
    if (!fs_mounted) return -1;
    char path[64];
    if (!build_full_path(name, path, sizeof(path))) return -1;
    if (f_unlink(path) != FR_OK) return -1;

    char meta[64];
    if (build_meta_path(name, meta, sizeof(meta))) f_unlink(meta);
    delete_hash_sidecar(name);
    printf("[filestore] Deleted %s\r\n", path);
    return 0;
}

// ---------------------------------------------------------------------------
// Storage info
// ---------------------------------------------------------------------------

void fs_storage_info(uint32_t *free_out, uint32_t *used_out,
                     uint32_t *reserve_out, uint32_t *count_out) {
    if (free_out) *free_out = 0;
    if (used_out) *used_out = 0;
    if (reserve_out) *reserve_out = STORAGE_RESERVE;
    if (count_out) *count_out = 0;

    if (!fs_mounted) return;

    // Free space from FATFS (free clusters * sectors per cluster * sector size).
    DWORD free_clust = 0;
    FATFS *fs_p = NULL;
    if (f_getfree("0:", &free_clust, &fs_p) == FR_OK && fs_p) {
        uint64_t free_bytes = (uint64_t)free_clust * fs_p->csize * DISK_SECTOR_SIZE;
        // Pretend the reserve eats into our reported free space, so the
        // user can't use the last N bytes via app-level UI.
        if (free_bytes > STORAGE_RESERVE) free_bytes -= STORAGE_RESERVE;
        else free_bytes = 0;
        if (free_out) *free_out = (uint32_t)free_bytes;
    }

    // Walk /files to compute used + count. Sidecars (.meta, .h8) are
    // counted as used bytes but excluded from the file count.
    uint32_t used_bytes = 0;
    uint32_t file_count = 0;
    DIR dir;
    if (f_opendir(&dir, FILES_DIR_NAME) == FR_OK) {
        FILINFO finfo;
        while (f_readdir(&dir, &finfo) == FR_OK && finfo.fname[0] != '\0') {
            if (finfo.fattrib & AM_DIR) continue;
            used_bytes += (uint32_t)finfo.fsize;
            if (!name_is_sidecar(finfo.fname)) file_count++;
        }
        f_closedir(&dir);
    }
    if (used_out) *used_out = used_bytes;
    if (count_out) *count_out = file_count;
}

// ---------------------------------------------------------------------------
// Metadata sidecars
// ---------------------------------------------------------------------------

int fs_write_meta(const char *name, const uint8_t *data, size_t len) {
    if (!fs_mounted) return -1;
    char meta[64];
    if (!build_meta_path(name, meta, sizeof(meta))) return -1;
    FIL fil;
    if (f_open(&fil, meta, FA_WRITE | FA_CREATE_ALWAYS) != FR_OK) return -1;
    UINT bw = 0;
    FRESULT res = f_write(&fil, data, (UINT)len, &bw);
    f_sync(&fil);
    f_close(&fil);
    return (res == FR_OK && (size_t)bw == len) ? 0 : -1;
}

int fs_read_meta(const char *name, uint8_t *buf, size_t buf_size) {
    if (!fs_mounted) return -1;
    char meta[64];
    if (!build_meta_path(name, meta, sizeof(meta))) return -1;
    FIL fil;
    if (f_open(&fil, meta, FA_READ) != FR_OK) return -1;
    UINT br = 0;
    FRESULT res = f_read(&fil, buf, (UINT)buf_size, &br);
    f_close(&fil);
    return (res == FR_OK) ? (int)br : -1;
}

// ---------------------------------------------------------------------------
// System blobs (/system/) — never reachable from BLE file commands.
// ---------------------------------------------------------------------------

int fs_system_read(const char *name, uint8_t *buf, size_t buf_size) {
    if (!fs_mounted) return -1;
    char path[64];
    if (!build_system_path(name, path, sizeof(path))) return -1;
    FIL fil;
    if (f_open(&fil, path, FA_READ) != FR_OK) return -1;
    UINT br = 0;
    FRESULT res = f_read(&fil, buf, (UINT)buf_size, &br);
    f_close(&fil);
    return (res == FR_OK) ? (int)br : -1;
}

int fs_system_write(const char *name, const uint8_t *data, size_t len) {
    if (!fs_mounted) return -1;
    char path[64];
    if (!build_system_path(name, path, sizeof(path))) return -1;

    FIL fil;
    if (f_open(&fil, path, FA_WRITE | FA_CREATE_ALWAYS) != FR_OK) return -1;
    UINT bw = 0;
    FRESULT res = f_write(&fil, data, (UINT)len, &bw);
    f_sync(&fil);
    f_close(&fil);
    if (res != FR_OK || (size_t)bw != len) return -1;
    printf("[filestore] Wrote %s (%zu bytes)\r\n", path, len);
    return 0;
}

int fs_system_delete(const char *name) {
    if (!fs_mounted) return -1;
    char path[64];
    if (!build_system_path(name, path, sizeof(path))) return -1;
    if (f_unlink(path) != FR_OK) return -1;
    printf("[filestore] Deleted %s\r\n", path);
    return 0;
}
