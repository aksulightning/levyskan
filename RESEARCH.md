# Implementation references

Reviewed 2026-10-18. No external implementation code was copied.

- [zlib manual](https://zlib.net/manual.html): streaming `deflateInit2`/`inflateInit2`,
  gzip framing, stream termination, and the permissive behavior of `gzread`.
  Used the lower-level API so verification rejects trailing garbage and extra members.
- [Linux statx(2)](https://man7.org/linux/man-pages/man2/statx.2.html): descriptor
  queries, `AT_EMPTY_PATH`, mount identifiers and Linux 5.8's `STATX_MNT_ID`.
  Used mount identities rather than device numbers alone for traversal boundaries.
- [Linux statvfs(3)](https://man7.org/linux/man-pages/man3/statvfs.3.html):
  `fstatvfs` and `ST_RDONLY`, used for the strict source mount check.
- [OpenSSL EVP digest API](https://docs.openssl.org/3.0/man3/EVP_DigestInit/):
  streaming digest initialization, update and finalization. Used EVP SHA-256;
  no cryptographic implementation was written for this project.

Design choices, rather than guarantees from those references: UTC dates, bounded
previews, two-pass inventory/content processing, lossless hexadecimal path fields,
flat JSONL, version suffixes and conservative mount-boundary refusal.
Direct musl header URLs were unavailable during research; native Alpine testing
is delegated to the supplied CI configuration, not claimed as an executed result.
