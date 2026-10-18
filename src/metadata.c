/* JSONL writer: readable strings plus lossless hexadecimal pathname bytes. */
#include "vinylscan.h"
#include <errno.h>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Invalid UTF-8 bytes get a valid JSON display escape. This display mapping is
 * not reversible alongside valid UTF-8; path_hex/backup_hex preserve exact bytes. */
void json_string(FILE *file, const char *value)
{
    if (!value) { fputs("null", file); return; }
    fputc('"', file);
    size_t length = strlen(value);
    for (size_t i = 0; i < length;) {
        const unsigned char *p = (const unsigned char *)value + i;
        size_t width = utf8_character(p, length - i);
        if (*p == '"' || *p == '\\') { fputc('\\', file); fputc(*p, file); i++; }
        else if (*p < 32 || *p == 127 || !width) { fprintf(file, "\\u%04x", *p); i++; }
        else { fwrite(p, 1, width, file); i += width; }
    }
    fputc('"', file);
}

/* Encode pathname bytes, excluding the terminating NUL, into caller-owned ASCII.
 * Check allocation arithmetic before doubling the length for two digits per byte. */
char *hex_encode(const char *value)
{
    size_t size = strlen(value);
    if (size > (SIZE_MAX - 1) / 2) { errno = EOVERFLOW; return NULL; }
    char *hex = allocate(size * 2 + 1);
    for (size_t i = 0; i < size; i++) snprintf(hex + i * 2, 3, "%02x", (unsigned char)value[i]);
    hex[size * 2] = '\0';
    return hex;
}

/* Decode a hex digit without locale rules; -1 distinguishes invalid input from zero. */
static int nibble(char value)
{
    if (value >= '0' && value <= '9') return value - '0';
    if (value >= 'a' && value <= 'f') return value - 'a' + 10;
    if (value >= 'A' && value <= 'F') return value - 'A' + 10;
    return -1;
}

/* Return owned pathname bytes or NULL for malformed hex. Embedded NUL is rejected
 * because filesystem APIs would otherwise use only a prefix of the supplied path. */
char *hex_decode(const char *value)
{
    size_t length = strlen(value);
    if (length % 2) return NULL;
    char *raw = allocate(length / 2 + 1);
    for (size_t i = 0; i < length; i += 2) {
        int high = nibble(value[i]), low = nibble(value[i + 1]);
        if (high < 0 || low < 0 || !(high || low)) { free(raw); return NULL; }
        raw[i / 2] = (char)(high * 16 + low);
    }
    raw[length / 2] = '\0';
    return raw;
}

/* Keys are fixed program literals; nullable values always pass through JSON escaping. */
static void string_field(FILE *file, const char *key, const char *value)
{
    fprintf(file, ",\"%s\":", key);
    json_string(file, value);
}

/* Assemble one complete line before writing. A short/failing write may still
 * leave a partial tail; the run coordinator must stop and retain metadata.partial. */
int metadata_write(Run *run, const Entry *entry)
{
    char *line = NULL;
    size_t length = 0;
    FILE *file = open_memstream(&line, &length);
    if (!file) return -1;
    char timestamp[64] = "";
    struct tm utc;
    if (gmtime_r(&entry->stat.st_mtime, &utc)) strftime(timestamp, sizeof(timestamp), "%Y-%m-%dT%H:%M:%SZ", &utc);
    char *path_hex = hex_encode(entry->path);
    char *backup_hex = entry->backup ? hex_encode(entry->backup) : NULL;
    fprintf(file, "{\"schema\":1,\"command\":\"%s\"", run->backup ? "backup" : "scan");
    string_field(file, "path", entry->path);
    string_field(file, "path_hex", path_hex);
    const char *filename = strrchr(entry->path, '/');
    string_field(file, "filename", filename ? filename + 1 : entry->path);
    fprintf(file, ",\"size\":%" PRIuMAX ",\"mode\":%u", (uintmax_t)entry->stat.st_size,
            (unsigned)(entry->stat.st_mode & 07777));
    string_field(file, "mtime", timestamp);
    fprintf(file, ",\"mtime_ns\":%ld", entry->stat.st_mtim.tv_nsec);
    /* Null distinguishes unavailable results from valid empty-file hash/preview data. */
    string_field(file, "sha256", entry->sha256[0] ? entry->sha256 : NULL);
    string_field(file, "preview_type", entry->preview.type);
    string_field(file, "preview", entry->preview.type ? entry->preview.text : NULL);
    fprintf(file, ",\"preview_truncated\":%s", entry->preview.truncated ? "true" : "false");
    string_field(file, "backup", entry->backup);
    string_field(file, "backup_hex", backup_hex);
    string_field(file, "status", entry->status);
    string_field(file, "error", entry->error);
    fputs("}\n", file);
    bool failed = ferror(file);
    /* Closing open_memstream finalizes its buffer and length before the fd write. */
    if (fclose(file) != 0) failed = true;
    int result = -1;
    if (!failed && length <= RECORD_LIMIT) result = write_all(run->metadata_fd, line, length);
    else errno = failed ? EIO : EOVERFLOW;
    free(path_hex); free(backup_hex); free(line);
    return result;
}
