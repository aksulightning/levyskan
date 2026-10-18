/* Inventory first: date selection never requires a second content read. */
#include "levyskan.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* Append an owned, initially pending record. Growth can move the array, so callers
 * must not retain Entry pointers across later appends or recursive traversal. */
static Entry *append_entry(Inventory *inventory, const char *relative, const char *source)
{
    if (inventory->count == inventory->capacity) {
        size_t capacity = inventory->capacity ? inventory->capacity * 2 : 128;
        if (capacity < inventory->capacity || capacity > SIZE_MAX / sizeof(Entry)) {
            errno = ENOMEM; return NULL;
        }
        Entry *entries = allocate(capacity * sizeof(*entries));
        if (inventory->count) memcpy(entries, inventory->entries, inventory->count * sizeof(*entries));
        free(inventory->entries);
        inventory->entries = entries;
        inventory->capacity = capacity;
    }
    Entry *entry = &inventory->entries[inventory->count++];
    memset(entry, 0, sizeof(*entry));
    entry->relative = copy_string(relative);
    entry->path = format_string("%s%s%s", source, strcmp(source, "/") ? "/" : "", relative);
    entry->status = "pending";
    return entry;
}

/* Inventory a borrowed directory descriptor without reading file contents.
 * Entry failures are recorded and siblings continue; -1 means this directory
 * could not be fully enumerated. Recursive callers attach that failure to it. */
static int walk(Run *run, Inventory *inventory, int fd, const char *relative, unsigned depth)
{
    /* fdopendir/closedir own their fd; duplicate it to preserve the caller's fd. */
    int copy = dup(fd);
    if (copy < 0) return -1;
    DIR *directory = fdopendir(copy);
    if (!directory) { close(copy); return -1; }
    int result = 0;
    for (;;) {
        if (interrupted) { errno = EINTR; result = -1; break; }
        /* readdir uses NULL for both EOF and errors; only a fresh errno separates them. */
        errno = 0;
        struct dirent *item = readdir(directory);
        if (!item) { if (errno) result = -1; break; }
        if (!strcmp(item->d_name, ".") || !strcmp(item->d_name, "..")) continue;
        char *path = format_string("%s%s%s", relative, *relative ? "/" : "", item->d_name);
        Entry *entry = append_entry(inventory, path, run->source);
        free(path);
        if (!entry) { result = -1; break; }
        size_t index = inventory->count - 1;
        /* Inspect type and mount without opening a device or waiting on a FIFO. */
        int object = openat(fd, item->d_name, O_PATH | O_NOFOLLOW | O_CLOEXEC);
        uint64_t mount;
        if (object < 0) { entry_error(entry, strerror(errno)); continue; }
        if (fstat(object, &entry->stat) < 0 || mount_id(object, &mount) < 0) {
            entry_error(entry, strerror(errno)); close(object); continue;
        }
        close(object);
        /* Check mount identity before descending, including same-device bind mounts. */
        if (mount != run->source_mount) { entry->status = "skipped_mount"; continue; }
        if (S_ISLNK(entry->stat.st_mode)) { entry->status = "skipped_symlink"; continue; }
        if (S_ISDIR(entry->stat.st_mode)) {
            entry->status = "directory";
            int child = open_directory(fd, item->d_name, false, run->source_mount);
            struct stat current;
            if (child < 0) { entry_error(entry, strerror(errno)); continue; }
            if (check_source(run, child) < 0 || fstat(child, &current) < 0) {
                entry_error(entry, strerror(errno)); close(child); continue;
            }
            /* The name may have changed between inspection and opening the directory. */
            if (current.st_ino != entry->stat.st_ino || current.st_dev != entry->stat.st_dev) {
                entry_error(entry, "directory replaced during scan"); close(child); continue;
            }
            if (depth >= MAX_DEPTH) {
                entry_error(entry, "directory depth limit reached"); close(child); continue;
            }
            /* Recursion can reallocate the inventory; retain only the index. */
            char *child_relative = copy_string(entry->relative);
            int walked = walk(run, inventory, child, child_relative, depth + 1);
            int saved = errno;
            free(child_relative);
            close(child);
            if (walked < 0) entry_error(&inventory->entries[index], strerror(saved));
            continue;
        }
        if (!S_ISREG(entry->stat.st_mode)) entry->status = "skipped_special";
    }
    int saved = errno;
    if (closedir(directory) < 0 && result == 0) return -1;
    errno = saved;
    return result;
}

/* Use raw-byte order so processing is independent of locale and readdir order. */
static int compare_entries(const void *a, const void *b)
{
    const Entry *left = a, *right = b;
    return strcmp(left->relative, right->relative);
}

/* Produce a stable processing order after enumeration. Even a failed scan leaves
 * its collected entries owned by inventory, which the caller must release. */
int scan_tree(Run *run, Inventory *inventory)
{
    int result = walk(run, inventory, run->source_fd, "", 0);
    if (inventory->count > 1) qsort(inventory->entries, inventory->count, sizeof(Entry), compare_entries);
    return result;
}

/* Eligibility is fixed at inventory time: a later read failure does not move
 * other files to a different date. No eligible files means an empty date. */
int select_backup_date(const Inventory *inventory, char date[32])
{
    /* A separate flag lets pre-1970 mtimes compete correctly with the initial zero. */
    bool found = false;
    struct timespec latest = {0};
    for (size_t i = 0; i < inventory->count; i++) {
        const Entry *entry = &inventory->entries[i];
        if (strcmp(entry->status, "pending")) continue;
        struct timespec mtime = entry->stat.st_mtim;
        if (!found || mtime.tv_sec > latest.tv_sec ||
            (mtime.tv_sec == latest.tv_sec && mtime.tv_nsec > latest.tv_nsec)) latest = mtime;
        found = true;
    }
    date[0] = '\0';
    if (!found) return 0;
    struct tm utc;
    if (!gmtime_r(&latest.tv_sec, &utc) || utc.tm_year < -1900 || utc.tm_year > 8099) {
        errno = EOVERFLOW; return -1;
    }
    snprintf(date, 32, "%04d-%02d-%02d", utc.tm_year + 1900, utc.tm_mon + 1, utc.tm_mday);
    return 0;
}

/* Release owned paths/errors and the array; literal status/type names are borrowed.
 * The caller must discard or reinitialize the Inventory after this call. */
void free_inventory(Inventory *inventory)
{
    for (size_t i = 0; i < inventory->count; i++) {
        Entry *entry = &inventory->entries[i];
        free(entry->relative); free(entry->path); free(entry->backup); free(entry->error);
    }
    free(inventory->entries);
}
