#!/usr/bin/env bash
# Downloads the `libraries:` a Bungee plugin jar declares into <out>/lib; direct coordinates with a SHA-1 only.
set -euo pipefail
jar="$1"
out="${2:-.basalt/bungee}"
mkdir -p "$out/lib"
sha1_of() { if command -v sha1sum >/dev/null; then sha1sum "$1"; else shasum -a 1 "$1"; fi; }
descriptor="$(unzip -p "$jar" bungee.yml 2>/dev/null || unzip -p "$jar" plugin.yml)"
printf '%s\n' "$descriptor" | awk '/^libraries:/{on=1;next} on&&/^[ \t]*-/{gsub(/^[ \t]*-[ \t]*["'\'']?|["'\'']?[ \t\r]*$/,"");print;next} on{on=0}' |
while read -r coord; do
    [ -z "$coord" ] && continue
    IFS=: read -r group artifact version <<<"$coord"
    target="$out/lib/$artifact-$version.jar"
    [ -s "$target" ] && continue
    url="https://repo1.maven.org/maven2/${group//.//}/$artifact/$version/$artifact-$version.jar"
    expected="$(curl -fsSL --retry 3 "$url.sha1" | cut -c1-40)" || { echo "REFUSED $coord: no published SHA-1" >&2; continue; }
    curl -fsSL --retry 3 "$url" -o "$target.part"
    if [ "$(sha1_of "$target.part" | cut -c1-40)" = "$expected" ]; then
        mv "$target.part" "$target"
        echo "fetched $coord"
    else
        echo "REFUSED $coord: SHA-1 mismatch" >&2
        mv "$target.part" "$target.rejected"
    fi
done
