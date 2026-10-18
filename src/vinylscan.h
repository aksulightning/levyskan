/* Shared contracts and owned run state; API rationale is indexed in RESEARCH.md. */
#ifndef VINYLSCAN_H
#define VINYLSCAN_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/types.h>

#define IO_SIZE 65536
#define PREVIEW_SAMPLE 4096
#define MAX_DEPTH 256
#define RECORD_LIMIT (16U * 1024U * 1024U)

_Static_assert(sizeof(off_t) >= 8, "Vinylscan requires 64-bit file offsets");

typedef struct {
    unsigned char *sample; /* Borrowed stream-local buffer, never retained in inventory. */
    size_t length;
    char text[257];
    const char *type;
    bool truncated;
} Preview;

typedef struct {
    char *relative, *path, *backup;
    struct stat stat;
    const char *status;
    char *error;
    char sha256[65];
    Preview preview;
} Entry;

/* Owns entries and their path/error strings; status and preview.type are literals. */
typedef struct {
    Entry *entries;
    size_t count, capacity;
} Inventory;

typedef struct {
    int source_fd, dest_fd, run_fd, metadata_fd, log_fd;
    uint64_t source_mount, dest_mount;
    char *source, *dest;
    char run_name[80], date[32];
    bool writable, same_filesystem, backup, fatal;
    size_t processed, skipped, errors;
} Run;

typedef struct {
    char *backup_hex, *sha256, *status, *command;
    uint64_t size;
} VerifyRecord;

extern volatile sig_atomic_t interrupted;
/* Allocation failure exits with an incomplete run; returned strings are caller-owned. */
void *allocate(size_t size);
void set_emergency_log(int fd);
char *copy_string(const char *s);
char *format_string(const char *format, ...);
ssize_t read_retry(int fd, void *buffer, size_t size);
int write_all(int fd, const void *buffer, size_t size);
int mount_id(int fd, uint64_t *id);
int rename_no_replace(int from, const char *old_name, int to, const char *new_name);
int check_source(Run *run, int fd);
int open_root(const char *path);
int open_directory(int parent, const char *name, bool create, uint64_t mount);
/* Returns an owned parent fd and allocated leaf on success, -1/errno on failure.
 * The root is borrowed. Only destination callers may request create=true. */
int open_parent(int root, const char *path, bool create, uint64_t mount, char **leaf);
bool same_stat(const struct stat *a, const struct stat *b);
int create_run(Run *run, const char *prefix);
void log_error(Run *run, const char *path, const char *message);
void entry_error(Entry *entry, const char *message);
int scan_tree(Run *run, Inventory *inventory);
int select_backup_date(const Inventory *inventory, char date[32]);
void free_inventory(Inventory *inventory);
void preview_add(Preview *preview, const unsigned char *data, size_t length);
void preview_finish(Preview *preview);
size_t utf8_character(const unsigned char *s, size_t length);
/* Fds are borrowed. output=-1 selects scan-only. Hash/preview require success;
 * the caller must still validate source identity/timestamps before trusting them. */
int stream_file(int source, int output, Entry *entry, uint64_t *bytes);
/* Borrow fd at its current offset; accept exactly one gzip member within maximum.
 * On 0, hash and size are valid; on -1, errno describes the failure. */
int hash_gzip(int fd, char hash[65], uint64_t *size, uint64_t maximum);
int metadata_write(Run *run, const Entry *entry);
void json_string(FILE *file, const char *value);
char *hex_encode(const char *value);
char *hex_decode(const char *value);
/* On success, record owns strings released by free_record(); failure frees them.
 * Path decoding and filesystem confinement remain the verifier's responsibility. */
int parse_record(const char *line, VerifyRecord *record);
void free_record(VerifyRecord *record);
int process_inventory(Run *run, Inventory *inventory);
int verify_backups(Run *run);

#endif
