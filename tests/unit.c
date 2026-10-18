/* Boundary tests for preview handling and the metadata parser. */
#include "vinylscan.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>

/* Exercise byte-budget boundaries, invalid Unicode and a split sample-ending
 * code point; these cases cannot be established just by testing ASCII files. */
static void preview_tests(void)
{
    unsigned char buffer[PREVIEW_SAMPLE];
    Preview preview = {.sample = buffer};
    memset(buffer, 'a', 255);
    memcpy(buffer + 255, "\xc3\xa4\n", 3);
    preview.length = 258;
    preview_finish(&preview);
    assert(!strcmp(preview.type, "text") && strlen(preview.text) == 255 && preview.truncated);
    preview = (Preview){.sample = buffer};
    memset(buffer, 'a', 256); buffer[256] = '\n';
    preview.length = 257; preview_finish(&preview);
    assert(strlen(preview.text) == 256 && !preview.truncated);
    const unsigned char invalid[][4] = {{0xc0, 0x80}, {0xed, 0xa0, 0x80}, {0xf4, 0x90, 0x80, 0x80}, {0}};
    for (size_t i = 0; i < sizeof(invalid) / sizeof(*invalid); i++) {
        preview = (Preview){.sample = buffer};
        preview_add(&preview, invalid[i], 4); preview_finish(&preview);
        assert(!strcmp(preview.type, "hex"));
    }
    preview = (Preview){.sample = buffer, .length = PREVIEW_SAMPLE};
    memset(buffer, 'x', PREVIEW_SAMPLE);
    buffer[PREVIEW_SAMPLE - 2] = 0xe2;
    buffer[PREVIEW_SAMPLE - 1] = 0x82;
    preview_finish(&preview);
    assert(!strcmp(preview.type, "text"));
    buffer[PREVIEW_SAMPLE - 1] = 'a';
    preview_finish(&preview);
    assert(!strcmp(preview.type, "hex"));
}

/* Reject every truncation of a valid record and malformed encodings/numbers,
 * then exercise deterministic mutations and lossless non-UTF-8 path transport. */
static void parser_tests(void)
{
    const char *valid = "{\"schema\":1,\"size\":0,\"command\":\"backup\",\"status\":\"error\",\"sha256\":null,\"backup_hex\":null}";
    VerifyRecord record;
    assert(parse_record(valid, &record) == 0); free_record(&record);
    for (size_t i = 0; i < strlen(valid); i++) {
        char *partial = copy_string(valid); partial[i] = 0;
        assert(parse_record(partial, &record) < 0); free(partial);
    }
    const char *invalid[] = {"", "{}", "{", "{\"", "{\"a\":\"\\", "{\"a\":\"\\u", "{\"a\":00}",
        "{\"a\":18446744073709551616}", "{\"a\":\"\\ud800\"}", "{\"a\":1,\"a\":2}",
        "{\"a\":\"\\u0000\"}", "{\"a\":\"\xff\"}", "{\"a\":true,}"};
    for (size_t i = 0; i < sizeof(invalid) / sizeof(*invalid); i++) assert(parse_record(invalid[i], &record) < 0);
    /* Deterministic malformed-input coverage, useful under ASan/UBSan. */
    unsigned state = 42;
    for (unsigned i = 0; i < 10000; i++) {
        char *mutation = copy_string(valid);
        state = state * 1664525U + 1013904223U;
        size_t at = state % strlen(valid);
        state = state * 1664525U + 1013904223U;
        mutation[at] = (char)(state >> 24);
        if (parse_record(mutation, &record) == 0) free_record(&record);
        free(mutation);
    }
    char *hex = hex_encode("a\xff\n");
    char *raw = hex_decode(hex);
    assert(raw && !strcmp(raw, "a\xff\n")); free(raw); free(hex);
    assert(!hex_decode("00") && !hex_decode("0") && !hex_decode("gg"));
}

/* Assertions stop at the first violated contract; print success only after both suites. */
int main(void)
{
    preview_tests(); parser_tests();
    puts("PASS preview and JSON parser boundary tests");
    return 0;
}
