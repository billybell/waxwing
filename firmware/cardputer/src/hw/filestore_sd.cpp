// CardPuter SD-card filestore. Implements core/filestore.h against the
// ESP32 Arduino SD library. Mirrors the Pico W FatFS impl
// (firmware/pico-w/src/hw/pico-w/filestore_fatfs.c) section by section so
// the contracts stay aligned: same /files/ + /system/ split, same .meta
// sidecars, same single-shot 2 KB / chunked 512 KB caps, same 32 KB
// reserve, same zero-byte hash placeholder.

extern "C" {
#include "core/filestore.h"
#include "core/hal_crypto.h"
}

#include <Arduino.h>
#include <SD.h>
#include <SPI.h>

#include <cstdio>
#include <cstring>

namespace {

// Pin assignments for the M5 Cardputer (and Cardputer ADV) microSD slot.
// If the ADV has different pins, surface as a build flag rather than
// hard-edit here.
constexpr int kPinCS   = 12;
constexpr int kPinSCK  = 40;
constexpr int kPinMISO = 39;
constexpr int kPinMOSI = 14;

constexpr const char *kFilesDir    = "/files";
constexpr const char *kSystemDir   = "/system";
constexpr const char *kMetaSuffix  = ".meta";
constexpr const char *kHashSuffix  = ".h8";
constexpr size_t      kMaxShotFile = 2048;
constexpr uint32_t    kMaxChunked  = 512u * 1024u;
constexpr uint32_t    kStorageReserve = 32u * 1024u;
constexpr int         kListStageMax   = 64;

bool g_mounted = false;

struct ChunkState {
    char     name[FS_MAX_NAME_LEN];
    File     file;
    uint32_t expected;
    uint32_t written;
    bool     active;
};
ChunkState g_chunk;

bool has_suffix(const char *name, const char *suffix) {
    const size_t nl = std::strlen(name);
    const size_t sl = std::strlen(suffix);
    return nl > sl && std::strcmp(name + nl - sl, suffix) == 0;
}

bool name_is_meta(const char *name) {
    return has_suffix(name, kMetaSuffix);
}

bool name_is_hash_sidecar(const char *name) {
    return has_suffix(name, kHashSuffix);
}

bool name_is_sidecar(const char *name) {
    return name_is_meta(name) || name_is_hash_sidecar(name);
}

bool valid_name(const char *name) {
    if (!name || !*name) return false;
    if (std::strchr(name, '/') != nullptr) return false;
    if (std::strlen(name) >= FS_MAX_NAME_LEN) return false;
    return true;
}

bool build_path(const char *dir, const char *name, char *out, size_t outsz,
                const char *suffix = nullptr) {
    if (!valid_name(name)) return false;
    int n = suffix
        ? std::snprintf(out, outsz, "%s/%s%s", dir, name, suffix)
        : std::snprintf(out, outsz, "%s/%s", dir, name);
    return n > 0 && static_cast<size_t>(n) < outsz;
}

bool ensure_dir(const char *path) {
    if (SD.exists(path)) return true;
    return SD.mkdir(path);
}

// ---------------------------------------------------------------------------
// Hash sidecar helpers (mirrors filestore_fatfs.c — see notes there).
// ---------------------------------------------------------------------------

int read_hash_sidecar(const char *name, uint8_t out_hash[8]) {
    char path[64];
    if (!build_path(kFilesDir, name, path, sizeof(path), kHashSuffix)) return -1;
    File f = SD.open(path, FILE_READ);
    if (!f) return -1;
    int n = f.read(out_hash, 8);
    f.close();
    return n == 8 ? 0 : -1;
}

void write_hash_sidecar(const char *name, const uint8_t *hash_8plus) {
    char path[64];
    if (!build_path(kFilesDir, name, path, sizeof(path), kHashSuffix)) return;
    // Arduino SD's FILE_WRITE doesn't truncate; remove first so that a
    // shorter old sidecar can't leave trailing bytes.
    SD.remove(path);
    File f = SD.open(path, FILE_WRITE, /*create=*/true);
    if (!f) return;
    f.write(hash_8plus, 8);
    f.flush();
    f.close();
}

void delete_hash_sidecar(const char *name) {
    char path[64];
    if (build_path(kFilesDir, name, path, sizeof(path), kHashSuffix)) {
        SD.remove(path);
    }
}

}  // namespace

extern "C" int fs_init(void) {
    SPI.begin(kPinSCK, kPinMISO, kPinMOSI, kPinCS);
    if (!SD.begin(kPinCS, SPI, 25000000)) {
        std::printf("[filestore] SD.begin failed\r\n");
        return -1;
    }
    if (!ensure_dir(kFilesDir) || !ensure_dir(kSystemDir)) {
        std::printf("[filestore] mkdir failed\r\n");
        return -1;
    }
    std::memset(&g_chunk, 0, sizeof(g_chunk));
    g_mounted = true;
    return 0;
}

// ---------------------------------------------------------------------------
// Listing
// ---------------------------------------------------------------------------

extern "C" int fs_list(char (*out_names)[FS_MAX_NAME_LEN], uint32_t *out_sizes,
                       uint8_t (*out_hash)[8], int out_max, int offset,
                       int limit, int *next_offset) {
    if (!g_mounted) return -1;

    File dir = SD.open(kFilesDir);
    if (!dir || !dir.isDirectory()) return -1;

    char     stage_names[kListStageMax][FS_MAX_NAME_LEN];
    uint32_t stage_sizes[kListStageMax];
    int      total = 0;

    for (File f = dir.openNextFile(); f && total < kListStageMax;
         f = dir.openNextFile()) {
        if (f.isDirectory()) { f.close(); continue; }
        const char *fname = f.name();
        // Some Arduino SD cores return the full path here; reduce to leaf.
        const char *slash = std::strrchr(fname, '/');
        if (slash) fname = slash + 1;
        if (name_is_sidecar(fname)) { f.close(); continue; }

        std::strncpy(stage_names[total], fname, FS_MAX_NAME_LEN - 1);
        stage_names[total][FS_MAX_NAME_LEN - 1] = '\0';
        stage_sizes[total] = static_cast<uint32_t>(f.size());
        total++;
        f.close();
    }
    dir.close();

    // Insertion sort, alphabetical. Tiny dataset; matches Pico W.
    for (int i = 1; i < total; i++) {
        char tmp_n[FS_MAX_NAME_LEN];
        uint32_t tmp_s;
        std::memcpy(tmp_n, stage_names[i], FS_MAX_NAME_LEN);
        tmp_s = stage_sizes[i];
        int j = i;
        while (j > 0 && std::strcmp(stage_names[j - 1], tmp_n) > 0) {
            std::memcpy(stage_names[j], stage_names[j - 1], FS_MAX_NAME_LEN);
            stage_sizes[j] = stage_sizes[j - 1];
            j--;
        }
        std::memcpy(stage_names[j], tmp_n, FS_MAX_NAME_LEN);
        stage_sizes[j] = tmp_s;
    }

    int start = offset < total ? offset : total;
    int end   = start + limit;
    if (end > total) end = total;
    if (end - start > out_max) end = start + out_max;
    int count = end - start;

    for (int i = 0; i < count; i++) {
        std::strncpy(out_names[i], stage_names[start + i], FS_MAX_NAME_LEN - 1);
        out_names[i][FS_MAX_NAME_LEN - 1] = '\0';
        out_sizes[i] = stage_sizes[start + i];
        if (fs_get_hash(out_names[i], out_hash[i]) != 0) {
            std::memset(out_hash[i], 0, 8);
        }
    }

    if (next_offset) *next_offset = end < total ? end : 0;
    return count;
}

extern "C" int fs_file_size(const char *name) {
    if (!g_mounted) return -1;
    char path[64];
    if (!build_path(kFilesDir, name, path, sizeof(path))) return -1;
    File f = SD.open(path);
    if (!f) return -1;
    int sz = static_cast<int>(f.size());
    f.close();
    return sz;
}

namespace {

int compute_file_hash_8(const char *path, uint8_t out_hash[8]) {
    File f = SD.open(path, FILE_READ);
    if (!f) return -1;

    hal_sha256_ctx_t *s = hal_sha256_init();
    if (!s) { f.close(); return -1; }

    uint8_t buf[256];
    for (;;) {
        int n = f.read(buf, sizeof(buf));
        if (n < 0) {
            hal_sha256_free(s);
            f.close();
            return -1;
        }
        if (n == 0) break;
        hal_sha256_update(s, buf, static_cast<size_t>(n));
        if (static_cast<size_t>(n) < sizeof(buf)) break;
    }

    uint8_t digest[32];
    hal_sha256_final(s, digest);
    hal_sha256_free(s);
    f.close();

    std::memcpy(out_hash, digest, 8);
    return 0;
}

}  // namespace

extern "C" int fs_get_hash(const char *name, uint8_t out_hash[8]) {
    if (!g_mounted) return -1;

    if (read_hash_sidecar(name, out_hash) == 0) return 0;

    char path[64];
    if (!build_path(kFilesDir, name, path, sizeof(path))) return -1;
    if (compute_file_hash_8(path, out_hash) != 0) return -1;
    write_hash_sidecar(name, out_hash);
    return 0;
}

// ---------------------------------------------------------------------------
// Single-shot read / write
// ---------------------------------------------------------------------------

extern "C" int fs_read(const char *name, uint8_t *buf, size_t buf_size) {
    if (!g_mounted) return -1;
    char path[64];
    if (!build_path(kFilesDir, name, path, sizeof(path))) return -1;
    File f = SD.open(path, FILE_READ);
    if (!f) return -1;
    int n = f.read(buf, buf_size);
    f.close();
    return n < 0 ? -1 : n;
}

extern "C" int fs_write(const char *name, const uint8_t *data, size_t len) {
    if (!g_mounted) return -1;
    if (len > kMaxShotFile) return -1;

    char path[64];
    if (!build_path(kFilesDir, name, path, sizeof(path))) return -1;

    File f = SD.open(path, FILE_WRITE, /*create=*/true);
    if (!f) return -1;
    size_t bw = f.write(data, len);
    f.flush();
    f.close();
    if (bw != len) {
        delete_hash_sidecar(name);
        return -1;
    }

    // Eager sidecar refresh while data is still in RAM.
    uint8_t digest[32];
    if (hal_sha256_blob(data, len, digest)) {
        write_hash_sidecar(name, digest);
    } else {
        delete_hash_sidecar(name);
    }

    std::printf("[filestore] Wrote %s (%zu bytes)\r\n", path, len);
    return 0;
}

// ---------------------------------------------------------------------------
// Chunked write
// ---------------------------------------------------------------------------

extern "C" int fs_chunked_start(const char *name, uint32_t total_size) {
    if (!g_mounted) return -1;
    if (total_size == 0 || total_size > kMaxChunked) return -1;

    if (g_chunk.active) {
        std::printf("[filestore] Aborting stale chunked write '%s' (%u/%u) for new '%s'\r\n",
                    g_chunk.name, static_cast<unsigned>(g_chunk.written),
                    static_cast<unsigned>(g_chunk.expected), name);
        fs_chunked_abort(nullptr);
    }

    char path[64];
    if (!build_path(kFilesDir, name, path, sizeof(path))) return -1;

    File f = SD.open(path, FILE_WRITE, /*create=*/true);
    if (!f) return -1;
    // File is about to be overwritten — drop stale hash sidecar.
    delete_hash_sidecar(name);
    g_chunk.file = f;
    std::strncpy(g_chunk.name, name, FS_MAX_NAME_LEN - 1);
    g_chunk.name[FS_MAX_NAME_LEN - 1] = '\0';
    g_chunk.expected = total_size;
    g_chunk.written  = 0;
    g_chunk.active   = true;
    std::printf("[filestore] Chunked write started: %s (%u bytes)\r\n", name,
                static_cast<unsigned>(total_size));
    return 0;
}

extern "C" int fs_chunked_append(const uint8_t *data, size_t len) {
    if (!g_chunk.active) return -1;
    if (g_chunk.written + len > g_chunk.expected) {
        fs_chunked_abort(g_chunk.name);
        return -1;
    }
    size_t bw = g_chunk.file.write(data, len);
    if (bw != len) return -1;
    g_chunk.written += len;
    return static_cast<int>(len);
}

extern "C" int fs_chunked_finish(const char *name) {
    if (!g_chunk.active) return -1;
    if (std::strcmp(name, g_chunk.name) != 0) return -1;

    g_chunk.file.flush();
    g_chunk.file.close();
    int written = static_cast<int>(g_chunk.written);

    if (g_chunk.written != g_chunk.expected) {
        char path[64];
        if (build_path(kFilesDir, name, path, sizeof(path))) SD.remove(path);
        std::memset(&g_chunk, 0, sizeof(g_chunk));
        return -1;
    }
    std::memset(&g_chunk, 0, sizeof(g_chunk));
    std::printf("[filestore] Chunked write complete: %s (%d bytes)\r\n", name, written);
    return written;
}

extern "C" void fs_chunked_abort(const char *name) {
    if (!g_chunk.active) return;
    if (name && std::strcmp(name, g_chunk.name) != 0) return;
    g_chunk.file.close();
    char path[64];
    if (build_path(kFilesDir, g_chunk.name, path, sizeof(path))) SD.remove(path);
    std::printf("[filestore] Chunked write aborted: %s\r\n", g_chunk.name);
    std::memset(&g_chunk, 0, sizeof(g_chunk));
}

extern "C" const char *fs_chunked_in_progress(void) {
    return g_chunk.active ? g_chunk.name : nullptr;
}

// ---------------------------------------------------------------------------
// Chunked read
// ---------------------------------------------------------------------------

extern "C" int fs_read_start(const char *name) {
    return fs_file_size(name);
}

extern "C" int fs_read_chunk(const char *name, uint32_t offset, uint32_t size,
                             uint8_t *buf, size_t buf_size) {
    if (!g_mounted) return -1;
    char path[64];
    if (!build_path(kFilesDir, name, path, sizeof(path))) return -1;
    File f = SD.open(path, FILE_READ);
    if (!f) return -1;
    if (!f.seek(offset)) { f.close(); return -1; }
    uint32_t to_read = (size < buf_size) ? size : static_cast<uint32_t>(buf_size);
    int n = f.read(buf, to_read);
    f.close();
    return n < 0 ? -1 : n;
}

// ---------------------------------------------------------------------------
// Delete
// ---------------------------------------------------------------------------

extern "C" int fs_delete(const char *name) {
    if (!g_mounted) return -1;
    char path[64];
    if (!build_path(kFilesDir, name, path, sizeof(path))) return -1;
    if (!SD.remove(path)) return -1;

    char meta[64];
    if (build_path(kFilesDir, name, meta, sizeof(meta), kMetaSuffix)) {
        SD.remove(meta);  // best-effort: missing meta is fine
    }
    delete_hash_sidecar(name);
    std::printf("[filestore] Deleted %s\r\n", path);
    return 0;
}

// ---------------------------------------------------------------------------
// Storage info
// ---------------------------------------------------------------------------

extern "C" void fs_storage_info(uint32_t *free_out, uint32_t *used_out,
                                uint32_t *reserve_out, uint32_t *count_out) {
    if (free_out)    *free_out    = 0;
    if (used_out)    *used_out    = 0;
    if (reserve_out) *reserve_out = kStorageReserve;
    if (count_out)   *count_out   = 0;

    if (!g_mounted) return;

    const uint64_t total = SD.totalBytes();
    const uint64_t used  = SD.usedBytes();
    uint64_t free_bytes = (total > used) ? (total - used) : 0;
    if (free_bytes > kStorageReserve) free_bytes -= kStorageReserve;
    else free_bytes = 0;
    if (free_out) *free_out = static_cast<uint32_t>(free_bytes);

    uint32_t files_used  = 0;
    uint32_t files_count = 0;
    File dir = SD.open(kFilesDir);
    if (dir && dir.isDirectory()) {
        for (File f = dir.openNextFile(); f; f = dir.openNextFile()) {
            if (!f.isDirectory()) {
                files_used += static_cast<uint32_t>(f.size());
                const char *fname = f.name();
                const char *slash = std::strrchr(fname, '/');
                if (slash) fname = slash + 1;
                if (!name_is_sidecar(fname)) files_count++;
            }
            f.close();
        }
        dir.close();
    }
    if (used_out)  *used_out  = files_used;
    if (count_out) *count_out = files_count;
}

// ---------------------------------------------------------------------------
// Metadata sidecars
// ---------------------------------------------------------------------------

extern "C" int fs_write_meta(const char *name, const uint8_t *data, size_t len) {
    if (!g_mounted) return -1;
    char meta[64];
    if (!build_path(kFilesDir, name, meta, sizeof(meta), kMetaSuffix)) return -1;
    File f = SD.open(meta, FILE_WRITE, /*create=*/true);
    if (!f) return -1;
    size_t bw = f.write(data, len);
    f.flush();
    f.close();
    return (bw == len) ? 0 : -1;
}

extern "C" int fs_read_meta(const char *name, uint8_t *buf, size_t buf_size) {
    if (!g_mounted) return -1;
    char meta[64];
    if (!build_path(kFilesDir, name, meta, sizeof(meta), kMetaSuffix)) return -1;
    File f = SD.open(meta, FILE_READ);
    if (!f) return -1;
    int n = f.read(buf, buf_size);
    f.close();
    return n < 0 ? -1 : n;
}

// ---------------------------------------------------------------------------
// System blobs (/system/) — never reachable from BLE file commands.
// ---------------------------------------------------------------------------

extern "C" int fs_system_read(const char *name, uint8_t *buf, size_t buf_size) {
    if (!g_mounted) return -1;
    char path[64];
    if (!build_path(kSystemDir, name, path, sizeof(path))) return -1;
    File f = SD.open(path, FILE_READ);
    if (!f) return -1;
    int n = f.read(buf, buf_size);
    f.close();
    return n < 0 ? -1 : n;
}

extern "C" int fs_system_write(const char *name, const uint8_t *data, size_t len) {
    if (!g_mounted) return -1;
    char path[64];
    if (!build_path(kSystemDir, name, path, sizeof(path))) return -1;
    File f = SD.open(path, FILE_WRITE, /*create=*/true);
    if (!f) return -1;
    size_t bw = f.write(data, len);
    f.flush();
    f.close();
    if (bw != len) return -1;
    std::printf("[filestore] Wrote %s (%zu bytes)\r\n", path, len);
    return 0;
}

extern "C" int fs_system_delete(const char *name) {
    if (!g_mounted) return -1;
    char path[64];
    if (!build_path(kSystemDir, name, path, sizeof(path))) return -1;
    if (!SD.remove(path)) return -1;
    std::printf("[filestore] Deleted %s\r\n", path);
    return 0;
}
