// Known-answer tests for the vendored SHA-256, plus a streaming check
// that splitting an input across many sha256_update() calls gives the
// same digest as a single-shot call.

#include "test.h"
#include "core/thirdparty/sha256/sha256.h"

#include <stdint.h>
#include <string.h>

static int hex_eq(const uint8_t got[32], const char *expected_hex) {
    char buf[65];
    static const char hex[] = "0123456789abcdef";
    for (int i = 0; i < 32; i++) {
        buf[2*i + 0] = hex[(got[i] >> 4) & 0xf];
        buf[2*i + 1] = hex[got[i] & 0xf];
    }
    buf[64] = '\0';
    return strcmp(buf, expected_hex) == 0;
}

void test_sha256_empty_string(void) {
    uint8_t digest[32];
    sha256((const uint8_t *)"", 0, digest);
    TEST_ASSERT(hex_eq(digest,
        "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"),
        "SHA-256(\"\") matches FIPS 180-2");
}

void test_sha256_abc(void) {
    uint8_t digest[32];
    sha256((const uint8_t *)"abc", 3, digest);
    TEST_ASSERT(hex_eq(digest,
        "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"),
        "SHA-256(\"abc\") matches FIPS 180-2");
}

void test_sha256_long_string(void) {
    // FIPS 180-2 vector: 448-bit message exercising the multi-block path.
    const char *msg =
        "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    uint8_t digest[32];
    sha256((const uint8_t *)msg, strlen(msg), digest);
    TEST_ASSERT(hex_eq(digest,
        "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"),
        "SHA-256 multi-block vector");
}

void test_sha256_streaming_matches_oneshot(void) {
    // Hash a 1000-byte buffer in many oddly-sized chunks; the result must
    // match the one-shot digest. Exercises the buffer top-up path in
    // sha256_update().
    uint8_t buf[1000];
    for (size_t i = 0; i < sizeof(buf); i++) {
        buf[i] = (uint8_t)(i * 31 + 7);
    }

    uint8_t one_shot[32];
    sha256(buf, sizeof(buf), one_shot);

    sha256_ctx ctx;
    sha256_init(&ctx);
    size_t off = 0;
    size_t step = 1;
    while (off < sizeof(buf)) {
        size_t take = step;
        if (off + take > sizeof(buf)) take = sizeof(buf) - off;
        sha256_update(&ctx, buf + off, take);
        off += take;
        step = (step * 7 + 1) % 137; // 1, 8, 57, 39, 18, 127, 121, ...
        if (step == 0) step = 1;
    }
    uint8_t streamed[32];
    sha256_final(&ctx, streamed);

    TEST_ASSERT(memcmp(one_shot, streamed, 32) == 0,
                "streaming sha256 matches one-shot");
}

void test_sha256_million_a(void) {
    // FIPS 180-2 vector: SHA-256 of 1,000,000 'a' bytes.
    sha256_ctx ctx;
    sha256_init(&ctx);
    uint8_t chunk[1000];
    memset(chunk, 'a', sizeof(chunk));
    for (int i = 0; i < 1000; i++) {
        sha256_update(&ctx, chunk, sizeof(chunk));
    }
    uint8_t digest[32];
    sha256_final(&ctx, digest);
    TEST_ASSERT(hex_eq(digest,
        "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0"),
        "SHA-256 of 1,000,000 'a' chars matches FIPS 180-2");
}
