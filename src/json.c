/* Strict flat-schema JSON reader for Vinylscan records; no external database. */
#include "vinylscan.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>

/* JSON permits only these four whitespace bytes, regardless of process locale. */
static void whitespace(const char **p)
{
    while (**p == ' ' || **p == '\t' || **p == '\r' || **p == '\n') (*p)++;
}

/* Consume exactly four hexadecimal digits after a JSON Unicode escape marker.
 * The string parser, not this helper, combines surrogate pairs into scalar values. */
static int unicode_escape(const char **p, unsigned *value)
{
    *value = 0;
    for (int i = 0; i < 4; i++) {
        unsigned char c = (unsigned char)**p;
        unsigned n;
        if (c >= '0' && c <= '9') n = c - '0';
        else if (c >= 'a' && c <= 'f') n = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') n = c - 'A' + 10;
        else return -1;
        *value = *value * 16 + n;
        (*p)++;
    }
    return 0;
}

/* Decode a quoted JSON string into owned UTF-8 and advance the input cursor.
 * Reject malformed UTF-8, invalid escapes and embedded NUL so subsequent C-string
 * comparisons cannot silently examine only part of a metadata field. */
static char *string_value(const char **p)
{
    if (*(*p)++ != '"') return NULL;
    /* Find the unescaped closing quote before allocating: charging each tiny key
     * for the entire remaining record would multiply memory use on large records. */
    const char *end = *p;
    while (*end && *end != '"') {
        if (*end == '\\' && end[1]) end++;
        end++;
    }
    if (!*end) return NULL;
    char *value = allocate((size_t)(end - *p) + 1);
    size_t length = 0;
    while (**p && **p != '"') {
        unsigned char c = (unsigned char)*(*p)++;
        if (c < 32) goto invalid;
        if (c != '\\') {
            if (c >= 128) {
                size_t width = utf8_character((const unsigned char *)*p - 1, (size_t)(end - *p) + 1);
                if (!width) goto invalid;
                memcpy(value + length, *p - 1, width);
                length += width; *p += width - 1;
            } else value[length++] = (char)c;
            continue;
        }
        c = (unsigned char)*(*p)++;
        switch (c) {
        case '"': case '\\': case '/': value[length++] = (char)c; break;
        case 'b': value[length++] = '\b'; break;
        case 'f': value[length++] = '\f'; break;
        case 'n': value[length++] = '\n'; break;
        case 'r': value[length++] = '\r'; break;
        case 't': value[length++] = '\t'; break;
        case 'u': {
            unsigned scalar;
            if (unicode_escape(p, &scalar) < 0 || !scalar) goto invalid;
            /* A high surrogate requires an adjacent low surrogate. A lone surrogate
             * is not a Unicode scalar and must never be encoded as UTF-8. */
            if (scalar >= 0xd800 && scalar <= 0xdbff) {
                unsigned low;
                if (strncmp(*p, "\\u", 2)) goto invalid;
                *p += 2;
                if (unicode_escape(p, &low) < 0 || low < 0xdc00 || low > 0xdfff) goto invalid;
                scalar = 0x10000 + ((scalar - 0xd800) << 10) + low - 0xdc00;
            } else if (scalar >= 0xdc00 && scalar <= 0xdfff) goto invalid;
            if (scalar < 0x80) value[length++] = (char)scalar;
            else {
                if (scalar >= 0x10000) value[length++] = (char)(0xf0 | (scalar >> 18));
                if (scalar >= 0x800) value[length++] = (char)((scalar < 0x10000 ? 0xe0 : 0x80) | ((scalar >> 12) & 63));
                value[length++] = (char)((scalar < 0x800 ? 0xc0 : 0x80) | ((scalar >> 6) & 63));
                value[length++] = (char)(0x80 | (scalar & 63));
            }
            break;
        }
        default: goto invalid;
        }
    }
    if (**p != '"') goto invalid;
    (*p)++;
    value[length] = '\0';
    return value;
invalid:
    free(value);
    return NULL;
}

/* Release decoded fields after either verification or parsing failure, then clear
 * the structure so callers cannot accidentally retain dangling string pointers. */
void free_record(VerifyRecord *record)
{
    free(record->backup_hex); free(record->sha256); free(record->status); free(record->command);
    memset(record, 0, sizeof(*record));
}

/* Accept the scalar-only version-1 schema, not arbitrary JSON documents.
 * Reject duplicate keys so two consumers cannot disagree about which path/hash
 * applies. Field order is irrelevant; unknown fields using supported scalar types
 * are ignored. Nested values, negative numbers and floating-point values are rejected. */
int parse_record(const char *line, VerifyRecord *record)
{
    const char *p = line;
    char *keys[64] = {0};
    size_t count = 0;
    /* Six bits track required field presence separately from nullable field values. */
    unsigned required = 0;
    int result = -1;
    memset(record, 0, sizeof(*record));
    whitespace(&p);
    if (*p++ != '{') goto done;
    for (;;) {
        whitespace(&p);
        if (*p != '"' || count == 64) goto done;
        char *key = string_value(&p);
        if (!key) goto done;
        for (size_t i = 0; i < count; i++) if (!strcmp(keys[i], key)) { free(key); goto done; }
        keys[count++] = key;
        whitespace(&p);
        if (*p++ != ':') goto done;
        whitespace(&p);
        char *value = NULL;
        enum { STRING, NUMBER, OTHER } type = OTHER;
        uint64_t number = 0;
        if (*p == '"') {
            type = STRING;
            value = string_value(&p);
            if (!value) goto done;
        } else if (*p >= '0' && *p <= '9') {
            type = NUMBER;
            const char *start = p;
            do {
                unsigned digit = (unsigned)(*p++ - '0');
                /* Test before multiply/add, so a malicious size cannot wrap around. */
                if (number > (UINT64_MAX - digit) / 10) goto done;
                number = number * 10 + digit;
            } while (*p >= '0' && *p <= '9');
            if (*start == '0' && p - start > 1) goto done;
        } else if (!strncmp(p, "null", 4)) p += 4;
        else if (!strncmp(p, "true", 4)) p += 4;
        else if (!strncmp(p, "false", 5)) p += 5;
        else goto done;
        bool valid = true;
        if (!strcmp(key, "schema")) { valid = type == NUMBER && number == 1; required |= 1; }
        else if (!strcmp(key, "size")) { valid = type == NUMBER; record->size = number; required |= 2; }
        else if (!strcmp(key, "command")) { record->command = value; value = NULL; required |= 4; }
        else if (!strcmp(key, "status")) { record->status = value; value = NULL; required |= 8; }
        else if (!strcmp(key, "sha256")) { record->sha256 = value; value = NULL; required |= 16; }
        else if (!strcmp(key, "backup_hex")) { record->backup_hex = value; value = NULL; required |= 32; }
        /* Recognized string fields transfer ownership to record by nulling value. */
        free(value);
        if (!valid) goto done;
        whitespace(&p);
        if (*p == '}') { p++; break; }
        if (*p++ != ',') goto done;
    }
    whitespace(&p);
    if (*p || required != 63 || !record->status || !record->command) goto done;
    if (strcmp(record->command, "scan") && strcmp(record->command, "backup")) goto done;
    const char *statuses[] = {"ok", "reused", "error", "directory", "skipped_mount", "skipped_symlink", "skipped_special"};
    bool known = false;
    for (size_t i = 0; i < sizeof(statuses) / sizeof(*statuses); i++)
        if (!strcmp(record->status, statuses[i])) known = true;
    if (!known) goto done;
    /* Successful entries must carry integrity data; skipped/failed ones may not. */
    if (!strcmp(record->status, "ok") || !strcmp(record->status, "reused")) {
        if (!record->sha256 || strlen(record->sha256) != 64 ||
            strspn(record->sha256, "0123456789abcdef") != 64) goto done;
        if (!strcmp(record->command, "backup") && !record->backup_hex) goto done;
    }
    result = 0;
done:
    for (size_t i = 0; i < count; i++) free(keys[i]);
    if (result) { free_record(record); errno = EBADMSG; }
    return result;
}
