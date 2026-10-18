/* CLI and run lifecycle. All source checks precede destination writes. */
#include "vinylscan.h"
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <unistd.h>

/* Share one usage description between explicit help (stdout) and bad arguments
 * (stderr), including the safety defaults and observable exit-code contract. */
static void usage(FILE *file)
{
    fputs("Vinylscan 0.1\n"
          "Usage: vinylscan scan|backup --source DIR --dest EXISTING_DIR [options]\n"
          "       vinylscan verify --dest EXISTING_DIR\n"
          "       vinylscan --help\n\n"
          "Default: read-only mounted source; separate destination filesystem.\n"
          "  --allow-writable-source  Permit possible source access-time updates\n"
          "  --allow-same-filesystem  Permit same filesystem (testing only; requires above)\n"
          "Traversal never follows symlinks or crosses mounts. Dates use UTC.\n"
          "Exit: 0 success, 1 file/integrity errors, 2 usage/setup/incomplete run,\n"
          "      130 SIGINT, 143 SIGTERM.\n", file);
}

/* Only store sig_atomic_t state here. Allocation, logging and cleanup are not
 * async-signal-safe and must wait for the normal processing loop to regain control. */
static void signal_handler(int signal_number)
{
    interrupted = signal_number;
}

/* Compare directory identities, not only strings: catches bind-path aliases. */
static int contains_directory(int ancestor, int child)
{
    struct stat target, current, up;
    if (fstat(ancestor, &target) < 0) return -1;
    int fd = dup(child);
    if (fd < 0) return -1;
    int result = -1;
    for (unsigned depth = 0; depth < 4096; depth++) {
        if (fstat(fd, &current) < 0) break;
        if (target.st_dev == current.st_dev && target.st_ino == current.st_ino) { result = 1; break; }
        int parent = openat(fd, "..", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        if (parent < 0) break;
        if (fstat(parent, &up) < 0) { close(parent); break; }
        if (up.st_dev == current.st_dev && up.st_ino == current.st_ino) { close(parent); result = 0; break; }
        close(fd); fd = parent;
    }
    close(fd);
    return result;
}

/* Establish trusted roots, reject overlap and enforce mount policy before any
 * destination creation. source=NULL selects verification, which never opens a source.
 * Opened roots stay in Run for cleanup if a later check fails. */
static int prepare(Run *run, const char *source, const char *destination)
{
    run->dest = realpath(destination, NULL);
    if (!run->dest || (run->dest_fd = open_root(run->dest)) < 0 || mount_id(run->dest_fd, &run->dest_mount) < 0) return -1;
    if (source) {
        run->source = realpath(source, NULL);
        if (!run->source || (run->source_fd = open_root(run->source)) < 0 ||
            mount_id(run->source_fd, &run->source_mount) < 0) return -1;
        if (check_source(run, run->source_fd) < 0) {
            fputs("Source must be mounted read-only (or explicitly use --allow-writable-source).\n", stderr);
            return -1;
        }
        /* Refuse both overlap directions: output inside input can recurse into
         * itself, and input inside output blurs the source/destination boundary. */
        int inside = contains_directory(run->source_fd, run->dest_fd);
        int outside = contains_directory(run->dest_fd, run->source_fd);
        if (inside < 0 || outside < 0) return -1;
        if (inside || outside) {
            fputs("Source and destination directory trees must not overlap.\n", stderr);
            errno = EINVAL; return -1;
        }
        struct stat from, to;
        if (fstat(run->source_fd, &from) < 0 || fstat(run->dest_fd, &to) < 0) return -1;
        if (from.st_dev == to.st_dev && !run->same_filesystem) {
            fputs("Destination must be on a separate filesystem.\n", stderr);
            errno = EXDEV; return -1;
        }
        if (from.st_dev == to.st_dev && run->source_mount != run->dest_mount) {
            fputs("Same-device mount aliases are refused, even with the test override.\n", stderr);
            errno = EXDEV; return -1;
        }
    }
    /* Serialize cooperative runs, including verification, without a lock file. */
    return flock(run->dest_fd, LOCK_EX | LOCK_NB);
}

/* Coordinate inventory, one date selection and per-file processing. Keep the
 * partial metadata name until all entries have been handled; failures leave it
 * behind as an explicit incomplete-run marker for later verification. */
static int execute_scan(Run *run)
{
    run->metadata_fd = openat(run->run_fd, "metadata.partial", O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (run->metadata_fd < 0 || fsync(run->run_fd) < 0) return -1;
    Inventory inventory = {0};
    puts("Scanning filesystem...");
    int result = scan_tree(run, &inventory);
    if (result < 0) log_error(run, run->source, strerror(errno));
    if (!result) result = select_backup_date(&inventory, run->date);
    if (!result) {
        printf("Entries found: %zu\nBackup date: %s\n", inventory.count, *run->date ? run->date : "none (no eligible files)");
        result = process_inventory(run, &inventory);
    }
    free_inventory(&inventory);
    if (result < 0 || interrupted || run->fatal) return -1;
    if (close(run->metadata_fd) < 0) { run->metadata_fd = -1; return -1; }
    run->metadata_fd = -1;
    /* The final filename means enumeration finished, not that every file passed.
     * Per-entry errors remain in JSONL and produce exit status 1. */
    if (rename_no_replace(run->run_fd, "metadata.partial", run->run_fd, "metadata.jsonl") < 0 ||
        fsync(run->run_fd) < 0) return -1;
    return 0;
}

/* Parse one command, establish the safety boundary, dispatch it and own cleanup.
 * Start with failure status so no early setup/error path can accidentally succeed. */
int main(int argc, char **argv)
{
    Run run = {.source_fd = -1, .dest_fd = -1, .run_fd = -1, .metadata_fd = -1, .log_fd = -1};
    if (argc == 2 && !strcmp(argv[1], "--help")) { usage(stdout); return 0; }
    if (argc < 2) { usage(stderr); return 2; }
    bool verify = !strcmp(argv[1], "verify");
    run.backup = !strcmp(argv[1], "backup");
    if (!verify && !run.backup && strcmp(argv[1], "scan")) { usage(stderr); return 2; }
    static const struct option options[] = {
        {"source", required_argument, NULL, 's'}, {"dest", required_argument, NULL, 'd'},
        {"allow-writable-source", no_argument, NULL, 'w'},
        {"allow-same-filesystem", no_argument, NULL, 'f'},
        {"help", no_argument, NULL, 'h'}, {NULL, 0, NULL, 0}
    };
    const char *source = NULL, *destination = NULL;
    /* argv[1] is the command, so option parsing starts after it. */
    optind = 2;
    int option;
    while ((option = getopt_long(argc, argv, "", options, NULL)) != -1) {
        switch (option) {
        case 's': if (source) { usage(stderr); return 2; } source = optarg; break;
        case 'd': if (destination) { usage(stderr); return 2; } destination = optarg; break;
        case 'w': run.writable = true; break;
        case 'f': run.same_filesystem = true; break;
        case 'h': usage(stdout); return 0;
        default: usage(stderr); return 2;
        }
    }
    if (optind != argc || !destination || (!verify && !source) ||
        (verify && (source || run.writable || run.same_filesystem)) || (run.same_filesystem && !run.writable)) {
        usage(stderr); return 2;
    }
    /* New backups and metadata should not become readable by other users by default. */
    umask(077);
    struct sigaction action = {.sa_handler = signal_handler};
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGINT, &action, NULL) < 0 || sigaction(SIGTERM, &action, NULL) < 0) return 2;
    /* Convert broken output pipes and file-size limits into checked I/O failures
     * instead of terminating before an incomplete result can be recorded. */
    action.sa_handler = SIG_IGN;
    if (sigaction(SIGPIPE, &action, NULL) < 0 || sigaction(SIGXFSZ, &action, NULL) < 0) return 2;
    int exit_code = 2;
    if (prepare(&run, source, destination) < 0) { log_error(&run, "setup", strerror(errno)); goto done; }
    if (create_run(&run, verify ? "verify" : "run") < 0) { log_error(&run, "destination", strerror(errno)); goto done; }
    set_emergency_log(run.log_fd);
    if (run.writable) log_error(&run, "WARNING", "writable source permitted: filesystem metadata changes, including access times, cannot be completely prevented");
    if (run.same_filesystem) log_error(&run, "WARNING", "same-filesystem test override: destination writes affect the source filesystem; use separate media for real backups");
    puts("Vinylscan 0.1");
    if (!verify) printf("Safety mode: %s\n", run.writable ? "writable source explicitly allowed" : "strict read-only");
    int result = verify ? verify_backups(&run) : execute_scan(&run);
    if (result < 0) { log_error(&run, "run incomplete", interrupted ? "interrupted" : strerror(errno)); goto done; }
    exit_code = run.errors ? 1 : 0;
    printf("%s: %zu\nSkipped: %zu\nErrors: %zu\n", verify ? "Verified records" : "Files processed", run.processed, run.skipped, run.errors);
    printf("Run: .vinylscan/%s/\n", run.run_name);
done:
    /* A late flush/close error can invalidate apparent success. SIGINT/SIGTERM
     * take precedence afterward so shells can identify an interrupted command. */
    if (run.metadata_fd >= 0 && close(run.metadata_fd) < 0) exit_code = 2;
    if (run.log_fd >= 0) {
        if (fsync(run.log_fd) < 0) { perror("vinylscan: log fsync"); exit_code = 2; }
        if (close(run.log_fd) < 0) exit_code = 2;
    }
    if (run.run_fd >= 0 && close(run.run_fd) < 0) exit_code = 2;
    if (run.source_fd >= 0) close(run.source_fd);
    if (run.dest_fd >= 0 && close(run.dest_fd) < 0) exit_code = 2;
    free(run.source); free(run.dest);
    if (interrupted) return 128 + interrupted;
    return run.fatal ? 2 : exit_code;
}
