#!/usr/bin/env bash
# Usage: publish-release.sh <nightly|release> <version> <sha> <artifact dir>
# Publishes the downloaded binaries as they are: a single moving prerelease "nightly", or a tagged release.
set -euo pipefail

mode="${1:?mode}"
version="${2:?version}"
sha="${3:?sha}"
artifacts="${4:?artifact dir}"
out="$(mktemp -d)"

cp "$artifacts"/*/* "$out"/
(cd "$out" && sha256sum -- * >SHA256SUMS)

if [ "$mode" = release ]; then
    gh release create "v$version" "$out"/* --target "$sha" --title "$version" --generate-notes --latest
    exit 0
fi

notes="Automatic build of $sha. It is replaced by the next push to main."
if gh release view nightly >/dev/null 2>&1; then
    old="$(gh api repos/"$GITHUB_REPOSITORY"/git/ref/tags/nightly --jq .object.sha)"
    status="$(gh api repos/"$GITHUB_REPOSITORY"/compare/"$old"..."$sha" --jq .status)"
    if [ "$status" = behind ]; then
        echo "a newer nightly is already published"
        exit 0
    fi
    gh api -X PATCH repos/"$GITHUB_REPOSITORY"/git/refs/tags/nightly -f sha="$sha" -F force=true >/dev/null
    for asset in $(gh release view nightly --json assets --jq '.assets[].name'); do
        gh release delete-asset nightly "$asset" --yes
    done
    gh release edit nightly --title "Nightly $version" --notes "$notes" --prerelease --latest=false
    gh release upload nightly "$out"/* --clobber
else
    gh release create nightly "$out"/* --target "$sha" --prerelease --latest=false --title "Nightly $version" --notes "$notes"
fi
