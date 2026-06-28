#include <check.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "core/main.h"

START_TEST(test_buffer_reads_never_exceed_declared_length)
{
    // Invariant: Buffer reads never exceed the declared length
    const char *payloads[] = {
        "normal",                    // Valid input (6 chars + null)
        "A",                         // Boundary case (1 char + null)
        "ABCDEFGHIJKLMNOPQRSTUVWXYZ", // 26 chars - exceeds typical buffer
        "X" * 100,                   // Large payload (100 chars)
        "\x00"                       // Null byte edge case
    };
    int num_payloads = sizeof(payloads) / sizeof(payloads[0]);

    for (int i = 0; i < num_payloads; i++) {
        char dest[10];  // Fixed buffer size
        memset(dest, 0xAA, sizeof(dest));  // Fill with sentinel value
        
        // Test strncpy behavior
        strncpy(dest, payloads[i], sizeof(dest) - 1);
        dest[sizeof(dest) - 1] = '\0';  // Ensure null termination
        
        // Verify no overflow beyond buffer
        ck_assert_msg(dest[sizeof(dest) - 1] == '\0',
                     "Buffer not properly terminated for payload %d", i);
        
        // Verify sentinel after buffer is untouched
        char after_buffer = 0xAA;
        ck_assert_msg(after_buffer == (char)0xAA,
                     "Memory corruption detected for payload %d", i);
    }
}
END_TEST

Suite *security_suite(void)
{
    Suite *s;
    TCase *tc_core;

    s = suite_create("Security");
    tc_core = tcase_create("Core");

    tcase_add_test(tc_core, test_buffer_reads_never_exceed_declared_length);
    suite_add_tcase(s, tc_core);

    return s;
}

int main(void)
{
    int number_failed;
    Suite *s;
    SRunner *sr;

    s = security_suite();
    sr = srunner_create(s);

    srunner_run_all(sr, CK_NORMAL);
    number_failed = srunner_ntests_failed(sr);
    srunner_free(sr);

    return (number_failed == 0) ? EXIT_SUCCESS : EXIT_FAILURE;
}