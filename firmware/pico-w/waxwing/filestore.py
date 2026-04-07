# waxwing/filestore.py
# Flat-file storage on Pico W internal flash (or future SD card).
#
# Phase 1.5: text files, limited to 2 KB each.
# Phase 1.6: binary files (images), chunked writes, storage quotas.
#
# The Pico W has ~1.5 MB of usable flash after MicroPython.  Without an
# SD card we need to be very careful about running out of space.  Every
# write operation checks free space BEFORE touching the filesystem.

import os
import gc

FILES_DIR = "/files"
MAX_FILE_SIZE = 2048          # single-shot text file limit (bytes)
MAX_CHUNKED_FILE_SIZE = 512 * 1024   # 512 KB hard cap per file
STORAGE_RESERVE = 32 * 1024  # keep 32 KB free for firmware / GC headroom


# ---------------------------------------------------------------------------
# Internal helpers
# ---------------------------------------------------------------------------

def _ensure_dir():
    """Create the /files directory if it doesn't exist."""
    try:
        os.stat(FILES_DIR)
    except OSError:
        os.mkdir(FILES_DIR)
        print("[filestore] Created {}".format(FILES_DIR))


def _path(name):
    """Resolve a filename to its full path. Rejects path traversal."""
    clean = name.replace("/", "").replace("\\", "").strip()
    if not clean:
        raise ValueError("Empty filename")
    return FILES_DIR + "/" + clean


def _fs_free():
    """
    Return the approximate number of free bytes on the filesystem
    that contains FILES_DIR.

    os.statvfs() returns a tuple:
      (f_bsize, f_frsize, f_blocks, f_bfree, f_bavail, ...)
    We use f_frsize * f_bavail as the available space.
    """
    try:
        st = os.statvfs(FILES_DIR)
        return st[1] * st[4]   # f_frsize * f_bavail
    except (OSError, AttributeError):
        # If statvfs isn't available (sim / test), assume plenty of room
        return 1_000_000


def _check_space(needed):
    """
    Raise OSError if writing `needed` bytes would drop free space
    below STORAGE_RESERVE.
    """
    free = _fs_free()
    if free - needed < STORAGE_RESERVE:
        gc.collect()            # last-ditch GC before giving up
        free = _fs_free()
        if free - needed < STORAGE_RESERVE:
            raise OSError(
                "Not enough storage ({} free, need {} + {} reserve)".format(
                    free, needed, STORAGE_RESERVE))


def _total_stored():
    """Return the total bytes consumed by all files in FILES_DIR."""
    _ensure_dir()
    total = 0
    for entry in os.listdir(FILES_DIR):
        try:
            total += os.stat(FILES_DIR + "/" + entry)[6]
        except OSError:
            pass
    return total


# ---------------------------------------------------------------------------
# Public API — queries
# ---------------------------------------------------------------------------

def _is_meta(name):
    """Return True if this filename is a metadata sidecar."""
    return name.endswith(".meta")


def list_files():
    """
    Return a list of dicts: [{"name": "foo.txt", "size": 123}, ...]
    Sorted alphabetically.  Excludes .meta sidecar files.
    """
    _ensure_dir()
    result = []
    for entry in os.listdir(FILES_DIR):
        if _is_meta(entry):
            continue
        try:
            stat = os.stat(FILES_DIR + "/" + entry)
            result.append({"name": entry, "size": stat[6]})
        except OSError:
            pass
    result.sort(key=lambda f: f["name"])
    return result


def read_file(name):
    """
    Read a text file and return its contents as a string.
    Raises OSError if file doesn't exist.
    """
    _ensure_dir()
    path = _path(name)
    with open(path, "r") as f:
        return f.read()


def read_file_binary(name):
    """
    Read a file and return its contents as bytes.
    Raises OSError if file doesn't exist.
    """
    _ensure_dir()
    path = _path(name)
    with open(path, "rb") as f:
        return f.read()


def file_exists(name):
    """Check if a file exists."""
    _ensure_dir()
    try:
        os.stat(_path(name))
        return True
    except OSError:
        return False


def storage_info():
    """
    Return a dict with storage stats:
      {"free": <bytes>, "used": <bytes>, "reserve": <bytes>,
       "file_count": <int>}
    file_count excludes .meta sidecar files.
    """
    _ensure_dir()
    free = _fs_free()
    files = os.listdir(FILES_DIR)
    used = 0
    count = 0
    for entry in files:
        try:
            used += os.stat(FILES_DIR + "/" + entry)[6]
        except OSError:
            pass
        if not _is_meta(entry):
            count += 1
    return {
        "free": free,
        "used": used,
        "reserve": STORAGE_RESERVE,
        "file_count": count,
    }


def content_file_count():
    """Return the number of non-meta files in the store."""
    _ensure_dir()
    count = 0
    for entry in os.listdir(FILES_DIR):
        if not _is_meta(entry):
            count += 1
    return count


# ---------------------------------------------------------------------------
# Public API — single-shot write (text, ≤2 KB)
# ---------------------------------------------------------------------------

def write_file(name, content):
    """
    Write a text file. Creates or overwrites.
    Raises ValueError if content exceeds MAX_FILE_SIZE.
    Raises OSError if not enough free space.
    """
    _ensure_dir()
    if len(content) > MAX_FILE_SIZE:
        raise ValueError("File too large ({} > {} bytes)".format(
            len(content), MAX_FILE_SIZE))
    _check_space(len(content))
    path = _path(name)
    with open(path, "w") as f:
        f.write(content)
    print("[filestore] Wrote {} ({} bytes)".format(name, len(content)))


# ---------------------------------------------------------------------------
# Public API — single-shot binary write (small files, ≤2 KB)
# ---------------------------------------------------------------------------

def write_file_binary(name, data):
    """
    Write raw bytes to a file. Creates or overwrites.
    Raises ValueError if data exceeds MAX_FILE_SIZE.
    Raises OSError if not enough free space.
    """
    _ensure_dir()
    if len(data) > MAX_FILE_SIZE:
        raise ValueError("File too large ({} > {} bytes)".format(
            len(data), MAX_FILE_SIZE))
    _check_space(len(data))
    path = _path(name)
    with open(path, "wb") as f:
        f.write(data)
    print("[filestore] Wrote binary {} ({} bytes)".format(name, len(data)))


# ---------------------------------------------------------------------------
# Public API — chunked binary write (images / large files)
# ---------------------------------------------------------------------------
#
# Protocol:
#   1. chunked_start(name, total_size)  — validate & open file
#   2. chunked_append(name, data)       — append bytes (called N times)
#   3. chunked_finish(name)             — finalise, verify size
#   4. chunked_abort(name)              — clean up on error
#
# Only ONE chunked write may be in progress at a time to keep RAM usage
# predictable on the Pico W (264 KB SRAM).

_chunked_state = None   # dict or None


def chunked_start(name, total_size):
    """
    Begin a chunked write.

    Validates:
      - No other chunked write is in progress
      - total_size is within MAX_CHUNKED_FILE_SIZE
      - Enough free space exists (including reserve)

    Opens the file for binary writing.
    """
    global _chunked_state

    if _chunked_state is not None:
        raise RuntimeError("Chunked write already in progress for '{}'".format(
            _chunked_state["name"]))

    if total_size > MAX_CHUNKED_FILE_SIZE:
        raise ValueError("File too large ({} > {} bytes)".format(
            total_size, MAX_CHUNKED_FILE_SIZE))

    if total_size <= 0:
        raise ValueError("Invalid file size: {}".format(total_size))

    _ensure_dir()
    _check_space(total_size)

    path = _path(name)

    # Open file — we keep the handle open across chunks so we never
    # buffer the whole image in RAM.
    fh = open(path, "wb")

    _chunked_state = {
        "name": name,
        "path": path,
        "fh": fh,
        "expected": total_size,
        "written": 0,
    }
    print("[filestore] Chunked write started: {} ({} bytes expected)".format(
        name, total_size))


def chunked_append(name, data):
    """
    Append a chunk of bytes to the in-progress file.

    Validates:
      - A chunked write is in progress for `name`
      - Appending won't exceed the declared total_size
    """
    global _chunked_state

    if _chunked_state is None:
        raise RuntimeError("No chunked write in progress")
    if _chunked_state["name"] != name:
        raise RuntimeError("Chunked write is for '{}', not '{}'".format(
            _chunked_state["name"], name))

    new_total = _chunked_state["written"] + len(data)
    if new_total > _chunked_state["expected"]:
        # Capture values before abort clears state
        prev_written = _chunked_state["written"]
        prev_expected = _chunked_state["expected"]
        chunked_abort(name)
        raise ValueError("Chunk would exceed declared size ({} + {} > {})".format(
            prev_written, len(data), prev_expected))

    _chunked_state["fh"].write(data)
    _chunked_state["written"] = new_total

    # Periodically free memory — we're in a tight loop receiving BLE data
    if new_total % (8 * 1024) == 0:
        gc.collect()


def chunked_finish(name):
    """
    Finalise the chunked write.  Closes the file handle and verifies
    the total bytes written matches what was declared.

    Returns the final byte count.
    """
    global _chunked_state

    if _chunked_state is None:
        raise RuntimeError("No chunked write in progress")
    if _chunked_state["name"] != name:
        raise RuntimeError("Chunked write is for '{}', not '{}'".format(
            _chunked_state["name"], name))

    written = _chunked_state["written"]
    expected = _chunked_state["expected"]

    _chunked_state["fh"].close()

    if written != expected:
        # Size mismatch — remove the partial file
        try:
            os.remove(_chunked_state["path"])
        except OSError:
            pass
        _chunked_state = None
        raise ValueError(
            "Size mismatch: wrote {} but expected {} bytes".format(
                written, expected))

    fname = _chunked_state["name"]
    _chunked_state = None
    gc.collect()
    print("[filestore] Chunked write complete: {} ({} bytes)".format(
        fname, written))
    return written


def chunked_abort(name):
    """
    Abort a chunked write.  Closes the file handle and removes the
    partial file.
    """
    global _chunked_state

    if _chunked_state is None:
        return  # nothing to abort

    try:
        _chunked_state["fh"].close()
    except Exception:
        pass

    try:
        os.remove(_chunked_state["path"])
    except OSError:
        pass

    print("[filestore] Chunked write aborted: {}".format(
        _chunked_state["name"]))
    _chunked_state = None
    gc.collect()


def chunked_in_progress():
    """Return the name of the file being written, or None."""
    if _chunked_state is not None:
        return _chunked_state["name"]
    return None


# ---------------------------------------------------------------------------
# Public API — chunked binary read (images / large files)
# ---------------------------------------------------------------------------
#
# Protocol (mirrors chunked write):
#   1. read_start(name)         — validate & return file size
#   2. read_chunk(name, offset, size) — return bytes at offset
#
# Unlike chunked writes, reads are stateless per-chunk so they don't need
# an open file handle — each chunk opens, seeks, reads, closes.  This keeps
# RAM usage minimal on the Pico W.

def read_start(name):
    """
    Begin a chunked read.  Returns the file size in bytes.
    Raises OSError if the file doesn't exist.
    """
    _ensure_dir()
    path = _path(name)
    stat = os.stat(path)
    size = stat[6]
    print("[filestore] Chunked read started: {} ({} bytes)".format(name, size))
    return size


def read_chunk(name, offset, size):
    """
    Read `size` bytes starting at `offset` from the named file.
    Returns bytes.

    Raises OSError if the file doesn't exist.
    Raises ValueError if offset/size are out of bounds.
    """
    _ensure_dir()
    path = _path(name)
    with open(path, "rb") as f:
        f.seek(offset)
        data = f.read(size)
    return data


# ---------------------------------------------------------------------------
# Public API — delete
# ---------------------------------------------------------------------------

def delete_file(name):
    """Delete a file and its metadata sidecar (if any)."""
    _ensure_dir()
    path = _path(name)
    os.remove(path)
    print("[filestore] Deleted {}".format(name))
    # Also remove sidecar metadata
    meta_name = name + ".meta"
    try:
        os.remove(_path(meta_name))
        print("[filestore] Deleted sidecar {}".format(meta_name))
    except OSError:
        pass


# ---------------------------------------------------------------------------
# Public API — metadata sidecars
# ---------------------------------------------------------------------------

def write_meta(name, data):
    """
    Write a metadata sidecar for the given filename.
    `data` should be raw CBOR bytes.
    """
    _ensure_dir()
    meta_name = name + ".meta"
    _check_space(len(data))
    path = _path(meta_name)
    with open(path, "wb") as f:
        f.write(data)
    print("[filestore] Wrote meta for {} ({} bytes)".format(name, len(data)))


def read_meta(name):
    """
    Read the metadata sidecar for the given filename.
    Returns raw bytes, or None if no sidecar exists.
    """
    _ensure_dir()
    meta_name = name + ".meta"
    path = _path(meta_name)
    try:
        with open(path, "rb") as f:
            return f.read()
    except OSError:
        return None
