# waxwing/cbor.py
# Minimal CBOR encoder/decoder for MicroPython (Pico W).
# Supports the subset of CBOR required for Waxwing wire messages:
#
#   Major type 0  — unsigned integer
#   Major type 1  — negative integer
#   Major type 2  — byte string
#   Major type 3  — text string
#   Major type 4  — array
#   Major type 5  — map (str/int keys only)
#   Major type 7  — float (16/32/64), True, False, None (null)
#
# Does NOT support: tags, indefinite-length items, bignum, etc.
# Compatible with the cbor2 library used in the PC tools.

# ---------------------------------------------------------------------------
# Encoder
# ---------------------------------------------------------------------------

def _encode_head(major, value):
    """Return the CBOR initial byte(s) for a given major type and argument."""
    major <<= 5
    if value <= 23:
        return bytes([major | value])
    elif value <= 0xFF:
        return bytes([major | 24, value])
    elif value <= 0xFFFF:
        return bytes([major | 25, (value >> 8) & 0xFF, value & 0xFF])
    elif value <= 0xFFFFFFFF:
        return bytes([
            major | 26,
            (value >> 24) & 0xFF,
            (value >> 16) & 0xFF,
            (value >> 8)  & 0xFF,
             value        & 0xFF,
        ])
    else:
        return bytes([
            major | 27,
            (value >> 56) & 0xFF,
            (value >> 48) & 0xFF,
            (value >> 40) & 0xFF,
            (value >> 32) & 0xFF,
            (value >> 24) & 0xFF,
            (value >> 16) & 0xFF,
            (value >> 8)  & 0xFF,
             value        & 0xFF,
        ])


def _encode_item(obj):
    if obj is None:
        return b'\xf6'                  # 0xf6 = null
    if obj is True:
        return b'\xf5'                  # 0xf5 = true
    if obj is False:
        return b'\xf4'                  # 0xf4 = false
    if isinstance(obj, int):
        if obj >= 0:
            return _encode_head(0, obj)
        else:
            return _encode_head(1, -1 - obj)
    if isinstance(obj, (bytes, bytearray, memoryview)):
        b = bytes(obj)
        return _encode_head(2, len(b)) + b
    if isinstance(obj, str):
        b = obj.encode("utf-8")
        return _encode_head(3, len(b)) + b
    if isinstance(obj, (list, tuple)):
        out = _encode_head(4, len(obj))
        for item in obj:
            out += _encode_item(item)
        return out
    if isinstance(obj, dict):
        out = _encode_head(5, len(obj))
        for k, v in obj.items():
            out += _encode_item(k)
            out += _encode_item(v)
        return out
    if isinstance(obj, float):
        # Encode as IEEE 754 double (major 7, additional 27)
        import struct
        b = struct.pack(">d", obj)
        return b'\xfb' + b
    raise TypeError("cbor: unsupported type: {}".format(type(obj)))


def dumps(obj):
    """Encode obj to CBOR bytes."""
    return _encode_item(obj)


# ---------------------------------------------------------------------------
# Decoder
# ---------------------------------------------------------------------------

class _Decoder:
    def __init__(self, data):
        self._data = memoryview(data)
        self._pos  = 0

    def _read(self, n):
        v = self._data[self._pos:self._pos + n]
        self._pos += n
        if len(v) < n:
            raise ValueError("cbor: truncated input")
        return bytes(v)

    def _read_byte(self):
        b = self._data[self._pos]
        self._pos += 1
        return b

    def _decode_head(self):
        """Return (major, value) pair."""
        b = self._read_byte()
        major    = (b >> 5) & 0x07
        addl     = b & 0x1F
        if addl <= 23:
            return major, addl
        if addl == 24:
            return major, self._read_byte()
        if addl == 25:
            raw = self._read(2)
            return major, (raw[0] << 8) | raw[1]
        if addl == 26:
            raw = self._read(4)
            v = 0
            for byte in raw:
                v = (v << 8) | byte
            return major, v
        if addl == 27:
            raw = self._read(8)
            v = 0
            for byte in raw:
                v = (v << 8) | byte
            return major, v
        raise ValueError("cbor: unsupported additional info: {}".format(addl))

    def decode(self):
        b = self._data[self._pos]
        major = (b >> 5) & 0x07
        addl  =  b & 0x1F

        # ---- major type 7: floats, bool, null ----
        if major == 7:
            self._pos += 1
            if addl == 20:
                return False        # 0xf4
            if addl == 21:
                return True         # 0xf5
            if addl == 22:
                return None         # 0xf6
            if addl == 23:
                return None         # 0xf7 undefined -> None
            if addl == 25:
                # float16
                raw = self._read(2)
                return _decode_f16(raw)
            if addl == 26:
                import struct
                raw = self._read(4)
                return struct.unpack(">f", raw)[0]
            if addl == 27:
                import struct
                raw = self._read(8)
                return struct.unpack(">d", raw)[0]
            raise ValueError("cbor: unsupported major-7 additional: {}".format(addl))

        # ---- all other majors: read head ----
        major, value = self._decode_head()

        if major == 0:              # unsigned int
            return value
        if major == 1:              # negative int
            return -1 - value
        if major == 2:              # byte string
            return self._read(value)
        if major == 3:              # text string
            return self._read(value).decode("utf-8")
        if major == 4:              # array
            return [self.decode() for _ in range(value)]
        if major == 5:              # map
            d = {}
            for _ in range(value):
                k = self.decode()
                v = self.decode()
                d[k] = v
            return d
        raise ValueError("cbor: unsupported major type: {}".format(major))


def _decode_f16(raw):
    """Decode a 2-byte IEEE 754 half-precision float."""
    h = (raw[0] << 8) | raw[1]
    exp  = (h >> 10) & 0x1F
    mant =  h        & 0x3FF
    sign = -1 if h & 0x8000 else 1
    if exp == 0:
        value = mant * (2 ** -24)
    elif exp == 31:
        value = float('inf') if mant == 0 else float('nan')
    else:
        value = (1 + mant / 1024) * (2 ** (exp - 15))
    return sign * value


def loads(data):
    """Decode CBOR bytes to a Python object."""
    dec = _Decoder(data)
    obj = dec.decode()
    return obj
