#!/usr/bin/env bash
# Build the CardPuter firmware. Mirrors firmware/pico-w/build.sh ergonomics.
set -euo pipefail

cd "$(dirname "$0")"
pio run "$@"
