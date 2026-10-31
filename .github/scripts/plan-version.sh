#!/usr/bin/env bash
# Usage: plan-version.sh <nightly|release|ci> <BuildInfo.hpp>
# Prints the version to stamp (also to $GITHUB_OUTPUT). Releases are <minecraft>.<n>, nightlies <minecraft>.<n>-n<k>.
set -euo pipefail

mode="${1:?mode}"
info="${2:?BuildInfo.hpp}"
mc="$(sed -n 's/.*kMinecraftVersion = "\([^"]*\)".*/\1/p' "$info")"
[ -n "$mc" ] || { echo "no Minecraft version in $info" >&2; exit 1; }

latest=0
while read -r tag; do
    patch="${tag#v"$mc".}"
    if [ "$patch" != "$tag" ] && [[ "$patch" =~ ^[0-9]+$ ]] && [ "$patch" -gt "$latest" ]; then
        latest="$patch"
    fi
done < <(gh release list --exclude-pre-releases --limit 100 --json tagName --jq '.[].tagName')
base="$mc.$latest"

case "$mode" in
release)
    version="$mc.$((latest + 1))"
    ;;
nightly)
    previous="$(gh release view nightly --json name --jq .name 2>/dev/null || true)"
    count=1
    if [[ "$previous" =~ ^Nightly\ ${base//./\\.}-n([0-9]+)$ ]]; then
        count=$((BASH_REMATCH[1] + 1))
    fi
    version="$base-n$count"
    ;;
*)
    version="$base-ci${GITHUB_RUN_NUMBER:-0}"
    ;;
esac

echo "$version"
[ -z "${GITHUB_OUTPUT:-}" ] || echo "version=$version" >>"$GITHUB_OUTPUT"
