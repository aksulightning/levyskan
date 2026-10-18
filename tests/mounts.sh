#!/bin/sh
# Optional privileged tests: run only in a private disposable mount namespace.
set -eu
if [ "${VINYLSCAN_MOUNT_TESTS:-}" != 1 ]; then
    echo 'Set VINYLSCAN_MOUNT_TESTS=1 inside a disposable private mount namespace.' >&2
    exit 2
fi
mount --make-rprivate /
cd "$(dirname "$0")/.."
bin="$PWD/vinylscan"
work=$(mktemp -d)
# Unmount disposable fixtures before removing their temporary directory.
cleanup() {
    umount -R "$work" 2>/dev/null || true
    rm -rf "$work"
}
trap cleanup EXIT
mount -t tmpfs tmpfs "$work"
mkdir "$work/source" "$work/dest" "$work/alias"
mount -t tmpfs tmpfs "$work/source"
mkdir "$work/source/nested"
printf 'root file\n' > "$work/source/file"
mount -t tmpfs tmpfs "$work/source/nested"
printf 'unrelated filesystem\n' > "$work/source/nested/skip"
mount -o remount,ro "$work/source"
"$bin" backup --source "$work/source" --dest "$work/dest"
jq -es 'any(.status=="skipped_mount") and any(.filename=="file" and .status=="ok")' "$work/dest"/.vinylscan/run-*/metadata.jsonl
"$bin" verify --dest "$work/dest"
echo 'PASS actual read-only mount and separate-filesystem traversal boundary'
umount "$work/source/nested"
mount -o remount,rw "$work/source"
mkdir "$work/source/bind" "$work/source/data"
printf 'same-device bind\n' > "$work/source/data/file"
mount --bind "$work/source/data" "$work/source/bind"
mount -o remount,ro "$work/source"
"$bin" scan --source "$work/source" --dest "$work/dest"
jq -es 'any(.status=="skipped_mount" and .filename=="bind")' "$work/dest"/.vinylscan/run-*/metadata.jsonl
echo 'PASS same-device bind mount skipped'
mount --bind "$work/source/data" "$work/alias"
if "$bin" backup --source "$work/source" --dest "$work/alias" --allow-writable-source --allow-same-filesystem; then
    echo 'FAIL destination bind alias accepted' >&2; exit 1
fi
echo 'PASS destination bind alias refused'
