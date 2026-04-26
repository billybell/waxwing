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
extern void test_read_chunk_past_eof(void);

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
    TEST_RUN(test_read_chunk_past_eof);

    printf("\n%d test(s) ran. %d assertion(s) failed.\n", 19, test_failures);
    return test_failures;
}
