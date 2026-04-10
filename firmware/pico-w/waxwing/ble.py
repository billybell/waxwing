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

        # Monotonic session counter — incremented on every connect.
        # Useful for correlating logs across a connect/disconnect cycle and
        # spotting cases where session N+1 starts before session N is fully
        # torn down.
        self._session_id = 0

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

        # File command handler (set by main.py)
        self._on_file_cmd_cb   = None

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

    def on_file_command(self, cb):
        """Register callback: cb(data_bytes) -> response_bytes"""
        self._on_file_cmd_cb = cb

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
        Register the Waxwing GATT service with active characteristics.

        NOTE: The Pico W BLE stack has a limited attribute handle table
        (~20 handles). Each characteristic consumes 2 handles (declaration
        + value) plus 1 more for a CCCD if it supports NOTIFY/INDICATE.
        Registering all 14 characteristics exceeds this limit and causes
        the later ones to be silently invisible to the central.

        We only register characteristics that are actually implemented.
        Stub characteristics for future phases (manifest, transfer, rating,
        reputation, WiFi, pairing, sync, encounter) will be added back
        when they have real handlers.
        """
        from .constants import (
            SERVICE_UUID,
            CHAR_DEVICE_IDENTITY,
            CHAR_FILE_COMMAND,
            CHAR_FILE_RESPONSE,
        )

        # Only register characteristics that have real implementations.
        # Handles budget: 1 (service) + 2 (identity) + 2 (file_cmd) + 3 (file_resp w/ CCCD) = 8
        char_defs = [
            (CHAR_DEVICE_IDENTITY,   _FLAG_READ),
            (CHAR_FILE_COMMAND,      _FLAG_WRITE),
            (CHAR_FILE_RESPONSE,     _FLAG_READ | _FLAG_NOTIFY),
        ]

        # Build the service tuple expected by MicroPython
        service_def = (
            bluetooth.UUID(SERVICE_UUID),
            [(bluetooth.UUID(uuid), flags) for uuid, flags in char_defs],
        )

        # Register; returns a list of lists of handles: [[h0, h1, ...]]
        ((h_dev_id, h_file_cmd, h_file_resp),) = \
            self._ble.gatts_register_services([service_def])

        # Store handles indexed by UUID string for easy lookup in IRQ handler
        self._handles = {
            CHAR_DEVICE_IDENTITY:   h_dev_id,
            CHAR_FILE_COMMAND:      h_file_cmd,
            CHAR_FILE_RESPONSE:     h_file_resp,
        }

        # Increase buffer size for file characteristics to handle larger payloads
        self._ble.gatts_set_buffer(h_file_cmd, 2048)
        self._ble.gatts_set_buffer(h_file_resp, 2048)

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
        print("[ble] gap_advertise(start) interval_us={}".format(interval_us))
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

            # Sanity check: detect overlapping sessions. If we believe we're
            # already connected when a new connect arrives, the previous
            # disconnect IRQ was either lost or never delivered — log loudly
            # so the bug is obvious instead of silent.
            if self._connected:
                print("[ble] WARNING: connect IRQ while already connected "
                      "(prev_handle={} new_handle={}); previous session was "
                      "not cleanly torn down".format(
                          self._conn_handle, conn_handle))

            self._session_id += 1
            self._connected   = True
            self._conn_handle = conn_handle
            addr_str = ":".join("{:02x}".format(b) for b in bytes(addr))
            print("[ble] Connected: session={} handle={} addr={}".format(
                self._session_id, conn_handle, addr_str))
            if self._on_connect_cb:
                self._on_connect_cb(conn_handle)

        elif event == _IRQ_CENTRAL_DISCONNECT:
            conn_handle, addr_type, addr = data

            # Sanity check: a disconnect for a handle other than the one we
            # think we're tracking means we have stale state somewhere.
            if self._conn_handle is not None and conn_handle != self._conn_handle:
                print("[ble] WARNING: disconnect for handle={} but we're "
                      "tracking handle={}".format(
                          conn_handle, self._conn_handle))

            print("[ble] Disconnected: session={} handle={}".format(
                self._session_id, conn_handle))

            self._connected   = False
            self._conn_handle = None

            # Clear the file-response characteristic value so a reconnecting
            # peer can't read a stale payload left over from the previous
            # session. Without this, iOS would re-subscribe, issue a new
            # file command, and race against a notify for a response that
            # was already sitting in the GATT attribute table — causing the
            # 20-second watchdog hang on the iOS side.
            from .constants import CHAR_FILE_RESPONSE
            h_resp = self._handles.get(CHAR_FILE_RESPONSE)
            if h_resp is not None:
                try:
                    self._ble.gatts_write(h_resp, b"")
                    print("[ble] Cleared file response characteristic "
                          "on disconnect")
                except Exception as e:
                    print("[ble] Failed to clear file response char: "
                          "{}".format(e))

            if self._on_disconnect_cb:
                try:
                    self._on_disconnect_cb(conn_handle)
                except Exception as e:
                    print("[ble] on_disconnect_cb error: {}".format(e))

            # Restart advertising so the next peer can find us
            self._advertise()

        elif event == _IRQ_GATTS_WRITE:
            conn_handle, attr_handle = data
            value = self._ble.gatts_read(attr_handle)
            uuid_str = self._handle_to_uuid.get(attr_handle, "unknown")
            print("[ble] Write: session={} handle={} char={} len={}".format(
                self._session_id, conn_handle,
                uuid_str[-8:] if uuid_str != "unknown" else "unknown",
                len(value)))

            # Defensive: a write should never arrive for a connection we
            # don't think we have. If it does, our connect/disconnect
            # bookkeeping is out of sync — log it but still try to serve
            # the request so we don't strand the client.
            if not self._connected:
                print("[ble] WARNING: write while not connected; "
                      "bookkeeping out of sync — accepting anyway")

            # Route file commands to the file handler
            from .constants import CHAR_FILE_COMMAND, CHAR_FILE_RESPONSE
            if uuid_str == CHAR_FILE_COMMAND and self._on_file_cmd_cb:
                try:
                    response = self._on_file_cmd_cb(bytes(value))
                    if response:
                        h_resp = self._handles.get(CHAR_FILE_RESPONSE)
                        if h_resp is not None:
                            self._ble.gatts_write(h_resp, response)
                            try:
                                self._ble.gatts_notify(conn_handle, h_resp)
                                print("[ble] File response sent "
                                      "({} bytes, handle={})".format(
                                          len(response), conn_handle))
                            except Exception as e:
                                # gatts_notify can fail if the central is
                                # not subscribed or the link has dropped.
                                # This is the smoking gun for "iOS thinks
                                # it's subscribed but the Pico can't push".
                                print("[ble] gatts_notify FAILED "
                                      "({} bytes, handle={}): {}".format(
                                          len(response), conn_handle, e))
                except Exception as e:
                    print("[ble] File command error: {}".format(e))
            elif self._on_write_cb:
                self._on_write_cb(conn_handle, uuid_str, bytes(value))

        elif event == _IRQ_GATTS_READ_REQUEST:
            # MicroPython calls this before sending a characteristic value.
            # We pre-populate all values with gatts_write(), so nothing to do
            # here for Phase 1.  Return 0 to allow the read.
            pass

        else:
            # Catch-all: log unknown IRQ events so newly-introduced ones
            # (e.g. MTU exchange, indicate complete) don't get silently
            # ignored during future debugging.
            print("[ble] IRQ event={} (unhandled)".format(event))
