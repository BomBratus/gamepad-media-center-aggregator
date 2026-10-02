#!/usr/bin/env bash
set -euo pipefail
: "${BASE_URL:?Set BASE_URL to the pinned PS4 package release}"
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cache="$root/.ps4-cache/packages"
mkdir -p "$cache"
packages=()
while IFS= read -r package; do
    [[ -n "$package" && "$package" != \#* ]] || continue
    archive="$cache/ps4-openorbis-${package}-any.pkg.tar.gz"
    if [[ ! -s "$archive" ]]; then
        curl --fail --location --retry 3 "$BASE_URL/$(basename "$archive")" --output "$archive.tmp"
        mv "$archive.tmp" "$archive"
    fi
    packages+=("$archive")
done < "$root/scripts/ps4/dependencies.txt"
pacman --noconfirm -U "${packages[@]}"
git config --system --add safe.directory "${GITHUB_WORKSPACE:-$root}"
