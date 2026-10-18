# Vinylscan 0.1

A conservative C11 command-line scanner and backup utility for Linux, targeting
Alpine/musl. Every regular file gets its own standard gzip file. Metadata is
human-readable JSON Lines; no tar, Python, SQLite, service, or network is needed.
The priorities are source safety, integrity, simple implementation and explicit
failure reporting. This is an early prototype, not a recovery tool.

## Build and test

```sh
apk add build-base linux-headers zlib-dev openssl-dev
make
./vinylscan --help
make docs

# Additional test tools only:
apk add jq gzip
make test
make clean
```

Runtime libraries: libc, zlib and OpenSSL libcrypto. Linux 5.8 or later is required
for mount-ID checks. The destination must support directory `fsync`, advisory
`flock`, and `renameat2(RENAME_NOREPLACE)`; unsupported operations fail safely.
Linux syscall wrappers are isolated from libc's version of `struct statx`.
No `/proc` mount is needed by the application.

The shell/C suite creates disposable temporary directories and covers UTF-8,
binary and empty files; preview boundaries; unusual filename bytes; nesting;
16 MiB streaming; gzip and SHA-256 compatibility; missing/unreadable files;
symlinks/FIFOs; date selection; existing versions; corruption; interruptions;
changed files; short I/O; disk-full, sync and metadata failures. Deterministic
fault injection is a separate shared library, never linked into Vinylscan.
Tests require a writable temporary filesystem (`TMPDIR` can select one).
`make docs` checks for required source and function comments; `make test` and CI
run the same check. Comment accuracy still requires human review.
`.github/workflows/ci.yml` builds and tests in Alpine, with a separate mount test.

## Usage

Prepare an existing, writable destination on a different filesystem. Arrange a
read-only source mount yourself; Vinylscan never mounts, remounts or repairs it.

```sh
./vinylscan scan --source /mnt/source --dest /mnt/backup
./vinylscan backup --source /mnt/source --dest /mnt/backup
./vinylscan verify --dest /mnt/backup
```

`scan` reads regular file contents to calculate previews and checksums, but
creates no gzip files. `backup` inventories metadata first, then reads each
eligible regular file once, sequentially, in 64 KiB buffers. SHA-256, preview
collection and gzip compression share that read. Entries are processed in bytewise
pathname order. `verify` reads all saved run metadata and gzip files, never the
source. It checks SHA-256, decompressed length, gzip CRC/trailer and complete stream
termination. Plaintext, trailing junk and concatenated members are rejected.
Verification of multiple runs may read the same reused backup more than once.

For development on writable temporary directories, explicitly opt out:

```sh
mkdir source destination
printf 'Hello world!\n' > source/example.txt
./vinylscan backup --source source --dest destination \
  --allow-writable-source --allow-same-filesystem
./vinylscan verify --dest destination
```

Both overrides print warnings and record them in the run log. The separate
`--allow-same-filesystem` test option requires `--allow-writable-source` and permits
destination writes on the same filesystem; do not use it for real backups.
Same-device mount aliases are refused even with this override.

Exit codes: **0** complete success; **1** file, metadata-record or integrity errors;
**2** invalid arguments, setup failure or incomplete/fatal run; **130** SIGINT;
**143** SIGTERM. Expected symlink, special-file and mount skips are not errors.
Errors go to stderr and the destination run log. Setup errors before a safe
destination can be established are stderr-only. Failure to write a log is fatal.

## Layout, dates and existing files

```text
/mnt/backup/
  2026-10-15/home/user/photos/image.png.gz
  2026-10-15/home/user/photos/image.png.v1.gz
  .vinylscan/run-<random-id>/metadata.jsonl
  .vinylscan/run-<random-id>/run.log
  .vinylscan/verify-<random-id>/run.log
```

One date directory is used for the entire run: the **UTC date of the latest
modification time among regular files eligible at inventory time**. This policy
is isolated in `select_backup_date()`. A file subsequently failing to open can
still determine that date. Directory and skipped-entry timestamps do not count.
Empty sources produce empty metadata and no date directory. The execution date
is never substituted. Paths preserve the canonical absolute source tree below
the date directory, including the source mountpoint itself.

Output is first written as `file.part` in the private destination run directory,
then flushed and published with an atomic, no-replace rename. Directory changes
are synced. An existing gzip with identical decompressed content is reused after
SHA-256/size checks. Different or corrupt existing data is preserved, and the
first available `.v1.gz`, `.v2.gz`, ... suffix is tried (maximum 9,999 variants).
Unsafe existing objects and read errors fail the affected file. Gzip files use
mode 0600 and created directories 0700; source permissions are recorded, not applied.

## JSONL format

Each filesystem entry occupies one complete JSON line. Directories, skipped
objects and errors are recorded too. Example (abbreviated):

```json
{"schema":1,"command":"backup","path":"/home/user/file.txt","size":6,"mtime":"2026-10-15T14:32:11Z","mode":420,"preview_type":"text","preview":"Hello","preview_truncated":false,"backup":"2026-10-15/home/user/file.txt.gz","status":"ok"}
```

Actual records also include `filename`, `path_hex`, `backup_hex`, `mtime_ns`,
`sha256` and `error`. `mode` is the permission/special-bit mask as a decimal
integer (420 = octal 0644); sizes are 64-bit; `mtime_ns` is the fractional second.
SHA-256 describes the original uncompressed bytes. `backup` is destination-relative;
scan records and entries without published backups have a null backup field.
An error after publication, such as directory-sync failure, can retain the path
of the unconfirmed output. `error` is null unless something failed.

Statuses: `ok`, `reused`, `directory`, `skipped_symlink`, `skipped_special`,
`skipped_mount`, `error`. Failed or changed reads have no trusted hash or preview.
Strings escape quotes, backslashes and control characters. Valid UTF-8 is kept;
invalid pathname bytes are displayed as `\u00XX`. **`path_hex` and `backup_hex`
are the authoritative lossless encodings of the original pathname bytes**, with
no terminating NUL. Verification decodes `backup_hex` and rejects absolute paths,
empty components, `.`/`..`, symlinks and mount crossings. Never reconstruct an
invalid-UTF-8 path from the display string.

Text detection examines the first 4,096 bytes: valid UTF-8 without NUL, DEL,
C1 controls or ASCII controls except tab/CR/LF. A potentially incomplete UTF-8
character at the sample boundary is ignored for classification. Text preview
ends at CR/LF or 256 bytes, without splitting a code point; truncation is recorded.
Binary previews contain up to 16 uppercase hexadecimal bytes. An empty file is
empty text. Data beyond the sample can be binary: this is a heuristic, not MIME detection.

```sh
cat /mnt/backup/.vinylscan/run-*/metadata.jsonl
jq -c 'select(.status == "error")' /mnt/backup/.vinylscan/run-*/metadata.jsonl
```

Records are synced as they are written. A completed enumeration, including any
per-file failures, publishes `metadata.jsonl`. Interrupted/fatal runs retain
`metadata.partial`; verify reports them as incomplete and checks available full
records. A crash can leave an orphan gzip or `.part` file. No automatic cleanup
deletes them; inspect them manually. Metadata is limited to 16 MiB per record
and uses a flat versioned schema. It is not required for manual decompression.

## Source safety and limits

A read-only file descriptor is **not** a read-only filesystem mount: reads can
update access times. Strict mode checks the source mount's read-only flag before
enumeration and on opened files/directories. Content descriptors use `O_RDONLY`
and `O_NOFOLLOW`; symlinks are recorded, not followed. Sockets, FIFOs and devices
are skipped. Mount IDs prevent traversal into nested mounts, including bind mounts
on the same device. Root arguments are resolved once to canonical directories.
Source and destination trees must not overlap in either direction.

Vinylscan never writes, deletes, renames, chmods, chowns, repairs or creates
temporary files on the source tree. Size, inode, mode, nanosecond mtime/ctime and
the final directory entry are compared with the inventory to detect changes.
This does **not** provide a snapshot of a live filesystem; metadata races, changes
between checks and changes outside the sampled interval can remain undetected.
Keep mount topology stable. A read-only bind view cannot stop another writable
mount or another machine from changing the underlying filesystem.

The destination and its administrators must be trusted. Descriptor-relative,
no-follow operations block symlink/path-traversal redirection; they cannot defend
against a privileged attacker moving already-open directories or remounting them.
An advisory lock serializes Vinylscan runs, not arbitrary external writers.
Checksums detect damage but do not authenticate editable metadata or identify
deleted records. Always keep independent copies of valuable data.

Reading a physically damaged disk can cause further hardware stress, even through
a read-only mount. Vinylscan cannot guarantee recovery from damaged hardware.
It has no repair operations and no automatic retry policy beyond interrupted I/O.

Other limits: inventory memory grows with entry count and pathname lengths;
directory depth is capped at 256; the gzip suffix must fit the destination's
filename limit. Sparse layout, hard-link relationships, ownership, ACLs, xattrs,
empty-directory restoration and original filesystem metadata are not restored.
Pseudo-filesystems with synthetic sizes are not suitable backup sources. A disk
full error can leave destination directories but never replaces a successful gzip.
Durability still depends on filesystem and hardware behavior.

## Restore one file

Create a separate restore directory and use ordinary gzip. This example refuses
to overwrite an existing restored file; inspect its exit status before using it.

```sh
mkdir -p /mnt/restore
(set -C; gzip -dc -- /mnt/backup/2026-10-15/home/user/photos/image.png.gz \
  > /mnt/restore/image.png)
```

There is no built-in restore command in 0.1. Future work could add atomic
single-file restoration, metadata restoration, bounded on-disk inventories and
explicit snapshot integration. Cloud storage, daemons, encryption and filesystem
repair are outside this prototype's scope.

See `RESEARCH.md` for reviewed API references and `TESTING.md` for executed checks.
