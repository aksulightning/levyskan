#!/bin/sh
# Isolated integration tests. jq/gzip are test tools, not application dependencies.
set -eu
TZ=UTC0
export TZ
cd "$(dirname "$0")/.."
bin="$PWD/levyskan"
for tool in jq gzip sha256sum; do command -v "$tool" >/dev/null || { echo "Missing test tool: $tool" >&2; exit 1; }; done
work=$(mktemp -d "${TMPDIR:-/tmp}/levyskan-test.XXXXXX")
trap 'rm -rf "$work"' EXIT HUP INT TERM
passed=0
# Count and report one successful integration-test group.
pass() { passed=$((passed + 1)); printf 'PASS %s\n' "$*"; }
# Run a command and fail the suite if its exit status differs from the expected value.
expect() {
    expected=$1; shift
    actual=0
    "$@" >"$work/stdout" 2>"$work/stderr" || actual=$?
    if [ "$actual" -ne "$expected" ]; then
        cat "$work/stdout" "$work/stderr" >&2
        echo "Expected exit $expected, got $actual: $*" >&2; exit 1
    fi
}
# Back up disposable fixtures with both development-only safety overrides.
backup() { "$bin" backup --source "$1" --dest "$2" --allow-writable-source --allow-same-filesystem; }
# Scan disposable fixtures with both development-only safety overrides.
scan() { "$bin" scan --source "$1" --dest "$2" --allow-writable-source --allow-same-filesystem; }
# List completed metadata files beneath the supplied destination.
metadata() { find "$1/.levyskan" -name metadata.jsonl -type f; }
# Check a jq predicate against completed JSONL records in a destination.
json() { find "$1/.levyskan" -name metadata.jsonl -type f -exec cat {} + | jq -es "$2" >/dev/null; }

${CC:-cc} -std=c11 -Wall -Wextra -Wpedantic -fPIC -shared tests/fault.c -o tests/fault.so
${CC:-cc} -D_GNU_SOURCE -D_FILE_OFFSET_BITS=64 -std=c11 -Wall -Wextra -Wpedantic -Isrc tests/unit.c src/utils.c src/linux.c src/preview.c src/metadata.c src/json.c -o tests/unit
./tests/unit
pass 'unit boundaries'

source="$work/source"; dest="$work/dest"
mkdir -p "$source/nested/deeper" "$dest"
printf 'Hello maailma — äö!\nSecond line\n' > "$source/UTF-8.txt"
printf '\211PNG\015\012\032\012\000\000\000\015IHDR' > "$source/binary"
: > "$source/empty"
printf 'no newline' > "$source/no-newline"
awk 'BEGIN {for(i=0;i<300;i++)printf "x";printf "\n"}' > "$source/long"
printf 'space' > "$source/file with spaces"
printf 'quote' > "$source/file\"quote"
printf 'slash' > "$source/file\\slash"
newline=$(printf 'line\nbreak'); printf 'newline' > "$source/$newline"
invalid=$(printf 'invalid-\377'); printf 'invalid path' > "$source/$invalid"
printf 'nested' > "$source/nested/deeper/file"
dd if=/dev/zero of="$source/large" bs=1048576 count=16 2>/dev/null
ln -s UTF-8.txt "$source/symlink"
ln -s nested "$source/directory-link"
mkfifo "$source/fifo"
find "$source" -type f -exec touch -t 202001020304.05 {} +
touch -t 202110030405.06 "$source/UTF-8.txt"
before=$(find "$source" -type f -exec stat -c '%n %s %Y %a %u %g' {} +)
expect 2 "$bin" backup --source "$source" --dest "$dest"
[ ! -e "$dest/.levyskan" ]
pass 'strict default rejects writable source without creating metadata'
expect 2 "$bin" backup --source "$source" --dest "$dest" --allow-writable-source
pass 'separate filesystem default'
expect 2 "$bin" scan --source "$work/missing" --dest "$dest"
pass 'missing source directory'
mkdir "$source/inside"
expect 2 backup "$source" "$source/inside"
expect 2 backup "$source" "$work"
rmdir "$source/inside"
pass 'overlapping source/destination rejected'
expect 0 backup "$source" "$dest"
json "$dest" 'all(.status != "error") and ([.[]|select(.status=="ok")]|length==12)'
json "$dest" 'all(.[]|select(.backup!=null); .backup|startswith("2021-10-03/"))'
json "$dest" 'any(.filename=="UTF-8.txt" and .preview=="Hello maailma — äö!" and .preview_type=="text")'
json "$dest" 'any(.filename=="binary" and .preview=="89 50 4E 47 0D 0A 1A 0A 00 00 00 0D 49 48 44 52" and .preview_type=="hex")'
json "$dest" 'any(.filename=="empty" and .preview=="" and .sha256=="e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855")'
json "$dest" 'any(.filename=="no-newline" and .preview=="no newline" and .preview_truncated==false)'
json "$dest" 'any(.filename=="long" and (.preview|length)==256 and .preview_truncated==true)'
json "$dest" '([.[]|select(.status=="skipped_symlink")]|length==2) and any(.status=="skipped_special")'
json "$dest" 'any(.filename=="line\nbreak") and any(.path_hex|endswith("696e76616c69642dff"))'
after=$(find "$source" -type f -exec stat -c '%n %s %Y %a %u %g' {} +)
[ "$before" = "$after" ]
pass 'text/binary/empty/long previews, unusual filenames, symlink/FIFO skips, date, source attributes'

prefix="$dest/2021-10-03$source"
for name in UTF-8.txt binary empty no-newline long 'file with spaces' 'file"quote' 'file\slash' "$newline" "$invalid" nested/deeper/file large; do
    gzip -t "$prefix/$name.gz"
    gzip -dc "$prefix/$name.gz" > "$work/restored"
    cmp "$source/$name" "$work/restored"
done
hash=$(sha256sum "$source/large" | cut -d ' ' -f1)
json "$dest" "any(.filename==\"large\" and .size==16777216 and .sha256==\"$hash\")"
expect 0 "$bin" verify --dest "$dest"
pass 'streaming 16 MiB backup, gzip compatibility, external SHA-256 and verification'
expect 0 backup "$source" "$dest"
json "$dest" '([.[]|select(.status=="reused")]|length==12)'
[ "$(find "$dest" -name '*.gz' | wc -l)" -eq 13 ] # newline name occupies two lines
pass 'identical existing backup reused'
printf 'different content' > "$source/no-newline"
touch -t 202001020304.05 "$source/no-newline"
expect 0 backup "$source" "$dest"
[ "$(gzip -dc "$prefix/no-newline.gz")" = 'no newline' ]
[ "$(gzip -dc "$prefix/no-newline.v1.gz")" = 'different content' ]
pass 'different content preserved with version suffix'
mv "$source" "$work/source-offline"
expect 0 "$bin" verify --dest "$dest"
mv "$work/source-offline" "$source"
pass 'verification without source access'

cp "$prefix/empty.gz" "$work/good.gz"
printf 'garbage' >> "$prefix/empty.gz"
expect 1 "$bin" verify --dest "$dest"
cp "$work/good.gz" "$prefix/empty.gz"
dd if="$work/good.gz" of="$prefix/empty.gz" bs=1 count=10 2>/dev/null
expect 1 "$bin" verify --dest "$dest"
cat "$work/good.gz" "$work/good.gz" > "$prefix/empty.gz"
expect 1 "$bin" verify --dest "$dest"
printf 'plaintext' > "$prefix/empty.gz"
expect 1 "$bin" verify --dest "$dest"
printf 'wrong content' | gzip > "$prefix/empty.gz"
expect 1 "$bin" verify --dest "$dest"
cp "$work/good.gz" "$prefix/empty.gz"
pass 'garbage, truncation, extra gzip member, plaintext and checksum mismatch rejected'

mkdir "$work/scan" "$work/empty-source" "$work/empty-dest"
expect 0 scan "$source" "$work/scan"
[ -z "$(find "$work/scan" -name '*.gz' -print)" ]
json "$work/scan" 'all(.command=="scan" and .backup==null)'
expect 0 backup "$work/empty-source" "$work/empty-dest"
expect 0 "$bin" verify --dest "$work/empty-dest"
pass 'scan-only and empty source'

mkdir "$work/symlink-dest" "$work/outside"
ln -s "$work/outside" "$work/symlink-dest/2021-10-03"
expect 1 backup "$source" "$work/symlink-dest"
[ -z "$(find "$work/outside" -mindepth 1 -print)" ]
pass 'destination directory symlink cannot redirect output'

mkdir "$work/tamper-source" "$work/tamper-dest"
dd if=/dev/urandom of="$work/tamper-source/random" bs=65536 count=17 2>/dev/null
touch -t 202301020304.05 "$work/tamper-source/random"
expect 0 backup "$work/tamper-source" "$work/tamper-dest"
tamper_meta=$(metadata "$work/tamper-dest")
cp "$tamper_meta" "$work/metadata-original"
tamper_gzip="$work/tamper-dest/2023-01-02$work/tamper-source/random.gz"
gzip -dc "$tamper_gzip" > "$work/restored"
cmp "$work/restored" "$work/tamper-source/random"
expect 0 "$bin" verify --dest "$work/tamper-dest"
pass 'incompressible streaming data across buffer boundaries'
for unsafe in '../escape.gz' '/absolute.gz' 'a/../b.gz' 'a//b.gz' './b.gz'; do
    hex=$(printf '%s' "$unsafe" | od -An -tx1 | tr -d ' \n')
    jq -c --arg hex "$hex" '.backup_hex=$hex' "$work/metadata-original" > "$tamper_meta"
    expect 1 "$bin" verify --dest "$work/tamper-dest"
done
printf '{"schema":' > "$tamper_meta"
expect 1 "$bin" verify --dest "$work/tamper-dest"
cp "$work/metadata-original" "$tamper_meta"
pass 'metadata path traversal and truncated JSON rejected'
mv "$tamper_gzip" "$work/outside-original.gz"
ln -s "$work/outside-original.gz" "$tamper_gzip"
expect 1 "$bin" verify --dest "$work/tamper-dest"
expect 1 backup "$work/tamper-source" "$work/tamper-dest"
gzip -dc "$work/outside-original.gz" > "$work/restored"
cmp "$work/restored" "$work/tamper-source/random"
pass 'terminal gzip symlink rejected and target preserved'

fault_source="$work/fault-source"
mkdir "$fault_source"
dd if=/dev/zero of="$fault_source/target" bs=65536 count=3 2>/dev/null
printf 'survivor' > "$fault_source/survivor"
touch -t 202201020304.05 "$fault_source/target" "$fault_source/survivor"
for fault in unreadable missing io changed diskfull fsync interrupt metadata eintr shortwrite; do
    fault_dest="$work/fault-$fault"; mkdir "$fault_dest"
    expected=1
    case "$fault" in interrupt) expected=143;; metadata) expected=2;; eintr|shortwrite) expected=0;; esac
    expect "$expected" env LD_PRELOAD="$PWD/tests/fault.so" LEVYSKAN_TEST_FAULT="$fault" "$bin" backup \
        --source "$fault_source" --dest "$fault_dest" --allow-writable-source --allow-same-filesystem
    case "$fault" in
    eintr|shortwrite) expect 0 "$bin" verify --dest "$fault_dest";;
    interrupt|metadata)
        [ -n "$(find "$fault_dest" -name metadata.partial -print)" ]
        expect 1 "$bin" verify --dest "$fault_dest";;
    *)
        json "$fault_dest" 'any(.status=="error")'
        [ ! -f "$fault_dest/2022-01-02$fault_source/target.gz" ]
        if [ "$fault" != diskfull ] && [ "$fault" != fsync ]; then
            json "$fault_dest" 'any(.filename=="survivor" and .status=="ok")'
        fi;;
    esac
    [ -z "$(find "$fault_dest" -name file.part -print)" ]
    pass "fault injection: $fault"
done
printf '%s test groups passed\n' "$passed"
