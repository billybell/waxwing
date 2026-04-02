# ADR 001: Device-First Architecture

**Date:** 2026-04
**Status:** Accepted

## Context

The original conception of Waxwing Mesh had mobile phones (iOS/Android) participating directly as mesh nodes — advertising via BLE, scanning for peers, and exchanging content in the background. During design review, several problems with this approach emerged:

**iOS background BLE limitations.** iOS severely restricts background BLE operation. Peripherals advertising custom service UUIDs in the background have their UUIDs moved to an "overflow area" not visible to non-Apple devices. Background scanning is duty-cycled and unreliable. These restrictions would make iPhones effectively invisible to microcontroller nodes when the app is not in the foreground.

**Privacy exposure.** If a user's phone participates in the mesh, its transport identity is linked to a device that is personally identifiable — registered to an Apple ID, associated with a phone number, physically carried by a specific person. Authorities who correlate BLE advertisements with location data could track individuals.

**Battery and resource constraints.** Continuous BLE scanning and advertising on a smartphone drains battery significantly. Users would disable the feature.

**Platform unpredictability.** Android BLE behaviour varies widely across manufacturers and OS versions. Building a reliable mesh on phone BLE is a support burden.

## Decision

Phones are **companion apps only**. The mesh is made of dedicated hardware nodes. A companion app connects to its single paired node — not to the mesh at large.

## Consequences

**Positive:**
- iOS background BLE problem disappears entirely. The companion app only needs to find its paired node (a known device), which is trivial with a registered service UUID.
- Privacy is dramatically improved. A seized phone yields minimal mesh-relevant information. The node (a cheap Pico W) can be discarded, replaced, and its transport identity rotated without losing the user's content identity.
- Battery life on phones is unaffected.
- The mesh runs continuously even when the user's phone is off, locked, or out of range.

**Negative:**
- Users must acquire and carry a separate hardware device. This is a barrier to adoption.
- The ecosystem is more complex to explain and onboard.

**Mitigations:**
- The Pico W node is inexpensive (< $10 USD). A node kit (Pico W + SD card + small LiPo + case) can be produced cheaply.
- Long-term, a phone-as-node mode may be added for Android (which has fewer background BLE restrictions) as an optional, lower-capability participation mode.
