# Waxwing

## When preparing a build for end-to-end testing

**Always run unit tests first.** `firmware/core/build-host/waxwing_test` must pass before building firmware or iOS. If the test suite fails, fix it and re-run until green — never ship e2e tests to the user when unit tests are already red.

## Before committing code (required)

Every fix or feature must pass all three checks before staging anything to git:

1. **Run unit tests** — `cd firmware/core && cmake -S . -B build-host && cmake --build build-host && ./build-host/waxwing_test`
2. **Build firmware** — `cd firmware/pico-w && bash build.sh` (Pico W) and `cd firmware/cardputer && bash build.sh` (CardPuter)
3. **Build iOS project** — verify the Swift code compiles (if a buildable Xcode project exists)

These rules exist to protect the code from me, not to protect me from bad code. Commit without passing all three is unacceptable.

## Tool usage notes

- Never use `cat -A`, `cat -v`, or `cat -T` — these are GNU flags that fail with BSD cat on macOS. Use the `Read` tool instead.

## Building firmware

```bash
cd firmware/pico-w
bash build.sh
# Output: build/waxwing_mesh.uf2 (Pico W flash target)
```
