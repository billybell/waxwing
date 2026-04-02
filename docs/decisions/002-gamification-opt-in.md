# ADR 002: Gamification and Social Layer as Opt-In Features

**Date:** 2026-04
**Status:** Accepted

## Context

The Waxwing Mesh protocol is designed for use in a range of environments, from casual community sharing to high-risk journalism and activism. These use cases have fundamentally different privacy requirements.

A gamification and social layer — encounter tracking, sync maps, peer graphs, milestone badges — is valuable for encouraging participation in low-risk environments. The same features become a liability in hostile environments: a seized node with a sync map is a movement record.

Two approaches were considered:

**Option A:** Make social features default-on, with opt-out.
Pro: Better discovery, easier onboarding, more engagement by default.
Con: Users in hostile environments must remember to disable features they may not know exist. A single failure to disable could compromise safety.

**Option B:** Make social features default-off, with opt-in.
Pro: Safe by default. Users in hostile environments are protected without needing to configure anything. Users who want the features can enable them explicitly.
Con: Lower out-of-the-box engagement. Features are less discoverable.

## Decision

All social and gamification features are **disabled by default**. Users opt into each feature independently via the companion app settings.

The companion app onboarding flow will present the social features as a group, explain what data each one stores, and allow the user to enable those they want. This gives engaged users easy access to the features while ensuring that the default state is maximally private.

## Consequences

**Positive:**
- A node running default settings stores no location data, no encounter history, and no attestation records. Seizure yields only the content library and transport identity.
- Users in hostile environments are protected without requiring special configuration.
- The explicit opt-in creates a moment of informed consent for each feature, reducing the chance that users are surprised by what data their node holds.

**Negative:**
- New users may not discover the social features unless the onboarding flow surfaces them clearly.
- The pass-along rate metric (useful for evaluating network health) will be unavailable for nodes that opt out of the encounter ledger.

## Notes on the Sync Attestation Feature

The sync attestation is unusual in that it is simultaneously a privacy risk (proves you were in proximity with a specific node at a specific time) and a privacy tool (allows a journalist to prove provenance of information without revealing the source's identity beyond a pseudonymous Transport Key).

This dual nature is why it is treated as an independent opt-in rather than being bundled with the general encounter ledger. A journalist may want attestation enabled but geolocation disabled, for example — producing a temporally-verified but locationally-ambiguous record.
