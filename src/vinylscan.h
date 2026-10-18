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

/* Holds borrowed sample bytes and the resulting text or hex preview. */
typedef struct {
    unsigned char *sample; /* Borrowed stream-local buffer, never retained in inventory. */
    size_t length;
    char text[257];
    const char *type;
    bool truncated;
} Preview;

/* Represents one filesystem entry with its processing outcome. */
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

/* Owns run descriptors, paths, safety policies and progress counters. */
typedef struct {
    int source_fd, dest_fd, run_fd, metadata_fd, log_fd;
    uint64_t source_mount, dest_mount;
    char *source, *dest;
    char run_name[80], date[32];
    bool writable, same_filesystem, backup, fatal;
    size_t processed, skipped, errors;
} Run;

/* Owns decoded fields for verifying one metadata record. */
typedef struct {
    char *backup_hex, *sha256, *status, *command;
    uint64_t size;
} VerifyRecord;

/* Set by a signal handler and inspected by normal processing loops. */
extern volatile sig_atomic_t interrupted;
/* Allocation failure exits with an incomplete run; returned strings are caller-owned. */
void *allocate(size_t size);
/* Use a borrowed descriptor for best-effort out-of-memory diagnostics. */
void set_emergency_log(int fd);
/* Duplicate a NUL-terminated string into caller-owned memory. */
char *copy_string(const char *s);
/* Return newly formatted text, or NULL on formatting failure. */
char *format_string(const char *format, ...);
/* Retry interrupted reads unless termination was requested. */
ssize_t read_retry(int fd, void *buffer, size_t size);
/* Write the entire buffer or return -1 with errno after failure. */
int write_all(int fd, const void *buffer, size_t size);
/* Query the Linux mount ID of an opened filesystem object. */
int mount_id(int fd, uint64_t *id);
/* Publish a name atomically without replacing an existing file. */
int rename_no_replace(int from, const char *old_name, int to, const char *new_name);
/* Enforce the source mount and read-only policy for an opened fd. */
int check_source(Run *run, int fd);
/* Open a canonical absolute directory component by component. */
int open_root(const char *path);
/* Open a validated child directory, optionally creating it. */
int open_directory(int parent, const char *name, bool create, uint64_t mount);
/* Returns an owned parent fd and allocated leaf on success, -1/errno on failure.
 * The root is borrowed. Only destination callers may request create=true. */
int open_parent(int root, const char *path, bool create, uint64_t mount, char **leaf);
/* Compare file identity and change markers without access time. */
bool same_stat(const struct stat *a, const struct stat *b);
/* Create private destination run storage and its error log. */
int create_run(Run *run, const char *prefix);
/* Log escaped diagnostics to stderr and the run log; track failures. */
void log_error(Run *run, const char *path, const char *message);
/* Store a copied diagnostic and mark a filesystem entry as failed. */
void entry_error(Entry *entry, const char *message);
/* Inventory the source and sort entries by raw pathname bytes. */
int scan_tree(Run *run, Inventory *inventory);
/* Select the latest eligible mtime as a UTC date, or empty. */
int select_backup_date(const Inventory *inventory, char date[32]);
/* Release all allocated entries, paths and errors in an inventory. */
void free_inventory(Inventory *inventory);
/* Save sample bytes up to the preview limit without allocation. */
void preview_add(Preview *preview, const unsigned char *data, size_t length);
/* Classify the collected sample and render a bounded preview. */
void preview_finish(Preview *preview);
/* Return the width of a complete valid UTF-8 scalar, or zero. */
size_t utf8_character(const unsigned char *s, size_t length);
/* Fds are borrowed. output=-1 selects scan-only. Hash/preview require success;
 * the caller must still validate source identity/timestamps before trusting them. */
int stream_file(int source, int output, Entry *entry, uint64_t *bytes);
/* Borrow fd at its current offset; accept exactly one gzip member within maximum.
 * On 0, hash and size are valid; on -1, errno describes the failure. */
int hash_gzip(int fd, char hash[65], uint64_t *size, uint64_t maximum);
/* Append one bounded JSONL entry to open run metadata. */
int metadata_write(Run *run, const Entry *entry);
/* Write an escaped JSON string or JSON null to a stream. */
void json_string(FILE *file, const char *value);
/* Encode path bytes as caller-owned lowercase hexadecimal text. */
char *hex_encode(const char *value);
/* Decode hex path bytes, rejecting malformed or embedded-NUL data. */
char *hex_decode(const char *value);
/* On success, record owns strings released by free_record(); failure frees them.
 * Path decoding and filesystem confinement remain the verifier's responsibility. */
int parse_record(const char *line, VerifyRecord *record);
/* Release owned verification fields and clear the record. */
void free_record(VerifyRecord *record);
/* Process scanned entries and durably record their outcomes. */
int process_inventory(Run *run, Inventory *inventory);
/* Validate destination metadata and gzip files without source access. */
int verify_backups(Run *run);

#endif
