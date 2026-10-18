/* One source read, checked snapshots, and atomic no-replace publication. */
#include "vinylscan.h"
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int existing_matches(Run *run, int parent, const char *name, const Entry *entry)
{
    int fd = openat(parent, name, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) return -1;
    struct stat before, after, named;
    uint64_t mount, size;
    char hash[65];
    int result = -1;
    if (fstat(fd, &before) < 0 || mount_id(fd, &mount) < 0) goto done;
    if (!S_ISREG(before.st_mode) || mount != run->dest_mount) { errno = EINVAL; goto done; }
    result = hash_gzip(fd, hash, &size, (uint64_t)entry->stat.st_size);
    if (result < 0) {
        /* Malformed existing gzip is preserved; genuine I/O errors stop the file. */
        if (errno == EBADMSG) result = 0;
        goto done;
    }
    if (fstat(fd, &after) < 0 || fstatat(parent, name, &named, AT_SYMLINK_NOFOLLOW) < 0) { result = -1; goto done; }
    if (!same_stat(&before, &after) || !same_stat(&before, &named)) { errno = ESTALE; result = -1; goto done; }
    result = size == (uint64_t)entry->stat.st_size && !strcmp(hash, entry->sha256);
done:
    {
        int saved = errno;
        if (close(fd) < 0 && result >= 0) return -1;
        errno = saved;
    }
    return result;
}

static int publish(Run *run, Entry *entry, const char *base)
{
    char *leaf = NULL;
    int parent = open_parent(run->dest_fd, base, true, run->dest_mount, &leaf);
    if (parent < 0) return -1;
    int result = -1;
    for (unsigned version = 0; version < 10000 && !interrupted; version++) {
        char *name = version ? format_string("%s.v%u.gz", leaf, version) : format_string("%s.gz", leaf);
        char *relative = version ? format_string("%s.v%u.gz", base, version) : format_string("%s.gz", base);
        /* Linux renameat2 guarantees existing names cannot be replaced. */
        if (rename_no_replace(run->run_fd, "file.part", parent, name) == 0) {
            entry->backup = relative;
            entry->status = "ok";
            free(name);
            if (fsync(parent) == 0 && fsync(run->run_fd) == 0) result = 0;
            break;
        }
        if (errno != EEXIST) { free(name); free(relative); break; }
        int matches = existing_matches(run, parent, name, entry);
        free(name);
        if (matches < 0) { free(relative); break; }
        if (matches) {
            entry->backup = relative;
            entry->status = "reused";
            if (unlinkat(run->run_fd, "file.part", 0) == 0 && fsync(run->run_fd) == 0) result = 0;
            break;
        }
        free(relative);
        errno = EEXIST;
    }
    int saved = interrupted ? EINTR : errno;
    free(leaf); close(parent); errno = saved;
    return result;
}

static int process_file(Run *run, Entry *entry)
{
    char *leaf = NULL;
    int parent = open_parent(run->source_fd, entry->relative, false, run->source_mount, &leaf);
    if (parent < 0) return -1;
    int fd = openat(parent, leaf, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
    int output = -1, result = -1;
    struct stat before, after, named;
    uint64_t bytes = 0;
    if (fd < 0) goto done;
    if (check_source(run, fd) < 0 || fstat(fd, &before) < 0) goto done;
    if (!S_ISREG(before.st_mode) || !same_stat(&entry->stat, &before)) { errno = ESTALE; goto done; }
    if (run->backup) {
        output = openat(run->run_fd, "file.part", O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
        if (output < 0) goto done;
    }
    if (stream_file(fd, output, entry, &bytes) < 0) goto done;
    if (fstat(fd, &after) < 0 || fstatat(parent, leaf, &named, AT_SYMLINK_NOFOLLOW) < 0 ||
        check_source(run, fd) < 0) goto done;
    if (!same_stat(&before, &after) || !same_stat(&before, &named) || bytes != (uint64_t)before.st_size) {
        errno = ESTALE; goto done;
    }
    if (close(fd) < 0) { fd = -1; goto done; }
    fd = -1;
    if (output >= 0) {
        if (fsync(output) < 0) goto done;
        if (close(output) < 0) { output = -1; goto done; }
        output = -1;
        char *base = format_string("%s/%s", run->date, entry->path + 1);
        result = publish(run, entry, base);
        free(base);
    } else { entry->status = "ok"; result = 0; }
done:
    {
        int saved = errno;
        if (fd >= 0) close(fd);
        if (output >= 0) close(output);
        close(parent); free(leaf);
        if (result < 0) {
            /* A failed or changed read has no trusted checksum or preview. */
            entry->sha256[0] = '\0'; entry->preview.type = NULL;
            if (run->backup && unlinkat(run->run_fd, "file.part", 0) < 0 && errno != ENOENT)
                log_error(run, entry->path, "could not remove incomplete destination temporary file");
        }
        errno = saved;
    }
    return result;
}

int process_inventory(Run *run, Inventory *inventory)
{
    for (size_t i = 0; i < inventory->count; i++) {
        Entry *entry = &inventory->entries[i];
        if (interrupted || run->fatal) break;
        if (!strcmp(entry->status, "pending")) {
            if (process_file(run, entry) < 0) {
                entry_error(entry, errno == ESTALE ? "file changed since scan or while reading" : strerror(errno));
            } else run->processed++;
        }
        if (!strcmp(entry->status, "error")) {
            run->errors++;
            log_error(run, entry->path, entry->error);
        } else if (!strncmp(entry->status, "skipped_", 8)) run->skipped++;
        if (metadata_write(run, entry) < 0 || fsync(run->metadata_fd) < 0) {
            log_error(run, "metadata.partial", strerror(errno));
            run->fatal = true;
            break;
        }
    }
    return run->fatal || interrupted ? -1 : 0;
}
