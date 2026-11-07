#!/bin/bash
# Build a stock iPhone X userspace on HFSX without mounting a host disk.
# Run as root: OS.dmg SYSTEMOS.dmg APPOS.dmg OUT.img [size, default 16G].
set -euo pipefail
[[ $# -ge 4 && $(id -u) == 0 ]] || { echo 'Run as root with OS, SystemOS, AppOS and output paths' >&2; exit 1; }
os=$(realpath "$1")
system=$(realpath "$2")
app=$(realpath "$3")
output=$(realpath "$(dirname "$4")")/$(basename "$4")
[[ ! -e "$output" ]] || { echo "Refusing to overwrite $output" >&2; exit 1; }
apfs=${APFS_FUSE:?Set APFS_FUSE to the apfs-fuse executable}
work=$(mktemp -d /var/tmp/gm9-iosroot.XXXXXX)
mounts=()
root_mounted=0
guest_pid=
cleanup() {
    local busy=0
    if (( root_mounted )); then guestunmount "$work/root" || busy=1; fi
    if [[ -n "$guest_pid" && $busy == 0 ]]; then wait "$guest_pid" || true; fi
    for path in "${mounts[@]}"; do
        if mountpoint -q "$path"; then fusermount3 -u "$path" || busy=1; fi
    done
    # Leave a failed mount in place rather than recurse into a filesystem.
    if (( busy == 0 )); then
        rm -f "$work"/*.dmg "$work/guest.log"
        rmdir "$work"/* "$work" 2>/dev/null || true
    else
        echo "Mount cleanup incomplete; retained $work" >&2
    fi
}
trap cleanup EXIT
attach_apfs() {
    ln -s "$1" "$work/$2.dmg"
    mkdir "$work/$2"
    "$apfs" "$work/$2.dmg" "$work/$2"
    mounts=("$work/$2" "${mounts[@]}")
    [[ -d "$work/$2/root" ]] || { echo "Missing APFS root in $2" >&2; exit 1; }
}
attach_apfs "$os" os
attach_apfs "$system" system
attach_apfs "$app" app
test -f "$work/os/root/sbin/launchd"
test -f "$work/os/root/usr/lib/dyld"
truncate -s "${5:-16G}" "$output"
mkfs.hfsplus -s -v GM9iOSX "$output"
export LIBGUESTFS_BACKEND=direct
import_tree() {
    echo "Importing $1 into $2"
    tar -C "$1" --numeric-owner -cpf - . | guestfish --rw --format=raw \
        -a "$output" -m /dev/sda mkdir-p "$2" : tar-in - "$2"
}
import_tree "$work/os/root" /
import_tree "$work/system/root" /private/preboot/Cryptexes/OS
import_tree "$work/app/root" /private/preboot/Cryptexes/App
mkdir "$work/root"
export LIBGUESTFS_BACKEND=direct
guestmount --rw --no-fork --format=raw -a "$output" -m /dev/sda \
    "$work/root" >"$work/guest.log" 2>&1 &
guest_pid=$!
for ((n=0; n<120; n++)); do
    mountpoint -q "$work/root" && break
    kill -0 "$guest_pid" 2>/dev/null || break
    sleep 1
done
if ! mountpoint -q "$work/root"; then
    cat "$work/guest.log" >&2
    kill "$guest_pid" 2>/dev/null || true
    exit 1
fi
root_mounted=1
r=$work/root
cache=private/preboot/Cryptexes/OS/System/Library/Caches/com.apple.dyld
mkdir -p "$r/System/Library/Caches/com.apple.dyld"
for file in "$r/$cache"/*; do
    ln "$file" "$r/System/Library/Caches/com.apple.dyld/${file##*/}"
done
for cryptex in OS App; do
    while IFS= read -r -d '' path; do
        relative=${path#"$r/private/preboot/Cryptexes/$cryptex/"}
        target=$r/$relative
        if [[ ! -e "$target" && ! -L "$target" ]]; then
            mkdir -p "$(dirname "$target")"
            if [[ -L "$path" ]]; then cp -aP "$path" "$target"; else ln "$path" "$target"; fi
        fi
    done < <(find "$r/private/preboot/Cryptexes/$cryptex" ! -type d \
        ! -path '*/System/Library/Caches/com.apple.dyld/*' -print0)
done
mkdir -p "$r/dev" "$r/private/var/run" "$r/private/var/db"
mkdir -p "$r/private/tmp" "$r/private/var/tmp"
chmod 1777 "$r/private/tmp" "$r/private/var/tmp"
cmp "$work/os/root/sbin/launchd" "$r/sbin/launchd"
cmp "$work/os/root/usr/lib/dyld" "$r/usr/lib/dyld"
sha256sum "$r/sbin/launchd" "$r/usr/lib/dyld"
stat -c '%a %u:%g %n' "$r/sbin/launchd" "$r/usr/lib/dyld"
sync
guestunmount "$work/root"
root_mounted=0
wait "$guest_pid"
guest_pid=
# Linux HFS+ can normalize a few Unicode names differently from Apple's tables.
if ! fsck.hfsplus -f -n "$output"; then
    fsck.hfsplus -f -y "$output"
    fsck.hfsplus -f -n "$output"
fi
echo "Prepared stock iPhone X root: $output"
