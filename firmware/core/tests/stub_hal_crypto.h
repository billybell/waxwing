#ifndef STUB_HAL_CRYPTO_H
#define STUB_HAL_CRYPTO_H

// Number of times hal_random_bytes has been called since
// mock_hal_reset(). Lets tests assert "no RNG was used on this code path"
// (e.g. when loading an existing identity blob).
extern unsigned mock_hal_random_call_count;

// Reset the call counter and the deterministic byte sequence. Call from
// the start of every test that depends on either.
void mock_hal_reset(void);

#endif // STUB_HAL_CRYPTO_H
