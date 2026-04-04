# waxwing/filestore.py
# Simple flat-file storage on Pico W internal flash.
#
# Files are stored in /files/ directory on the flash filesystem.
# Phase 1: text files only, limited to ~2 KB each to stay within
# BLE characteristic buffer limits.

import os

FILES_DIR = "/files"
MAX_FILE_SIZE = 2048  # bytes — keeps things comfy within BLE buffers


def _ensure_dir():
    """Create the /files directory if it doesn't exist."""
    try:
        os.stat(FILES_DIR)
    except OSError:
        os.mkdir(FILES_DIR)
        print("[filestore] Created {}".format(FILES_DIR))


def _path(name):
    """Resolve a filename to its full path. Rejects path traversal."""
    # Strip any directory components for safety
    clean = name.replace("/", "").replace("\\", "").strip()
    if not clean:
        raise ValueError("Empty filename")
    return FILES_DIR + "/" + clean


def list_files():
    """
    Return a list of dicts: [{"name": "foo.txt", "size": 123}, ...]
    Sorted alphabetically.
    """
    _ensure_dir()
    result = []
    for entry in os.listdir(FILES_DIR):
        try:
            stat = os.stat(FILES_DIR + "/" + entry)
            # stat[6] is file size
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


def write_file(name, content):
    """
    Write a text file. Creates or overwrites.
    Raises ValueError if content exceeds MAX_FILE_SIZE.
    """
    _ensure_dir()
    if len(content) > MAX_FILE_SIZE:
        raise ValueError("File too large ({} > {} bytes)".format(
            len(content), MAX_FILE_SIZE))
    path = _path(name)
    with open(path, "w") as f:
        f.write(content)
    print("[filestore] Wrote {} ({} bytes)".format(name, len(content)))


def delete_file(name):
    """Delete a file. Raises OSError if it doesn't exist."""
    _ensure_dir()
    path = _path(name)
    os.remove(path)
    print("[filestore] Deleted {}".format(name))


def file_exists(name):
    """Check if a file exists."""
    _ensure_dir()
    try:
        os.stat(_path(name))
        return True
    except OSError:
        return False
