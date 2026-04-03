# waxwing/ble.py
# BLE GATT server for Waxwing Mesh (Pico W / MicroPython).
#
# Responsibilities:
#   - Register the Waxwing GATT service and all Phase-1 characteristics
#   - Build and split advertisement payload (adv_data + resp_data)
#   - Start advertising; restart after disconnect
#   - Handle IRQ events: connect, disconnect, read, write
#   - Expose a WaxwingBLE class that main.py wires together
#
# BLE advertisement budget (31-byte hard limit per payload):
#   adv_data  : Flags(3) + 128-bit UUID(18) = 21 bytes  ✓
#   resp_data : Complete Local Name "WX:AABBCCDD"(13)    ✓
#
# GATT characteristic properties:
#   READ  = 0x02
#   WRITE = 0x08
#   NOTIFY = 0x10
#
# All 12 Waxwing characteristics are registered; in Phase 1 only
# CHAR_DEVICE_IDENTITY responds with real data; the rest return empty bytes.

import bluetooth
import struct
from micropython import const

# BLE event codes (from MicroPython bluetooth module)
_IRQ_CENTRAL_CONNECT         = const(1)
_IRQ_CENTRAL_DISCONNECT      = const(2)
_IRQ_GATTS_WRITE             = const(3)
_IRQ_GATTS_READ_REQUEST      = const(4)

# GATT characteristic flags
_FLAG_READ   = const(0x0002)
_FLAG_WRITE  = const(0x0008)
_FLAG_NOTIFY = const(0x0010)

# AD types for advertisement packets
_AD_TYPE_FLAGS           = const(0x01)
_AD_TYPE_UUID128_FULL    = const(0x07)   # complete list of 128-bit UUIDs
_AD_TYPE_COMPLETE_NAME   = const(0x09)


def _uuid_to_bytes(uuid_str):
    """
    Convert a 128-bit UUID string to little-endian bytes for BLE advertisements.
    Input format: "XXXXXXXX-XXXX-XXXX-XXXX-XXXXXXXXXXXX"
    """
    hex_str = uuid_str.replace("-", "")
    b = bytes(int(hex_str[i:i+2], 16) for i in range(0, 32, 2))
    return bytes(reversed(b))   # little-endian


def _ad_element(ad_type, payload):
    """Wrap payload bytes in a (length, type, payload) AD structure."""
    return bytes([len(payload) + 1, ad_type]) + payload


def _build_adv_data(service_uuid):
    """
    Build the primary advertisement payload.
    Contents: LE General Discoverable + BR/EDR not supported, + 128-bit UUID.
    Total: 3 + 18 = 21 bytes.
    """
    flags   = _ad_element(_AD_TYPE_FLAGS, bytes([0x06]))         # 3 bytes
    uuid128 = _ad_element(_AD_TYPE_UUID128_FULL,
                          _uuid_to_bytes(service_uuid))          # 18 bytes
    return flags + uuid128


def _build_resp_data(node_name):
    """
    Build the scan response payload.
    Contents: Complete Local Name, e.g. "WX:AABBCCDD" (11 chars → 13 bytes).
    """
    name_bytes = node_name.encode("utf-8")
    return _ad_element(_AD_TYPE_COMPLETE_NAME, name_bytes)


class WaxwingBLE:
    """
    Manages the Waxwing BLE GATT server on the Pico W.

    Usage:
        ble = WaxwingBLE(identity)
        ble.start()          # begin advertising + serving
        while True:
            ble.tick()       # call frequently from main loop
    """

    def __init__(self, identity, messages_module):
        """
        Parameters
        ----------
        identity : dict
            From identity.load_or_generate().
        messages_module : module
            The waxwing.messages module (passed to avoid circular import).
        """
        self._identity  = identity
        self._messages  = messages_module
        self._ble       = bluetooth.BLE()
        self._connected = False
        self._conn_handle = None

        # Characteristic value handles (populated after GATT registration)
        self._handles = {}

        # Pre-built advertisement payloads
        from .constants import SERVICE_UUID
        self._adv_data  = _build_adv_data(SERVICE_UUID)
        self._resp_data = _build_resp_data(identity["node_name"])

        # Callbacks registered by main.py
        self._on_connect_cb    = None
        self._on_disconnect_cb = None
        self._on_write_cb      = None

    # -----------------------------------------------------------------------
    # Public API
    # -----------------------------------------------------------------------

    def on_connect(self, cb):
        """Register callback: cb(conn_handle)"""
        self._on_connect_cb = cb

    def on_disconnect(self, cb):
        """Register callback: cb(conn_handle)"""
        self._on_disconnect_cb = cb

    def on_write(self, cb):
        """Register callback: cb(conn_handle, char_uuid_str, data_bytes)"""
        self._on_write_cb = cb

    def start(self):
        """Activate BLE, register GATT service, and start advertising."""
        self._ble.active(True)
        self._ble.irq(self._irq_handler)
        self._register_services()
        self._update_device_identity()
        self._advertise()
        print("[ble] Advertising as {}".format(self._identity["node_name"]))

    def stop(self):
        """Stop advertising and deactivate BLE."""
        self._ble.gap_advertise(None)
        self._ble.active(False)
        self._connected = False
        print("[ble] Stopped")

    def tick(self):
        """Call from main loop; currently a no-op (IRQs handle events)."""
        pass

    @property
    def connected(self):
        return self._connected

    def update_device_identity(self, manifest_count=0, attended=False):
        """
        Rebuild and re-write the Device Identity characteristic value.
        Call this whenever the manifest changes or pairing state changes.
        """
        self._update_device_identity(manifest_count=manifest_count,
                                     attended=attended)

    # -----------------------------------------------------------------------
    # GATT service registration
    # -----------------------------------------------------------------------

    def _register_services(self):
        """
        Register the Waxwing GATT service with all 12 characteristics.
        MicroPython bluetooth.BLE.gatts_register_services() takes a list of
        (service_uuid, [(char_uuid, flags), ...]) tuples.
        """
        from .constants import (
            SERVICE_UUID,
            CHAR_DEVICE_IDENTITY,
            CHAR_MANIFEST_META,
            CHAR_MANIFEST_CHUNK,
            CHAR_TRANSFER_REQUEST,
            CHAR_TRANSFER_DATA,
            CHAR_TRANSFER_ACK,
            CHAR_RATING_SUBMIT,
            CHAR_REP_EXCHANGE,
            CHAR_WIFI_NEGOTIATE,
            CHAR_PAIRING_AUTH,
            CHAR_SYNC_ATTEST,
            CHAR_ENCOUNTER_LEDGER,
        )

        # Ordered list of (uuid, flags) for each characteristic.
        # The order must match the unpacking of gatts_register_services() result.
        char_defs = [
            (CHAR_DEVICE_IDENTITY,   _FLAG_READ),
            (CHAR_MANIFEST_META,     _FLAG_READ | _FLAG_NOTIFY),
            (CHAR_MANIFEST_CHUNK,    _FLAG_READ | _FLAG_WRITE),
            (CHAR_TRANSFER_REQUEST,  _FLAG_WRITE),
            (CHAR_TRANSFER_DATA,     _FLAG_READ | _FLAG_NOTIFY),
            (CHAR_TRANSFER_ACK,      _FLAG_WRITE),
            (CHAR_RATING_SUBMIT,     _FLAG_WRITE),
            (CHAR_REP_EXCHANGE,      _FLAG_READ | _FLAG_NOTIFY),
            (CHAR_WIFI_NEGOTIATE,    _FLAG_READ | _FLAG_WRITE),
            (CHAR_PAIRING_AUTH,      _FLAG_WRITE),
            (CHAR_SYNC_ATTEST,       _FLAG_READ | _FLAG_WRITE),
            (CHAR_ENCOUNTER_LEDGER,  _FLAG_READ),
        ]

        # Build the service tuple expected by MicroPython
        service_def = (
            bluetooth.UUID(SERVICE_UUID),
            [(bluetooth.UUID(uuid), flags) for uuid, flags in char_defs],
        )

        # Register; returns a list of lists of handles: [[h0, h1, ...]]
        ((h_dev_id, h_mfst_meta, h_mfst_chunk,
          h_xfr_req, h_xfr_data, h_xfr_ack,
          h_rating, h_rep_ex, h_wifi_neg,
          h_pair_auth, h_sync_att, h_enc_ledger),) = \
            self._ble.gatts_register_services([service_def])

        # Store handles indexed by UUID string for easy lookup in IRQ handler
        self._handles = {
            CHAR_DEVICE_IDENTITY:   h_dev_id,
            CHAR_MANIFEST_META:     h_mfst_meta,
            CHAR_MANIFEST_CHUNK:    h_mfst_chunk,
            CHAR_TRANSFER_REQUEST:  h_xfr_req,
            CHAR_TRANSFER_DATA:     h_xfr_data,
            CHAR_TRANSFER_ACK:      h_xfr_ack,
            CHAR_RATING_SUBMIT:     h_rating,
            CHAR_REP_EXCHANGE:      h_rep_ex,
            CHAR_WIFI_NEGOTIATE:    h_wifi_neg,
            CHAR_PAIRING_AUTH:      h_pair_auth,
            CHAR_SYNC_ATTEST:       h_sync_att,
            CHAR_ENCOUNTER_LEDGER:  h_enc_ledger,
        }

        # Reverse map: handle -> UUID string (for IRQ callbacks)
        self._handle_to_uuid = {v: k for k, v in self._handles.items()}

        print("[ble] GATT service registered ({} characteristics)".format(
            len(char_defs)))

    # -----------------------------------------------------------------------
    # Characteristic value management
    # -----------------------------------------------------------------------

    def _update_device_identity(self, manifest_count=0, attended=False):
        """Encode and write the Device Identity characteristic value."""
        from .constants import CHAR_DEVICE_IDENTITY
        payload = self._messages.build_device_identity(
            self._identity,
            manifest_count=manifest_count,
            attended=attended,
        )
        h = self._handles.get(CHAR_DEVICE_IDENTITY)
        if h is not None:
            self._ble.gatts_write(h, payload)
            print("[ble] Device Identity updated ({} bytes)".format(len(payload)))

    # -----------------------------------------------------------------------
    # Advertising
    # -----------------------------------------------------------------------

    def _advertise(self, interval_us=500_000):
        """
        Start or restart BLE advertising.
        interval_us: advertising interval in microseconds (default 500 ms).
        """
        self._ble.gap_advertise(
            interval_us,
            adv_data=self._adv_data,
            resp_data=self._resp_data,
            connectable=True,
        )

    # -----------------------------------------------------------------------
    # IRQ handler
    # -----------------------------------------------------------------------

    def _irq_handler(self, event, data):
        if event == _IRQ_CENTRAL_CONNECT:
            conn_handle, addr_type, addr = data
            self._connected   = True
            self._conn_handle = conn_handle
            addr_str = ":".join("{:02x}".format(b) for b in bytes(addr))
            print("[ble] Connected: handle={} addr={}".format(
                conn_handle, addr_str))
            if self._on_connect_cb:
                self._on_connect_cb(conn_handle)

        elif event == _IRQ_CENTRAL_DISCONNECT:
            conn_handle, addr_type, addr = data
            self._connected   = False
            self._conn_handle = None
            print("[ble] Disconnected: handle={}".format(conn_handle))
            if self._on_disconnect_cb:
                self._on_disconnect_cb(conn_handle)
            # Restart advertising so the next peer can find us
            self._advertise()

        elif event == _IRQ_GATTS_WRITE:
            conn_handle, attr_handle = data
            value = self._ble.gatts_read(attr_handle)
            uuid_str = self._handle_to_uuid.get(attr_handle, "unknown")
            print("[ble] Write: char={} len={}".format(
                uuid_str[-8:], len(value)))
            if self._on_write_cb:
                self._on_write_cb(conn_handle, uuid_str, bytes(value))

        elif event == _IRQ_GATTS_READ_REQUEST:
            # MicroPython calls this before sending a characteristic value.
            # We pre-populate all values with gatts_write(), so nothing to do
            # here for Phase 1.  Return 0 to allow the read.
            pass
