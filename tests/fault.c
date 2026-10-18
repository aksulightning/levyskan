/* Test-only syscall fault injection; never linked into the application. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

static int target = -1;
static int part_fd = -1, metadata_fd = -1;
static unsigned reads, writes;

/* Each subprocess selects one reproducible failure; no production code reads this. */
static bool fault(const char *name)
{
    const char *selected = getenv("VINYLSCAN_TEST_FAULT");
    return selected && !strcmp(selected, name);
}

/* Let O_PATH inventory succeed, then fail the fixture's content open when asked.
 * Track reused fd numbers so failures do not leak into unrelated sibling files. */
static int test_open(int parent, const char *path, int flags, mode_t mode)
{
    bool is_target = !strcmp(path, "target") && !(flags & (O_PATH | O_DIRECTORY));
    if (is_target && fault("unreadable")) { errno = EACCES; return -1; }
    if (is_target && fault("missing")) { errno = ENOENT; return -1; }
    int fd = (int)syscall(SYS_openat, parent, path, flags, mode);
    if (fd >= 0) {
        if (fd == target) target = -1;
        if (fd == part_fd) part_fd = -1;
        if (fd == metadata_fd) metadata_fd = -1;
        if (is_target) target = fd;
        if (!strcmp(path, "file.part")) part_fd = fd;
        if (!strcmp(path, "metadata.partial")) metadata_fd = fd;
    }
    return fd;
}

/* Interpose libc's openat while preserving its optional mode argument. The helper
 * invokes the syscall directly to avoid recursively entering this interposer. */
int openat(int parent, const char *path, int flags, ...)
{
    mode_t mode = 0;
    if (flags & O_CREAT) {
        va_list args;
        va_start(args, flags); mode = (mode_t)va_arg(args, int); va_end(args);
    }
    return test_open(parent, path, flags, mode);
}

#if defined(__GLIBC__)
/* glibc redirects large-file builds here; musl uses the ordinary openat symbol. */
int openat64(int parent, const char *path, int flags, ...)
{
    mode_t mode = 0;
    if (flags & O_CREAT) {
        va_list args;
        va_start(args, flags); mode = (mode_t)va_arg(args, int); va_end(args);
    }
    return test_open(parent, path, flags, mode);
}
#endif

/* Inject signals, short reads or a failure after progress into the target fixture.
 * The changed case deliberately edits only a disposable test file's timestamp;
 * this shared library is never part of the Vinylscan executable. */
ssize_t read(int fd, void *buffer, size_t size)
{
    if (fd == target) {
        if (fault("interrupt")) { raise(SIGTERM); errno = EINTR; return -1; }
        if (fault("io") && reads++) { errno = EIO; return -1; }
        if (fault("eintr")) {
            if (!reads++) { errno = EINTR; return -1; }
            if (size > 137) size = 137;
        }
        if (fault("changed") && !reads++) {
            struct stat status;
            if (fstat(fd, &status) == 0) {
                struct timespec times[2] = {{.tv_nsec = UTIME_OMIT}, status.st_mtim};
                times[1].tv_sec++;
                if (futimens(fd, times) < 0) abort();
            }
        }
    }
    return syscall(SYS_read, fd, buffer, size);
}

/* Limit faults to destination temporary/metadata fds. The metadata case writes
 * a real prefix before failing, exercising detection of a truncated JSONL tail. */
ssize_t write(int fd, const void *buffer, size_t size)
{
    if (fd == part_fd) {
        if (fault("diskfull")) { errno = ENOSPC; return -1; }
        if (fault("shortwrite")) {
            if (!writes++) { errno = EINTR; return -1; }
            if (size > 7) size = 7;
        }
    }
    if (fault("metadata") && fd == metadata_fd) {
        if (writes++) { errno = ENOSPC; return -1; }
        if (size > 20) size = 20;
    }
    return syscall(SYS_write, fd, buffer, size);
}

/* Simulate a delayed storage error after writes succeeded but before publication. */
int fsync(int fd)
{
    if (fault("fsync") && fd == part_fd) { errno = EIO; return -1; }
    return (int)syscall(SYS_fsync, fd);
}
