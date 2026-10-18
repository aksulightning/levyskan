/* Verify only destination data, including complete records in interrupted runs. */
#include "levyskan.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Treat stored paths as untrusted even if this program originally wrote them.
 * Only backup_hex is authoritative; source paths are never opened here. */
static int verify_file(Run *run, const VerifyRecord *record)
{
    char *path = hex_decode(record->backup_hex);
    if (!path || !*path) { free(path); errno = EBADMSG; return -1; }
    char *leaf = NULL;
    int parent = open_parent(run->dest_fd, path, false, run->dest_mount, &leaf);
    free(path);
    if (parent < 0) return -1;
    int fd = openat(parent, leaf, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
    int saved = errno;
    free(leaf); close(parent); errno = saved;
    if (fd < 0) return -1;
    struct stat before, after;
    uint64_t mount, size;
    char hash[65];
    int result = -1;
    if (fstat(fd, &before) < 0 || mount_id(fd, &mount) < 0) goto done;
    if (!S_ISREG(before.st_mode) || mount != run->dest_mount) { errno = EINVAL; goto done; }
    if (hash_gzip(fd, hash, &size, record->size) < 0 || fstat(fd, &after) < 0) goto done;
    if (!same_stat(&before, &after)) { errno = ESTALE; goto done; }
    if (size != record->size || strcmp(hash, record->sha256)) { errno = EILSEQ; goto done; }
    result = 0;
done:
    saved = errno;
    if (close(fd) < 0 && result == 0) return -1;
    errno = saved;
    return result;
}

/* Return 1 for a newline-terminated record, 0 for clean EOF, or -1 for a broken
 * record/read. Bound allocation instead of trusting an arbitrarily long line. */
static int read_record(FILE *file, char **buffer, size_t *capacity)
{
    size_t length = 0;
    for (;;) {
        int c = fgetc(file);
        if (c == EOF) return ferror(file) || length ? -1 : 0;
        if (!c || length == RECORD_LIMIT) { errno = EBADMSG; return -1; }
        if (length + 2 > *capacity) {
            size_t next = *capacity ? *capacity * 2 : 4096;
            char *grown = allocate(next);
            if (length) memcpy(grown, *buffer, length);
            free(*buffer); *buffer = grown; *capacity = next;
        }
        (*buffer)[length++] = (char)c;
        if (c == '\n') { (*buffer)[length] = '\0'; return 1; }
    }
}

/* Verify the usable records of one run, accumulating failures rather than hiding
 * later independent results. Only fall back to partial metadata when the final
 * file is absent; a present but unsafe/unreadable final file remains an error. */
static void verify_metadata(Run *run, int directory, const char *run_name)
{
    bool partial = false;
    int fd = openat(directory, "metadata.jsonl", O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0 && errno == ENOENT) {
        partial = true;
        fd = openat(directory, "metadata.partial", O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
    }
    /* Valid earlier records can still be checked, but they cannot turn an
     * interrupted run into a completely successful verification. */
    if (partial) { run->errors++; log_error(run, run_name, "incomplete run; checking available complete records"); }
    struct stat stat;
    uint64_t mount;
    if (fd < 0 || fstat(fd, &stat) < 0 || mount_id(fd, &mount) < 0 ||
        !S_ISREG(stat.st_mode) || mount != run->dest_mount) {
        if (fd >= 0) close(fd);
        run->errors++; log_error(run, run_name, "missing or unsafe metadata"); return;
    }
    FILE *file = fdopen(fd, "r");
    if (!file) { close(fd); run->fatal = true; return; }
    char *line = NULL;
    size_t capacity = 0, line_number = 0;
    while (!interrupted && !run->fatal) {
        int read_status = read_record(file, &line, &capacity);
        if (!read_status) break;
        line_number++;
        char *location = format_string("%s:%zu", run_name, line_number);
        VerifyRecord record;
        /* A malformed complete line has a known next boundary; a failed line
         * read does not. Continue only when the next record can be located safely. */
        if (read_status < 0 || parse_record(line, &record) < 0) {
            run->errors++; log_error(run, location, "invalid, truncated or unreadable metadata record");
            free(location);
            if (read_status < 0) break;
            continue;
        }
        if (!strcmp(record.status, "error")) {
            run->errors++; log_error(run, location, "source record reports a scan/backup failure");
        } else if (!strcmp(record.command, "backup") &&
                   (!strcmp(record.status, "ok") || !strcmp(record.status, "reused"))) {
            if (verify_file(run, &record) < 0) {
                int saved = errno;
                char *path = hex_decode(record.backup_hex);
                const char *reason = saved == EILSEQ ? "SHA-256 or uncompressed size mismatch" :
                    saved == EBADMSG ? "invalid backup path or corrupt/truncated gzip stream" : strerror(saved);
                run->errors++; log_error(run, path ? path : location, reason);
                free(path);
            } else run->processed++;
        } else run->skipped++;
        free_record(&record); free(location);
    }
    free(line);
    if (fclose(file) != 0) { run->errors++; log_error(run, run_name, "metadata close failed"); }
}

/* Walk destination run directories only. Count missing metadata and file failures
 * in Run, return -1 for traversal/setup failure, and ignore separate verify logs. */
int verify_backups(Run *run)
{
    int parent = open_directory(run->dest_fd, ".levyskan", false, run->dest_mount);
    if (parent < 0) return -1;
    DIR *directory = fdopendir(parent);
    if (!directory) { close(parent); return -1; }
    size_t runs = 0;
    int result = 0;
    while (!interrupted && !run->fatal) {
        errno = 0;
        struct dirent *entry = readdir(directory);
        if (!entry) { if (errno) result = -1; break; }
        /* The current verification also creates a directory, with a verify- prefix. */
        if (strncmp(entry->d_name, "run-", 4)) continue;
        runs++;
        int fd = open_directory(parent, entry->d_name, false, run->dest_mount);
        if (fd < 0) { run->errors++; log_error(run, entry->d_name, "cannot open run directory"); continue; }
        verify_metadata(run, fd, entry->d_name);
        close(fd);
    }
    if (closedir(directory) < 0) result = -1;
    if (!runs) { log_error(run, "destination", "no backup or scan runs found"); errno = ENOENT; return -1; }
    return result;
}
