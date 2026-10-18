/* Descriptor-based path handling and shared I/O. No source writes. */
#include "vinylscan.h"
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/random.h>
#include <sys/statvfs.h>
#include <unistd.h>

volatile sig_atomic_t interrupted;
static int emergency_log = -1;

void set_emergency_log(int fd)
{
    emergency_log = fd;
}

void *allocate(size_t size)
{
    void *memory = malloc(size ? size : 1);
    if (!memory) {
        /* Fail closed: the unfinished run and temporary gzip remain uncommitted. */
        const char message[] = "vinylscan: out of memory; run incomplete\n";
        int reported = write_all(STDERR_FILENO, message, sizeof(message) - 1);
        if (emergency_log >= 0) {
            reported = write_all(emergency_log, message, sizeof(message) - 1);
            if (reported == 0) reported = fsync(emergency_log);
        }
        (void)reported;
        exit(2);
    }
    return memory;
}

char *copy_string(const char *s)
{
    char *copy = allocate(strlen(s) + 1);
    memcpy(copy, s, strlen(s) + 1);
    return copy;
}

char *format_string(const char *format, ...)
{
    if (!format) { errno = EINVAL; return NULL; }
    va_list args, duplicate;
    va_start(args, format);
    va_copy(duplicate, args);
    int length = vsnprintf(NULL, 0, format, duplicate);
    va_end(duplicate);
    if (length < 0) { va_end(args); return NULL; }
    char *result = allocate((size_t)length + 1);
    vsnprintf(result, (size_t)length + 1, format, args);
    va_end(args);
    return result;
}

ssize_t read_retry(int fd, void *buffer, size_t size)
{
    ssize_t count;
    do {
        if (interrupted) { errno = EINTR; return -1; }
        count = read(fd, buffer, size);
    } while (count < 0 && errno == EINTR && !interrupted);
    return count;
}

int write_all(int fd, const void *buffer, size_t size)
{
    const unsigned char *p = buffer;
    while (size) {
        ssize_t count = write(fd, p, size);
        if (count < 0 && errno == EINTR && !interrupted) continue;
        if (count <= 0) { if (!count) errno = EIO; return -1; }
        p += count;
        size -= (size_t)count;
    }
    return 0;
}

int check_source(Run *run, int fd)
{
    uint64_t mount;
    struct statvfs filesystem;
    if (mount_id(fd, &mount) < 0) return -1;
    if (mount != run->source_mount) { errno = EXDEV; return -1; }
    if (fstatvfs(fd, &filesystem) < 0) return -1;
    if (!run->writable && !(filesystem.f_flag & ST_RDONLY)) {
        errno = EROFS;
        return -1;
    }
    return 0;
}

int open_directory(int parent, const char *name, bool create, uint64_t mount)
{
    if (!*name || !strcmp(name, ".") || !strcmp(name, "..") || strchr(name, '/')) {
        errno = EINVAL;
        return -1;
    }
    if (create && mkdirat(parent, name, 0700) < 0 && errno != EEXIST) return -1;
    int fd = openat(parent, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) return -1;
    uint64_t found;
    if (mount && (mount_id(fd, &found) < 0 || found != mount)) {
        close(fd); errno = EXDEV; return -1;
    }
    if (create && fsync(parent) < 0) { int saved = errno; close(fd); errno = saved; return -1; }
    return fd;
}

int open_parent(int root, const char *path, bool create, uint64_t mount, char **leaf)
{
    if (!*path || *path == '/') { errno = EINVAL; return -1; }
    char *copy = copy_string(path), *part = copy, *slash;
    int fd = dup(root);
    if (fd < 0) { free(copy); return -1; }
    while ((slash = strchr(part, '/'))) {
        *slash = '\0';
        int next = open_directory(fd, part, create, mount);
        int saved = errno;
        close(fd);
        if (next < 0) { free(copy); errno = saved; return -1; }
        fd = next;
        part = slash + 1;
    }
    if (!*part || !strcmp(part, ".") || !strcmp(part, "..")) {
        close(fd); free(copy); errno = EINVAL; return -1;
    }
    *leaf = copy_string(part);
    free(copy);
    return fd;
}

int open_root(const char *path)
{
    if (path[0] != '/') { errno = EINVAL; return -1; }
    int root = open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (root < 0 || !path[1]) return root;
    char *leaf = NULL;
    int parent = open_parent(root, path + 1, false, 0, &leaf);
    close(root);
    if (parent < 0) return -1;
    int fd = open_directory(parent, leaf, false, 0);
    int saved = errno;
    free(leaf); close(parent); errno = saved;
    return fd;
}

bool same_stat(const struct stat *a, const struct stat *b)
{
    return a->st_dev == b->st_dev && a->st_ino == b->st_ino &&
        a->st_mode == b->st_mode && a->st_size == b->st_size &&
        a->st_mtim.tv_sec == b->st_mtim.tv_sec && a->st_mtim.tv_nsec == b->st_mtim.tv_nsec &&
        a->st_ctim.tv_sec == b->st_ctim.tv_sec && a->st_ctim.tv_nsec == b->st_ctim.tv_nsec;
}

int create_run(Run *run, const char *prefix)
{
    int parent = open_directory(run->dest_fd, ".vinylscan", true, run->dest_mount);
    if (parent < 0) return -1;
    unsigned char random[16];
    ssize_t count;
    do { count = getrandom(random, sizeof(random), 0); } while (count < 0 && errno == EINTR);
    if (count != sizeof(random)) { close(parent); errno = EIO; return -1; }
    char suffix[33];
    for (size_t i = 0; i < sizeof(random); i++) snprintf(suffix + i * 2, 3, "%02x", random[i]);
    snprintf(run->run_name, sizeof(run->run_name), "%s-%s", prefix, suffix);
    if (mkdirat(parent, run->run_name, 0700) < 0) { close(parent); return -1; }
    run->run_fd = open_directory(parent, run->run_name, false, run->dest_mount);
    int synced = fsync(parent);
    close(parent);
    if (run->run_fd < 0 || synced < 0) return -1;
    run->log_fd = openat(run->run_fd, "run.log", O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    return run->log_fd < 0 ? -1 : fsync(run->run_fd);
}

void log_error(Run *run, const char *path, const char *message)
{
    /* JSON quoting also prevents terminal/control-character injection. */
    char *line = NULL;
    size_t size = 0;
    FILE *memory = open_memstream(&line, &size);
    if (!memory) { run->fatal = true; return; }
    fputs("vinylscan: ", memory); json_string(memory, path);
    fputs(": ", memory); json_string(memory, message); fputc('\n', memory);
    if (fclose(memory) != 0) run->fatal = true;
    if (line) {
        if (write_all(STDERR_FILENO, line, size) < 0) run->fatal = true;
        if (run->log_fd >= 0 && write_all(run->log_fd, line, size) < 0) run->fatal = true;
    }
    free(line);
}

void entry_error(Entry *entry, const char *message)
{
    entry->status = "error";
    free(entry->error);
    entry->error = copy_string(message);
}
