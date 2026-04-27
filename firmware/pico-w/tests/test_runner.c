#include "test.h"

int test_failures = 0;

// Forward declarations for test functions.
extern void test_ls_empty(void);
extern void test_ls_with_files(void);
extern void test_ls_pagination(void);
extern void test_storage_info(void);
extern void test_read_existing_file(void);
extern void test_read_missing_file(void);
extern void test_write_and_read_roundtrip(void);
extern void test_delete(void);
extern void test_delete_missing(void);
extern void test_unknown_command(void);
extern void test_missing_name_field(void);
extern void test_chunked_write_roundtrip(void);
extern void test_chunked_write_size_mismatch(void);
extern void test_read_start_chunk_read(void);
extern void test_write_meta_read_meta(void);
extern void test_read_meta_missing_sidecar(void);
extern void test_storage_info_fields(void);
extern void test_write_then_ls(void);
extern void test_cbor_parse_floats(void);
extern void test_write_meta_with_doubles_roundtrip(void);
extern void test_read_chunk_past_eof(void);

extern void test_identity_generate_fresh(void);
extern void test_identity_persists_across_load(void);
extern void test_identity_load_existing(void);
extern void test_identity_corrupted_pub_regenerates(void);
extern void test_identity_wrong_magic_regenerates(void);
extern void test_identity_wrong_version_regenerates(void);
extern void test_identity_short_blob_regenerates(void);
extern void test_identity_node_name_format(void);
extern void test_identity_not_in_user_files(void);
extern void test_identity_hex_encoding(void);
extern void test_identity_b64url_encoding(void);

extern void test_counter_starts_at_zero(void);
extern void test_counter_persists(void);
extern void test_counter_wraps_at_256(void);
extern void test_counter_bump_writes_through(void);
extern void test_counter_load_corrupted_blob(void);
extern void test_counter_init_idempotent(void);

extern void test_peer_unknown_connects(void);
extern void test_peer_version_changed_connects(void);
extern void test_peer_caught_up_skips_within_window(void);
extern void test_peer_caught_up_connects_after_window(void);
extern void test_peer_failed_uses_short_backoff(void);
extern void test_peer_wraparound_inequality_triggers_sync(void);
extern void test_peer_lru_evicts_oldest(void);
extern void test_peer_record_updates_in_place(void);
extern void test_peer_distinct_tpks_isolated(void);
extern void test_peer_init_clears(void);

int main(void) {
    TEST_RUN(test_ls_empty);
    TEST_RUN(test_ls_with_files);
    TEST_RUN(test_ls_pagination);
    TEST_RUN(test_storage_info);
    TEST_RUN(test_read_existing_file);
    TEST_RUN(test_read_missing_file);
    TEST_RUN(test_write_and_read_roundtrip);
    TEST_RUN(test_delete);
    TEST_RUN(test_delete_missing);
    TEST_RUN(test_unknown_command);
    TEST_RUN(test_missing_name_field);
    TEST_RUN(test_chunked_write_roundtrip);
    TEST_RUN(test_chunked_write_size_mismatch);
    TEST_RUN(test_read_start_chunk_read);
    TEST_RUN(test_write_meta_read_meta);
    TEST_RUN(test_read_meta_missing_sidecar);
    TEST_RUN(test_storage_info_fields);
    TEST_RUN(test_write_then_ls);
    TEST_RUN(test_cbor_parse_floats);
    TEST_RUN(test_write_meta_with_doubles_roundtrip);
    TEST_RUN(test_read_chunk_past_eof);

    TEST_RUN(test_identity_generate_fresh);
    TEST_RUN(test_identity_persists_across_load);
    TEST_RUN(test_identity_load_existing);
    TEST_RUN(test_identity_corrupted_pub_regenerates);
    TEST_RUN(test_identity_wrong_magic_regenerates);
    TEST_RUN(test_identity_wrong_version_regenerates);
    TEST_RUN(test_identity_short_blob_regenerates);
    TEST_RUN(test_identity_node_name_format);
    TEST_RUN(test_identity_not_in_user_files);
    TEST_RUN(test_identity_hex_encoding);
    TEST_RUN(test_identity_b64url_encoding);

    TEST_RUN(test_counter_starts_at_zero);
    TEST_RUN(test_counter_persists);
    TEST_RUN(test_counter_wraps_at_256);
    TEST_RUN(test_counter_bump_writes_through);
    TEST_RUN(test_counter_load_corrupted_blob);
    TEST_RUN(test_counter_init_idempotent);

    TEST_RUN(test_peer_unknown_connects);
    TEST_RUN(test_peer_version_changed_connects);
    TEST_RUN(test_peer_caught_up_skips_within_window);
    TEST_RUN(test_peer_caught_up_connects_after_window);
    TEST_RUN(test_peer_failed_uses_short_backoff);
    TEST_RUN(test_peer_wraparound_inequality_triggers_sync);
    TEST_RUN(test_peer_lru_evicts_oldest);
    TEST_RUN(test_peer_record_updates_in_place);
    TEST_RUN(test_peer_distinct_tpks_isolated);
    TEST_RUN(test_peer_init_clears);

    printf("\n%d test(s) ran. %d assertion(s) failed.\n", 48, test_failures);
    return test_failures;
}
