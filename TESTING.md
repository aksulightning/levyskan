# Validation record — 2026-10-18

Executed locally on Ubuntu 24.04/glibc, x86-64, GCC 13.3.0:

- Clean C11 build with `-Wall -Wextra -Wpedantic -Werror`: passed, no warnings.
- `TMPDIR="$PWD" make test`: all **26 test groups** passed.
- Standalone preview/JSON tests under UndefinedBehaviorSanitizer: passed,
  including 10,000 deterministic malformed-record mutations.
- `sh -n tests/test.sh tests/mounts.sh`: passed.
- `git diff --check`: passed for tracked changes.

The suite checks normal scan/backup/verification; gzip decompression with the
system gzip; an external `sha256sum` comparison; 16 MiB sequential input;
incompressible input crossing I/O boundaries; UTF-8 and binary previews;
empty/no-newline/long-line files; UTF-8-safe truncation; quotes, spaces,
backslashes, newlines and invalid UTF-8 pathname bytes; nested paths; strict
writable-source refusal; separate-filesystem and overlap checks; symlink/FIFO
skips; consistent newest-mtime date selection; identical backup reuse;
version preservation; source-offline verification; damaged/truncated/plaintext/
concatenated/trailing-junk gzip; invalid JSON and malicious metadata paths;
destination directory and final-file symlink rejection.

Test-only injected failures cover EACCES, files disappearing between inventory
and open, EIO after a partial read, a real test-file timestamp change during
reading, ENOSPC, failed output fsync, SIGTERM, partially written metadata, EINTR,
short reads and short writes. Unreadable-file behavior is injected so the test
remains meaningful when run as root. Failed files are not published successfully;
other files continue when safe; partial runs remain identifiable.

Not verified in this environment:

- Alpine/musl compilation or the GitHub Actions workflow. The supplied CI builds
  on Alpine 3.22. Nothing was pushed or run on GitHub.
- Actual read-only/bind-mount integration: `tests/mounts.sh` is supplied for an
  isolated privileged mount namespace and configured in CI. Local ordinary tests
  exercise strict writable-source refusal, not actual read-only mount acceptance.
- AddressSanitizer/LeakSanitizer did not complete: the sandbox has no `/proc`,
  and LeakSanitizer reported that it could not inspect process tasks. No clean
  ASan/LSan result is claimed.
- Physical media faults, filesystem disconnection, power-loss durability, files
  larger than 4 GiB, every Linux filesystem or hostile privileged concurrent writers.

Reproduce the UBSan unit check with:

```sh
cc -D_GNU_SOURCE -D_FILE_OFFSET_BITS=64 -std=c11 -Wall -Wextra -Wpedantic \
  -g -O1 -fsanitize=undefined -fno-omit-frame-pointer -Isrc \
  tests/unit.c src/utils.c src/linux.c src/preview.c src/metadata.c src/json.c \
  -o tests/unit
./tests/unit
```
