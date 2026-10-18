# Research sources and implementation decisions

These are web sources actually accessed, not suggested further reading. R1–R4
were reviewed during the initial implementation and included in its first commit.
R5–R6 were reviewed during the subsequent code-comment review. No external
implementation code was copied; cryptography and compression use library APIs.

## Reviewed sources

| ID | Source title and URL | Access date | Finding used | Implementation |
| --- | --- | --- | --- | --- |
| R1 | [zlib 1.3.1 Manual](https://zlib.net/manual.html) | 2026-10-18 | `deflateInit2`/`inflateInit2` support streaming gzip; `Z_STREAM_END` identifies a member boundary. `gzread` permits plaintext and trailing garbage. | `src/stream.c`: gzip framing, streaming compression and strict single-member verification. |
| R2 | [Linux statx(2) manual](https://man7.org/linux/man-pages/man2/statx.2.html) | 2026-10-18 | `AT_EMPTY_PATH` queries an open descriptor; `STATX_MNT_ID` is available since Linux 5.8. | `src/linux.c`, `src/scanner.c`, `src/utils.c`: identify mount boundaries, including same-device bind mounts; refuse missing mount IDs. |
| R3 | [Linux statvfs(3) manual](https://man7.org/linux/man-pages/man3/statvfs.3.html) | 2026-10-18 | `fstatvfs` reports filesystem flags, including `ST_RDONLY`. | `src/utils.c`: check the opened source's mount flags before reading; require the explicit override for writable mounts. |
| R4 | [OpenSSL EVP_DigestInit documentation](https://docs.openssl.org/3.0/man3/EVP_DigestInit/) | 2026-10-18 | EVP supports incremental digest initialization, updates and finalization. | `src/stream.c`: calculate SHA-256 with OpenSSL while streaming; no custom cryptographic algorithm. |
| R5 | [Linux open(2)/openat(2) manual](https://man7.org/linux/man-pages/man2/open.2.html) | 2026-10-18 | `openat` resolves relative to a directory descriptor; `O_NOFOLLOW` protects the final component; `O_PATH` allows inspection without ordinary file I/O. | `src/utils.c`: open each directory component separately; `src/scanner.c`: inspect types; `src/backup.c`: use nonblocking opens before rejecting FIFO replacements. |
| R6 | [Linux rename(2)/renameat2(2) manual](https://man7.org/linux/man-pages/man2/rename.2.html) | 2026-10-18 | `RENAME_NOREPLACE` refuses to replace an existing destination and requires filesystem support. | `src/linux.c`, `src/backup.c`, `src/main.c`: publish gzip and completed metadata without an overwrite-prone existence-check/rename sequence. |

## Project policy and limits

UTC dates, a 4,096-byte preview sample, two-pass inventory/content processing,
lossless hexadecimal path fields, flat JSONL and version suffixes are Levyskan
design choices. The sources document APIs; they do not establish that this
program provides a snapshot or guarantees recovery or power-loss durability.
Mount checks are point-in-time, and a trusted, stable destination remains necessary.

Direct musl header pages could not be retrieved during the initial research;
their contents were not treated as reviewed evidence. Alpine compatibility was
subsequently tested rather than inferred from those unavailable pages:
[CI run 39120604851](https://github.com/aksulightning/vinylscan/actions/runs/39120604851)
passed on 2026-10-18 for commit `12aa1a5324ce9d7f65d9085bfd124d6ac9f63bfb`.
It covered the Alpine/musl build, all 26 test groups and actual read-only/bind-mount
checks. See `TESTING.md` for the remaining unverified cases.
