#!/bin/sh
# A real FreeBSD sysroot, including its static libc++ and libcxxrt. No host
# headers or Linux runtime libraries may enter the cross-built executable.
set -eu
root="$PWD/build/freebsd-sysroot"
if [ -f "$root/14.5-aarch64" ]; then
    exit 0
fi
source="$(mktemp -d)"
trap 'rm -rf "$source"' EXIT
release=releases/arm64/aarch64/14.5-RELEASE
fetched=0
for mirror in https://download.freebsd.org https://ftp.de.freebsd.org/pub/FreeBSD \
    https://mirrors.mit.edu/FreeBSD; do
    if curl -fLsS --retry 2 --connect-timeout 20 "$mirror/$release/MANIFEST" -o "$source/MANIFEST" &&
        curl -fLsS --retry 2 --connect-timeout 20 "$mirror/$release/base.txz" -o "$source/base.txz"; then
        fetched=1
        break
    fi
done
[ "$fetched" = 1 ]
digest="$(awk '$1 == "base.txz" {print $2}' "$source/MANIFEST")"
[ "${#digest}" -eq 64 ]
if command -v sha256sum >/dev/null; then
    printf '%s  %s\n' "$digest" "$source/base.txz" | sha256sum -c -
else
    printf '%s  %s\n' "$digest" "$source/base.txz" | shasum -a 256 -c -
fi
mkdir -p "$root"
tar -xJf "$source/base.txz" -C "$root" ./usr/include ./usr/lib ./lib
touch "$root/14.5-aarch64"
