/* Linux ABI wrappers isolated from libc's possibly older struct statx. */
#include <errno.h>
#include <fcntl.h>
#include <linux/stat.h>
#include <stdint.h>
#include <sys/syscall.h>
#include <unistd.h>

/* st_dev alone cannot distinguish same-device bind mounts. Refuse operation if
 * the kernel cannot supply a mount ID; never silently weaken the boundary check. */
int mount_id(int fd, uint64_t *id)
{
    struct statx value;
    if (syscall(SYS_statx, fd, "", AT_EMPTY_PATH | AT_SYMLINK_NOFOLLOW,
                STATX_MNT_ID, &value) < 0) return -1;
    if (!(value.stx_mask & STATX_MNT_ID)) { errno = ENOTSUP; return -1; }
    *id = value.stx_mnt_id;
    return 0;
}

int rename_no_replace(int from, const char *old_name, int to, const char *new_name)
{
    /* RENAME_NOREPLACE = 1; musl does not expose every Linux syscall wrapper. */
    return (int)syscall(SYS_renameat2, from, old_name, to, new_name, 1U);
}
