#!/bin/sh
# Prints cmspeed's version for the current checkout. Releases are git tags named vX.Y.Z.
#
# - On a commit tagged vX.Y.Z: X.Y.Z
# - Otherwise, a prerelease of the next patch version: X.Y.(Z+1)-dev.N+gSHA, where N counts the commits since
#   vX.Y.Z. With no release tag yet the base is 0.0.0, so the first builds are 0.0.1-dev.N.
set -e
cd "$(dirname "$0")/.."
sha=$(git rev-parse --short HEAD)
if tag=$(git describe --tags --abbrev=0 --match 'v[0-9]*.[0-9]*.[0-9]*' 2>/dev/null); then
    base=${tag#v}
    n=$(git rev-list --count "$tag..HEAD")
else
    base=0.0.0
    n=$(git rev-list --count HEAD)
fi
if [ "$n" -eq 0 ]; then
    echo "$base"
    exit
fi
major=${base%%.*}
rest=${base#*.}
minor=${rest%%.*}
patch=${rest#*.}
echo "$major.$minor.$((patch + 1))-dev.$n+g$sha"
