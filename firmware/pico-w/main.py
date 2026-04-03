# main.py — Waxwing Mesh firmware entry point (Pico W / MicroPython)
#
# Boot sequence:
#   1. Load (or generate + persist) Transport Identity
#   2. Build Device Identity CBOR payload
#   3. Register GATT service and start BLE advertising
#   4. Enter main loop: LED heartbeat + BLE event processing
#
# LED pattern:
#   Idle (advertising)  : slow blink, 1 s on / 1 s off
#   Connected           : fast blink, 100 ms on / 100 ms off
#   Error               : 3 rapid flashes then 2 s pause

import time
import machine

from waxwing import identity as id_module
from waxwing import messages
from waxwing.ble import WaxwingBLE

# ---------------------------------------------------------------------------
# LED helpers
# ---------------------------------------------------------------------------
_led = machine.Pin("LED", machine.Pin.OUT)


def _led_error():
    """Three rapid flashes to signal a fatal error."""
    for _ in range(3):
        _led.on()
        time.sleep_ms(100)
        _led.off()
        time.sleep_ms(100)


# ---------------------------------------------------------------------------
# Application state
# ---------------------------------------------------------------------------
_connected = False


def _on_connect(conn_handle):
    global _connected
    _connected = True
    print("[main] Peer connected (handle={})".format(conn_handle))


def _on_disconnect(conn_handle):
    global _connected
    _connected = False
    print("[main] Peer disconnected (handle={})".format(conn_handle))


def _on_write(conn_handle, char_uuid, data):
    """
    Handle incoming writes from a connected peer.
    Phase 1: log only.  Phase 2+ will route to transfer/rating handlers.
    """
    print("[main] Write to {} ({} bytes): {}".format(
        char_uuid[-8:], len(data), data[:16]))


# ---------------------------------------------------------------------------
# Main entry point
# ---------------------------------------------------------------------------

def main():
    global _connected

    print("\n=== Waxwing Mesh Firmware ===")
    print("Loading identity...")

    # -- Step 1: Transport Identity --
    try:
        identity = id_module.load_or_generate()
    except Exception as e:
        print("[main] FATAL: identity init failed: {}".format(e))
        while True:
            _led_error()
            time.sleep(2)

    print("[main] Node name : {}".format(identity["node_name"]))
    print("[main] TPK       : {}...".format(identity["tpk_hex"][:16]))

    # -- Step 2 + 3: BLE GATT server --
    try:
        ble = WaxwingBLE(identity, messages)
        ble.on_connect(_on_connect)
        ble.on_disconnect(_on_disconnect)
        ble.on_write(_on_write)
        ble.start()
    except Exception as e:
        print("[main] FATAL: BLE init failed: {}".format(e))
        while True:
            _led_error()
            time.sleep(2)

    print("[main] Ready — entering main loop")

    # -- Step 4: Main loop --
    last_toggle_ms = time.ticks_ms()
    led_state      = False

    while True:
        now = time.ticks_ms()
        blink_interval = 100 if _connected else 1000

        if time.ticks_diff(now, last_toggle_ms) >= blink_interval:
            led_state = not led_state
            _led.value(led_state)
            last_toggle_ms = now

        # Give BLE stack a chance to process events
        ble.tick()

        # Small yield to avoid starving the BLE IRQ handler
        time.sleep_ms(10)


# ---------------------------------------------------------------------------
# Run
# ---------------------------------------------------------------------------
try:
    main()
except Exception as e:
    print("[main] Unhandled exception: {}".format(e))
    import sys
    sys.print_exception(e)
    # Blink SOS then reset
    for _ in range(3):
        _led_error()
        time.sleep_ms(500)
    machine.reset()
