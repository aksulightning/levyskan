/* Predictable UTF-8 validation and previews from the first 4096 bytes. */
#include "levyskan.h"
#include <string.h>

/* Return the first complete UTF-8 scalar's byte width, or zero if invalid/incomplete.
 * Validate bytes without locale-dependent character classification or decoding files. */
size_t utf8_character(const unsigned char *s, size_t length)
{
    if (!length) return 0;
    unsigned char a = s[0];
    if (a < 0x80) return 1;
    size_t n = a >= 0xc2 && a <= 0xdf ? 2 :
               a >= 0xe0 && a <= 0xef ? 3 : a >= 0xf0 && a <= 0xf4 ? 4 : 0;
    if (!n || length < n) return 0;
    for (size_t i = 1; i < n; i++) if ((s[i] & 0xc0) != 0x80) return 0;
    /* Exclude overlong forms, UTF-16 surrogate values and values beyond U+10FFFF. */
    if ((a == 0xe0 && s[1] < 0xa0) || (a == 0xed && s[1] >= 0xa0) ||
        (a == 0xf0 && s[1] < 0x90) || (a == 0xf4 && s[1] >= 0x90)) return 0;
    return n;
}

/* Retain only the first sample window in the caller-provided buffer. Later data
 * still reaches the checksum/compressor but adds no preview memory or source reads. */
void preview_add(Preview *preview, const unsigned char *data, size_t length)
{
    size_t remaining = PREVIEW_SAMPLE - preview->length;
    if (length > remaining) length = remaining;
    memcpy(preview->sample + preview->length, data, length);
    preview->length += length;
}

/* Distinguish a valid prefix cut by the sampling boundary from already-invalid
 * UTF-8. Only a boundary cut may be ignored when classifying the sample as text. */
static bool incomplete_utf8(const unsigned char *s, size_t length)
{
    unsigned char c = s[0];
    size_t expected = c >= 0xc2 && c <= 0xdf ? 2 :
                      c >= 0xe0 && c <= 0xef ? 3 : c >= 0xf0 && c <= 0xf4 ? 4 : 0;
    if (!expected || length >= expected) return false;
    for (size_t i = 1; i < length; i++) if ((s[i] & 0xc0) != 0x80) return false;
    if (length > 1 && ((c == 0xe0 && s[1] < 0xa0) || (c == 0xed && s[1] >= 0xa0) ||
                      (c == 0xf0 && s[1] < 0x90) || (c == 0xf4 && s[1] >= 0x90))) return false;
    return true;
}

/* Classify the sample, then store either its first line or its first 16 hex bytes.
 * Classification examines the whole sample, so a later NUL can make an apparently
 * textual first line binary. Empty input is text; no filename extension is consulted. */
void preview_finish(Preview *preview)
{
    bool text = true;
    for (size_t i = 0; i < preview->length;) {
        unsigned char c = preview->sample[i];
        size_t width = utf8_character(preview->sample + i, preview->length - i);
        /* A sample boundary may split a valid multibyte character. */
        if (!width && preview->length == PREVIEW_SAMPLE &&
            incomplete_utf8(preview->sample + i, preview->length - i)) break;
        /* Permit common text whitespace but reject NUL, DEL and C1 controls. */
        if (!width || (c < 32 && c != '\n' && c != '\r' && c != '\t') || c == 127 ||
            (c == 0xc2 && preview->sample[i + 1] >= 0x80 && preview->sample[i + 1] <= 0x9f)) {
            text = false; break;
        }
        i += width;
    }
    if (!text) {
        preview->type = "hex";
        size_t length = preview->length < 16 ? preview->length : 16;
        for (size_t i = 0; i < length; i++)
            snprintf(preview->text + i * 3, sizeof(preview->text) - i * 3,
                     i + 1 == length ? "%02X" : "%02X ", preview->sample[i]);
        return;
    }
    preview->type = "text";
    size_t length = 0;
    while (length < preview->length && preview->sample[length] != '\n' && preview->sample[length] != '\r') {
        size_t width = utf8_character(preview->sample + length, preview->length - length);
        /* Count complete code points against a byte budget, never split UTF-8. */
        if (!width || length + width > 256) { preview->truncated = true; break; }
        length += width;
    }
    memcpy(preview->text, preview->sample, length);
    preview->text[length] = '\0';
}
