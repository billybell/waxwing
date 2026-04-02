#!/usr/bin/env python3
"""
Waxwing Mesh BLE Scanner

Scans for nearby Waxwing nodes, connects to them, and displays their
Device Identity and manifest contents. Useful for verifying that firmware
is advertising correctly and responding to GATT reads as the spec requires.

Usage:
    # Scan only — list nodes found in range
    python ble_scanner.py

    # Scan and connect to every found node, read Device Identity
    python ble_scanner.py --connect

    # Scan, connect, and read full manifest from every node
    python ble_scanner.py --connect --manifest

    # Connect to a specific device by address
    python ble_scanner.py --address AA:BB:CC:DD:EE:FF --manifest

    # Increase scan timeout
    python ble_scanner.py --timeout 20

Requirements:
    pip install bleak cbor2 cryptography
"""

import argparse
import asyncio
import base64
import sys
import time
from typing import Optional

import cbor2
from bleak import BleakClient, BleakScanner
from bleak.backends.device import BLEDevice
from bleak.backends.scanner import AdvertisementData

from waxwing.constants import (
    SERVICE_UUID,
    CHAR_DEVICE_IDENTITY,
    CHAR_MANIFEST_CHUNK,
    CHAR_TRANSFER_REQUEST,
    CHAR_REPUTATION_EXCHANGE,
    REQ_MANIFEST_READ,
    MANIFEST_HEADER_SIZE,
    ACTION_LABELS,
    caps_to_strings,
)
from waxwing.messages import (
    parse_device_identity,
    parse_manifest_payload,
    parse_file_metadata,
    build_manifest_read_request,
    verify_forwarding_declaration,
)
from waxwing.crypto import verify

# ---------------------------------------------------------------------------
# Formatting helpers
# ---------------------------------------------------------------------------

RESET  = "\033[0m"
BOLD   = "\033[1m"
GREEN  = "\033[32m"
YELLOW = "\033[33m"
CYAN   = "\033[36m"
RED    = "\033[31m"
DIM    = "\033[2m"

def fmt_tpk(tpk: bytes) -> str:
    """Format a Transport Public Key as a short hex prefix + base64url."""
    hex_prefix = tpk.hex()[:8].upper()
    b64        = base64.urlsafe_b64encode(tpk).decode().rstrip("=")
    return f"WX:{hex_prefix}  ({b64[:16]}…)"

def fmt_size(n: int) -> str:
    if n < 1024:       return f"{n} B"
    if n < 1_048_576:  return f"{n/1024:.1f} KB"
    return f"{n/1_048_576:.1f} MB"

def fmt_time(ts: int) -> str:
    try:
        return time.strftime("%Y-%m-%d %H:%M UTC", time.gmtime(ts))
    except Exception:
        return str(ts)

def separator(char: str = "─", width: int = 60) -> str:
    return DIM + char * width + RESET


# ---------------------------------------------------------------------------
# Scanning
# ---------------------------------------------------------------------------

async def scan_for_nodes(
    timeout: float,
    verbose: bool = False,
) -> list[tuple[BLEDevice, AdvertisementData]]:
    """
    Scan for BLE devices advertising the Waxwing service UUID.
    Returns a list of (device, advertisement_data) tuples.
    """
    found: list[tuple[BLEDevice, AdvertisementData]] = []
    seen_addresses: set[str] = set()

    def on_detection(device: BLEDevice, adv: AdvertisementData) -> None:
        if device.address in seen_addresses:
            return
        uuids = [str(u).upper() for u in (adv.service_uuids or [])]
        if SERVICE_UUID.upper() not in uuids:
            return
        seen_addresses.add(device.address)
        found.append((device, adv))
        rssi_bar = rssi_indicator(adv.rssi or -100)
        print(f"  {GREEN}▸{RESET} {BOLD}{device.name or '(unnamed)':<20}{RESET} "
              f"[{device.address}]  {rssi_bar}  RSSI {adv.rssi} dBm")
        if verbose and adv.service_data:
            for uuid, data in adv.service_data.items():
                print(f"    Service data [{uuid}]: {data.hex()}")

    print(f"\n{BOLD}Scanning for Waxwing nodes ({timeout:.0f}s)…{RESET}")
    print(separator())

    scanner = BleakScanner(detection_callback=on_detection, service_uuids=[SERVICE_UUID])
    await scanner.start()
    await asyncio.sleep(timeout)
    await scanner.stop()

    print(separator())
    if found:
        print(f"{GREEN}{BOLD}Found {len(found)} node(s).{RESET}\n")
    else:
        print(f"{YELLOW}No Waxwing nodes found in range.{RESET}")
        print(f"{DIM}Make sure your node is powered on and advertising.{RESET}\n")

    return found


def rssi_indicator(rssi: int) -> str:
    """Return a coloured signal strength bar."""
    bars = "█" * min(5, max(1, (rssi + 100) // 10))
    bars = bars.ljust(5, "░")
    if rssi > -60:   colour = GREEN
    elif rssi > -75: colour = YELLOW
    else:            colour = RED
    return colour + bars + RESET


# ---------------------------------------------------------------------------
# Node inspection
# ---------------------------------------------------------------------------

async def inspect_node(
    address:      str,
    read_manifest: bool = False,
    verbose:      bool  = False,
) -> None:
    """Connect to a node, read its Device Identity, and optionally its manifest."""
    print(f"{BOLD}Connecting to {address}…{RESET}")

    try:
        async with BleakClient(address, timeout=15.0) as client:
            print(f"{GREEN}Connected.{RESET}  MTU: {client.mtu_size} bytes\n")

            # --- Device Identity ---
            print(f"{BOLD}{CYAN}Device Identity{RESET}")
            print(separator("─", 40))
            try:
                raw      = await client.read_gatt_char(CHAR_DEVICE_IDENTITY)
                identity = parse_device_identity(raw)
                print_device_identity(identity, verbose=verbose)
            except Exception as e:
                print(f"{RED}Failed to read Device Identity: {e}{RESET}")
                return

            # --- Manifest ---
            manifest_count = identity.get("manifest_count", 0)
            if read_manifest:
                print(f"\n{BOLD}{CYAN}Manifest  ({manifest_count} item(s) reported){RESET}")
                print(separator("─", 40))
                if manifest_count == 0:
                    print(f"{DIM}Manifest is empty.{RESET}")
                else:
                    try:
                        manifest = await read_full_manifest(client, verbose=verbose)
                        print_manifest(manifest, verbose=verbose)
                    except Exception as e:
                        print(f"{RED}Failed to read manifest: {e}{RESET}")
            else:
                print(f"\n{DIM}(Use --manifest to read {manifest_count} manifest item(s)){RESET}")

    except asyncio.TimeoutError:
        print(f"{RED}Connection timed out. Is the node in range and accepting connections?{RESET}")
    except Exception as e:
        print(f"{RED}Connection failed: {e}{RESET}")


async def read_full_manifest(client: BleakClient, verbose: bool = False) -> list[dict]:
    """
    Read the complete manifest from a connected node using the chunked read protocol.
    See PROTOCOL.md §6 for the exchange sequence.
    """
    # Step 1: set read offset to 0
    await client.write_gatt_char(
        CHAR_TRANSFER_REQUEST,
        build_manifest_read_request(0),
        response=True,
    )

    # Step 2: read chunks until we have the full payload
    full_data    = bytearray()
    total_length = None
    chunk_count  = 0

    while True:
        chunk = await client.read_gatt_char(CHAR_MANIFEST_CHUNK)
        chunk_count += 1

        if total_length is None:
            if len(chunk) < MANIFEST_HEADER_SIZE:
                raise ValueError(f"First manifest chunk too short: {len(chunk)} bytes")
            total_length = int.from_bytes(chunk[:MANIFEST_HEADER_SIZE], "little")
            full_data.extend(chunk[MANIFEST_HEADER_SIZE:])
            if verbose:
                print(f"  {DIM}Manifest total size: {fmt_size(total_length)}{RESET}")
        else:
            full_data.extend(chunk)

        if verbose:
            pct = min(100, int(len(full_data) / max(1, total_length) * 100))
            print(f"  {DIM}Reading manifest… {pct}% ({len(full_data)}/{total_length} bytes, "
                  f"{chunk_count} chunks){RESET}", end="\r")

        if len(full_data) >= total_length:
            break

        # Set offset for next chunk
        await client.write_gatt_char(
            CHAR_TRANSFER_REQUEST,
            build_manifest_read_request(len(full_data)),
            response=True,
        )

    if verbose:
        print()  # newline after progress line

    return parse_manifest_payload(bytes(full_data[:total_length]))


# ---------------------------------------------------------------------------
# Pretty-printing
# ---------------------------------------------------------------------------

def print_device_identity(identity: dict, verbose: bool = False) -> None:
    tpk      = identity.get("tpk", b"")
    attended = identity.get("attended", True)
    caps     = identity.get("caps", 0)
    mode     = identity.get("unattended_mode", "")
    name     = identity.get("name", "(unnamed)")

    node_type = "Attended" if attended else f"Unattended — {mode or 'relay'}"

    print(f"  {'Name':<18} {BOLD}{name}{RESET}")
    print(f"  {'Transport key':<18} {fmt_tpk(tpk)}")
    print(f"  {'Protocol version':<18} {identity.get('v', '?')}")
    print(f"  {'Node type':<18} {node_type}")

    cap_list = caps_to_strings(caps)
    print(f"  {'Capabilities':<18} {', '.join(cap_list) if cap_list else 'BLE only'}")
    print(f"  {'Manifest items':<18} {identity.get('manifest_count', 0)}")

    if verbose:
        print(f"  {'Raw caps':<18} 0x{caps:02X}")
        print(f"  {'Raw CBOR size':<18} (shown above)")


def print_manifest(manifest: list[dict], verbose: bool = False) -> None:
    if not manifest:
        print(f"{DIM}  (empty){RESET}")
        return

    for i, item in enumerate(manifest):
        file_id   = item.get("id", b"")
        id_str    = file_id.hex()[:16] + "…" if file_id else "?"
        mime      = item.get("mime", "?")
        size      = item.get("size", 0)
        title     = item.get("title") or DIM + "(untitled)" + RESET
        tags      = item.get("tags", [])
        routing   = item.get("routing", "broadcast")
        ttl       = item.get("ttl", 0)
        ts        = item.get("timestamp", 0)
        has_cpk   = item.get("cpk") is not None
        fwd_decl  = item.get("fwd_decl")

        # Signature validity
        sig_status = DIM + "anonymous" + RESET
        if has_cpk:
            sig_status = YELLOW + "signed (unverified)" + RESET

        # Forwarding declaration
        fwd_status = DIM + "none" + RESET
        if fwd_decl:
            action_label = ACTION_LABELS.get(fwd_decl.get("action"), "?")
            fwd_tpk_str  = fwd_decl.get("fwd_tpk", b"").hex()[:8] + "…"
            sig_ok       = verify_forwarding_declaration(fwd_decl, file_id)
            sig_mark     = f"{GREEN}✓{RESET}" if sig_ok else f"{RED}✗{RESET}"
            fwd_status   = f"{action_label} by WX:{fwd_tpk_str.upper()} {sig_mark}"

        print(f"\n  {BOLD}[{i+1}]{RESET} {title}")
        print(f"       ID       {id_str}")
        print(f"       Created  {fmt_time(ts)}")
        print(f"       Type     {mime}  {fmt_size(size)}")
        print(f"       TTL      {ttl}  Routing: {routing}")
        if tags:
            print(f"       Tags     {', '.join(tags)}")
        print(f"       Creator  {sig_status}")
        print(f"       Vouched  {fwd_status}")

        if verbose and item.get("recipient"):
            print(f"       Recipient  {item['recipient'].hex()[:16]}…")

    print()


# ---------------------------------------------------------------------------
# Entry point
# ---------------------------------------------------------------------------

def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Waxwing Mesh BLE Scanner — find and inspect nearby nodes",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Examples:
  python ble_scanner.py                    # Scan for 10 seconds
  python ble_scanner.py --connect          # Scan and read Device Identity
  python ble_scanner.py --connect --manifest   # Also read manifests
  python ble_scanner.py --address DE:AD:BE:EF:00:01 --manifest
  python ble_scanner.py --timeout 30 --verbose
        """,
    )
    parser.add_argument("--timeout",  type=float, default=10.0,
                        help="BLE scan duration in seconds (default: 10)")
    parser.add_argument("--address",  type=str,   default=None,
                        help="Connect to a specific device by BLE address")
    parser.add_argument("--connect",  action="store_true",
                        help="Connect to all found nodes and read Device Identity")
    parser.add_argument("--manifest", action="store_true",
                        help="Read full manifest from connected nodes (implies --connect)")
    parser.add_argument("--verbose",  action="store_true",
                        help="Show additional diagnostic output")
    return parser.parse_args()


async def main() -> None:
    args = parse_args()

    print(f"\n{BOLD}Waxwing Mesh BLE Scanner{RESET}  "
          f"{DIM}(github.com/billybell/waxwing){RESET}")
    print(f"Service UUID: {DIM}{SERVICE_UUID}{RESET}\n")

    if args.address:
        # Skip scan; connect directly to the given address
        await inspect_node(
            address       = args.address,
            read_manifest = args.manifest,
            verbose       = args.verbose,
        )
        return

    # Scan
    found = await scan_for_nodes(timeout=args.timeout, verbose=args.verbose)

    if not found:
        return

    # Connect if requested
    if args.connect or args.manifest:
        for device, _ in found:
            print(separator("═"))
            await inspect_node(
                address       = device.address,
                read_manifest = args.manifest,
                verbose       = args.verbose,
            )
    else:
        print(f"{DIM}Run with --connect to read Device Identity from these nodes.{RESET}")
        print(f"{DIM}Run with --connect --manifest to also read their content manifests.{RESET}")


if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        print(f"\n{YELLOW}Scan interrupted.{RESET}")
        sys.exit(0)
