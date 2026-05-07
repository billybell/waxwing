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
extern void test_delete_does_not_bump_manifest(void);
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

extern void test_sha256_empty_string(void);
extern void test_sha256_abc(void);
extern void test_sha256_long_string(void);
extern void test_sha256_streaming_matches_oneshot(void);
extern void test_sha256_million_a(void);

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

extern void test_meeting_count_starts_at_zero(void);
extern void test_meeting_count_bump_returns_new_value(void);
extern void test_meeting_count_persists_across_init(void);
extern void test_meeting_count_corrupted_blob_resets(void);
extern void test_meeting_count_does_not_wrap_uint64_max(void);
extern void test_meeting_count_persisted_bytes_are_le(void);

extern void test_peer_ledger_starts_empty(void);
extern void test_peer_ledger_first_contact_creates_entry(void);
extern void test_peer_ledger_deltas_accumulate(void);
extern void test_peer_ledger_meeting_count_does_not_decrease(void);
extern void test_peer_ledger_persists_across_init(void);
extern void test_peer_ledger_eviction_picks_lowest_meeting_count(void);
extern void test_peer_ledger_set_rep_creates_entry(void);
extern void test_peer_ledger_get_zeroes_out_on_miss(void);
extern void test_peer_ledger_get_by_prefix_hits_first_8_bytes(void);
extern void test_peer_ledger_get_by_prefix_misses_unknown(void);
extern void test_peer_ledger_corrupted_blob_resets(void);

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

extern void test_mesh_starts_in_grace(void);
extern void test_mesh_grace_expires_to_advertising(void);
extern void test_mesh_grace_expires_to_scanning(void);
extern void test_mesh_dwell_within_bounds(void);
extern void test_mesh_alternates_or_repeats(void);
extern void test_mesh_companion_freezes_schedule(void);
extern void test_mesh_grace_skipped_if_companion_connects_immediately(void);
extern void test_mesh_jitter_breaks_lockstep(void);

extern void test_peer_sync_empty_peer(void);
extern void test_peer_sync_single_file_full_pull(void);
extern void test_peer_sync_multi_chunk_pull(void);
extern void test_peer_sync_pagination(void);
extern void test_peer_sync_dedup_by_name(void);
extern void test_peer_sync_dedup_skips_through_to_unique(void);
extern void test_peer_sync_peer_error_mid_chunk(void);
extern void test_peer_sync_read_start_not_found_skips(void);
extern void test_peer_sync_meta_error_is_best_effort(void);
extern void test_peer_sync_end_aborts_in_flight(void);
extern void test_peer_sync_only_one_session_at_a_time(void);
extern void test_peer_sync_envelope_fits_mtu(void);

extern void test_ssid_scan_init_empty(void);
extern void test_ssid_scan_random_bssid_filtered(void);
extern void test_ssid_scan_add_basic(void);
extern void test_ssid_scan_dedup_updates_in_place(void);
extern void test_ssid_scan_full_replaces_weakest(void);
extern void test_ssid_scan_full_rejects_weaker(void);
extern void test_ssid_scan_sort_by_rssi(void);
extern void test_ssid_scan_sort_stable_on_empty(void);
extern void test_ssid_scan_bssids_differ(void);
extern void test_ssid_scan_long_ssid_truncated(void);
extern void test_ssid_scan_hidden_ssid(void);

extern void test_peer_gate_companion_only_write_rejected(void);
extern void test_peer_gate_companion_only_delete_rejected(void);
extern void test_peer_gate_companion_only_scan_get_rejected(void);
extern void test_peer_gate_companion_only_encounters_get_rejected(void);
extern void test_peer_gate_companion_only_attestation_write_rejected(void);
extern void test_peer_gate_companion_only_attestations_get_rejected(void);
extern void test_peer_gate_ls_allowed(void);
extern void test_peer_gate_storage_info_allowed(void);
extern void test_peer_gate_unknown_cmd_still_unknown(void);
extern void test_peer_gate_companion_mode_unchanged(void);

extern void test_encounter_record_body_byte_for_byte_reproducible(void);
extern void test_encounter_record_full_round_trip(void);
extern void test_encounter_record_verify_happy_path(void);
extern void test_encounter_record_verify_rejects_tampered_field(void);
extern void test_encounter_record_verify_rejects_tampered_sig(void);
extern void test_encounter_record_verify_rejects_swapped_sigs(void);
extern void test_encounter_record_decode_rejects_wrong_version(void);
extern void test_encounter_record_decode_rejects_oversized_bssids(void);
extern void test_encounter_record_id_is_side_symmetric(void);
extern void test_encounter_record_id_changes_with_nonce(void);
extern void test_encounter_record_decode_rejects_truncated(void);
extern void test_encounter_record_empty_bssids_allowed(void);

extern void test_encounter_session_happy_path(void);
extern void test_encounter_session_id_agrees_between_sides(void);
extern void test_encounter_session_initiator_rejects_wrong_prefix(void);
extern void test_encounter_session_responder_rejects_wrong_prefix(void);
extern void test_encounter_session_initiator_rejects_bad_sig_b(void);
extern void test_encounter_session_responder_rejects_bad_sig_a(void);
extern void test_encounter_session_initiator_drop_after_propose(void);
extern void test_encounter_session_responder_drop_after_accept(void);
extern void test_encounter_session_responder_rejects_propose_wrong_type(void);
extern void test_encounter_session_handle_before_start_is_error(void);
extern void test_encounter_session_responder_late_bind_populates_b_side(void);
extern void test_encounter_session_responder_late_bind_miss_keeps_zeros(void);
extern void test_encounter_session_responder_late_bind_runs_after_prefix_check(void);
extern void test_encounter_session_first_contact_with_late_bind_set(void);

extern void test_encounters_init_empty(void);
extern void test_encounters_record_one(void);
extern void test_encounters_iter_oldest_first(void);
extern void test_encounters_iter_order_preserved(void);
extern void test_encounters_ring_evicts_oldest(void);
extern void test_encounters_persists_across_init(void);
extern void test_encounters_should_record_first(void);
extern void test_encounters_should_record_dedup_same_set(void);
extern void test_encounters_should_record_dedup_diff_set(void);
extern void test_encounters_should_record_age_bypass(void);
extern void test_encounters_record_signs_with_seed(void);
extern void test_encounters_empty_scan_rejected(void);

extern void test_scan_get_no_scan_yet(void);
extern void test_scan_get_returns_observations(void);
extern void test_scan_get_paginates_via_next_offset(void);
extern void test_encounters_get_empty(void);
extern void test_encounters_get_returns_records(void);
extern void test_encounters_get_since_filter(void);
extern void test_encounters_get_paginates_via_next_offset(void);

extern void test_attestations_init_empty(void);
extern void test_attestations_write_one(void);
extern void test_attestations_too_big_rejected(void);
extern void test_attestations_iter_oldest_first(void);
extern void test_attestations_ring_evicts_oldest(void);
extern void test_attestations_persist_across_init(void);

extern void test_cmd_attestation_write_round_trip(void);
extern void test_cmd_attestation_write_missing_blob(void);

extern void test_attest_cache_init_empty(void);
extern void test_attest_cache_write_one(void);
extern void test_attest_cache_dedup_returns_zero(void);
extern void test_attest_cache_too_big_rejected(void);
extern void test_attest_cache_iter_oldest_first(void);
extern void test_attest_cache_ring_evicts_oldest(void);
extern void test_attest_cache_dedup_after_eviction(void);
extern void test_attest_cache_persist_across_init(void);

extern void test_cmd_attest_query_match_round_trip(void);
extern void test_cmd_attest_query_no_match_returns_empty(void);
extern void test_cmd_attest_query_paginates(void);
extern void test_cmd_attest_query_missing_bssids(void);
extern void test_cmd_attest_ingest_round_trip(void);
extern void test_cmd_attest_ingest_dedup_counts(void);
extern void test_cmd_attest_ingest_missing_blobs(void);

extern void test_attest_extract_bssids_basic(void);
extern void test_attest_extract_bssids_caps_at_max(void);
extern void test_attest_extract_bssids_skips_wrong_size(void);
extern void test_attest_extract_bssids_missing_key4(void);
extern void test_attest_extract_bssids_truncated_blob(void);
extern void test_attest_match_any_bssid_hit(void);
extern void test_attest_match_any_bssid_miss(void);
extern void test_attest_query_for_each_finds_self_records(void);
extern void test_attest_query_for_each_finds_cache_records(void);
extern void test_attest_query_for_each_filters_non_matching(void);
extern void test_attest_query_for_each_early_stops(void);
extern void test_attest_query_for_each_empty_query(void);

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
    TEST_RUN(test_delete_does_not_bump_manifest);
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

    TEST_RUN(test_sha256_empty_string);
    TEST_RUN(test_sha256_abc);
    TEST_RUN(test_sha256_long_string);
    TEST_RUN(test_sha256_streaming_matches_oneshot);
    TEST_RUN(test_sha256_million_a);

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

    TEST_RUN(test_meeting_count_starts_at_zero);
    TEST_RUN(test_meeting_count_bump_returns_new_value);
    TEST_RUN(test_meeting_count_persists_across_init);
    TEST_RUN(test_meeting_count_corrupted_blob_resets);
    TEST_RUN(test_meeting_count_does_not_wrap_uint64_max);
    TEST_RUN(test_meeting_count_persisted_bytes_are_le);

    TEST_RUN(test_peer_ledger_starts_empty);
    TEST_RUN(test_peer_ledger_first_contact_creates_entry);
    TEST_RUN(test_peer_ledger_deltas_accumulate);
    TEST_RUN(test_peer_ledger_meeting_count_does_not_decrease);
    TEST_RUN(test_peer_ledger_persists_across_init);
    TEST_RUN(test_peer_ledger_eviction_picks_lowest_meeting_count);
    TEST_RUN(test_peer_ledger_set_rep_creates_entry);
    TEST_RUN(test_peer_ledger_get_zeroes_out_on_miss);
    TEST_RUN(test_peer_ledger_get_by_prefix_hits_first_8_bytes);
    TEST_RUN(test_peer_ledger_get_by_prefix_misses_unknown);
    TEST_RUN(test_peer_ledger_corrupted_blob_resets);

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

    TEST_RUN(test_mesh_starts_in_grace);
    TEST_RUN(test_mesh_grace_expires_to_advertising);
    TEST_RUN(test_mesh_grace_expires_to_scanning);
    TEST_RUN(test_mesh_dwell_within_bounds);
    TEST_RUN(test_mesh_alternates_or_repeats);
    TEST_RUN(test_mesh_companion_freezes_schedule);
    TEST_RUN(test_mesh_grace_skipped_if_companion_connects_immediately);
    TEST_RUN(test_mesh_jitter_breaks_lockstep);

    TEST_RUN(test_peer_sync_empty_peer);
    TEST_RUN(test_peer_sync_single_file_full_pull);
    TEST_RUN(test_peer_sync_multi_chunk_pull);
    TEST_RUN(test_peer_sync_pagination);
    TEST_RUN(test_peer_sync_dedup_by_name);
    TEST_RUN(test_peer_sync_dedup_skips_through_to_unique);
    TEST_RUN(test_peer_sync_peer_error_mid_chunk);
    TEST_RUN(test_peer_sync_read_start_not_found_skips);
    TEST_RUN(test_peer_sync_meta_error_is_best_effort);
    TEST_RUN(test_peer_sync_end_aborts_in_flight);
    TEST_RUN(test_peer_sync_only_one_session_at_a_time);
    TEST_RUN(test_peer_sync_envelope_fits_mtu);

    TEST_RUN(test_ssid_scan_init_empty);
    TEST_RUN(test_ssid_scan_random_bssid_filtered);
    TEST_RUN(test_ssid_scan_add_basic);
    TEST_RUN(test_ssid_scan_dedup_updates_in_place);
    TEST_RUN(test_ssid_scan_full_replaces_weakest);
    TEST_RUN(test_ssid_scan_full_rejects_weaker);
    TEST_RUN(test_ssid_scan_sort_by_rssi);
    TEST_RUN(test_ssid_scan_sort_stable_on_empty);
    TEST_RUN(test_ssid_scan_bssids_differ);
    TEST_RUN(test_ssid_scan_long_ssid_truncated);
    TEST_RUN(test_ssid_scan_hidden_ssid);

    TEST_RUN(test_peer_gate_companion_only_write_rejected);
    TEST_RUN(test_peer_gate_companion_only_delete_rejected);
    TEST_RUN(test_peer_gate_companion_only_scan_get_rejected);
    TEST_RUN(test_peer_gate_companion_only_encounters_get_rejected);
    TEST_RUN(test_peer_gate_companion_only_attestation_write_rejected);
    TEST_RUN(test_peer_gate_companion_only_attestations_get_rejected);
    TEST_RUN(test_peer_gate_ls_allowed);
    TEST_RUN(test_peer_gate_storage_info_allowed);
    TEST_RUN(test_peer_gate_unknown_cmd_still_unknown);
    TEST_RUN(test_peer_gate_companion_mode_unchanged);

    TEST_RUN(test_encounter_record_body_byte_for_byte_reproducible);
    TEST_RUN(test_encounter_record_full_round_trip);
    TEST_RUN(test_encounter_record_verify_happy_path);
    TEST_RUN(test_encounter_record_verify_rejects_tampered_field);
    TEST_RUN(test_encounter_record_verify_rejects_tampered_sig);
    TEST_RUN(test_encounter_record_verify_rejects_swapped_sigs);
    TEST_RUN(test_encounter_record_decode_rejects_wrong_version);
    TEST_RUN(test_encounter_record_decode_rejects_oversized_bssids);
    TEST_RUN(test_encounter_record_id_is_side_symmetric);
    TEST_RUN(test_encounter_record_id_changes_with_nonce);
    TEST_RUN(test_encounter_record_decode_rejects_truncated);
    TEST_RUN(test_encounter_record_empty_bssids_allowed);

    TEST_RUN(test_encounter_session_happy_path);
    TEST_RUN(test_encounter_session_id_agrees_between_sides);
    TEST_RUN(test_encounter_session_initiator_rejects_wrong_prefix);
    TEST_RUN(test_encounter_session_responder_rejects_wrong_prefix);
    TEST_RUN(test_encounter_session_initiator_rejects_bad_sig_b);
    TEST_RUN(test_encounter_session_responder_rejects_bad_sig_a);
    TEST_RUN(test_encounter_session_initiator_drop_after_propose);
    TEST_RUN(test_encounter_session_responder_drop_after_accept);
    TEST_RUN(test_encounter_session_responder_rejects_propose_wrong_type);
    TEST_RUN(test_encounter_session_handle_before_start_is_error);
    TEST_RUN(test_encounter_session_responder_late_bind_populates_b_side);
    TEST_RUN(test_encounter_session_responder_late_bind_miss_keeps_zeros);
    TEST_RUN(test_encounter_session_responder_late_bind_runs_after_prefix_check);
    TEST_RUN(test_encounter_session_first_contact_with_late_bind_set);

    TEST_RUN(test_encounters_init_empty);
    TEST_RUN(test_encounters_record_one);
    TEST_RUN(test_encounters_iter_oldest_first);
    TEST_RUN(test_encounters_iter_order_preserved);
    TEST_RUN(test_encounters_ring_evicts_oldest);
    TEST_RUN(test_encounters_persists_across_init);
    TEST_RUN(test_encounters_should_record_first);
    TEST_RUN(test_encounters_should_record_dedup_same_set);
    TEST_RUN(test_encounters_should_record_dedup_diff_set);
    TEST_RUN(test_encounters_should_record_age_bypass);
    TEST_RUN(test_encounters_record_signs_with_seed);
    TEST_RUN(test_encounters_empty_scan_rejected);

    TEST_RUN(test_scan_get_no_scan_yet);
    TEST_RUN(test_scan_get_returns_observations);
    TEST_RUN(test_scan_get_paginates_via_next_offset);

    TEST_RUN(test_attestations_init_empty);
    TEST_RUN(test_attestations_write_one);
    TEST_RUN(test_attestations_too_big_rejected);
    TEST_RUN(test_attestations_iter_oldest_first);
    TEST_RUN(test_attestations_ring_evicts_oldest);
    TEST_RUN(test_attestations_persist_across_init);

    TEST_RUN(test_cmd_attestation_write_round_trip);
    TEST_RUN(test_cmd_attestation_write_missing_blob);

    TEST_RUN(test_encounters_get_empty);
    TEST_RUN(test_encounters_get_returns_records);
    TEST_RUN(test_encounters_get_since_filter);
    TEST_RUN(test_encounters_get_paginates_via_next_offset);

    TEST_RUN(test_attest_cache_init_empty);
    TEST_RUN(test_attest_cache_write_one);
    TEST_RUN(test_attest_cache_dedup_returns_zero);
    TEST_RUN(test_attest_cache_too_big_rejected);
    TEST_RUN(test_attest_cache_iter_oldest_first);
    TEST_RUN(test_attest_cache_ring_evicts_oldest);
    TEST_RUN(test_attest_cache_dedup_after_eviction);
    TEST_RUN(test_attest_cache_persist_across_init);

    TEST_RUN(test_attest_extract_bssids_basic);
    TEST_RUN(test_attest_extract_bssids_caps_at_max);
    TEST_RUN(test_attest_extract_bssids_skips_wrong_size);
    TEST_RUN(test_attest_extract_bssids_missing_key4);
    TEST_RUN(test_attest_extract_bssids_truncated_blob);
    TEST_RUN(test_attest_match_any_bssid_hit);
    TEST_RUN(test_attest_match_any_bssid_miss);
    TEST_RUN(test_attest_query_for_each_finds_self_records);
    TEST_RUN(test_attest_query_for_each_finds_cache_records);
    TEST_RUN(test_attest_query_for_each_filters_non_matching);
    TEST_RUN(test_attest_query_for_each_early_stops);
    TEST_RUN(test_attest_query_for_each_empty_query);

    TEST_RUN(test_cmd_attest_query_match_round_trip);
    TEST_RUN(test_cmd_attest_query_no_match_returns_empty);
    TEST_RUN(test_cmd_attest_query_paginates);
    TEST_RUN(test_cmd_attest_query_missing_bssids);
    TEST_RUN(test_cmd_attest_ingest_round_trip);
    TEST_RUN(test_cmd_attest_ingest_dedup_counts);
    TEST_RUN(test_cmd_attest_ingest_missing_blobs);

    printf("\n%d test(s) ran. %d assertion(s) failed.\n", 192, test_failures);
    return test_failures;
}
