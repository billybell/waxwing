# BIP-39 Wordlist Resource

`BIP39.swift` loads the standard 2048-word English BIP-39 wordlist from a
bundle resource named `bip39-english.txt`. The Waxwing companion app **will
not generate or restore Content Identities until this file is present in the
app bundle.**

## Adding the wordlist

1. Download the official wordlist from the BIP-0039 specification:

       https://github.com/bitcoin/bips/blob/master/bip-0039/english.txt

   The file MUST contain exactly 2048 lines, one lowercase ASCII word per
   line, in alphabetical order, ending with a trailing newline.

2. Verify the SHA-256 of the file matches the canonical value published with
   the BIP-0039 spec. (As of this writing, the file is fixed and has not been
   updated since adoption.)

3. Drop `bip39-english.txt` into this `Crypto/Resources/` folder.

4. In Xcode: add the file to the `WaxwingCompanion` target so it gets copied
   into the app bundle's `Resources/` directory. Confirm "Target Membership"
   includes the main app target.

5. Build and run. `BIP39.isWordlistLoaded` should return `true`.

## Why isn't it checked in?

We deliberately don't vendor the wordlist into version control so there is
exactly one source of truth (the BIP-0039 spec) and so accidental edits to
the file are noticed at integration time. Replacing it is a one-time setup
step per fresh checkout.
