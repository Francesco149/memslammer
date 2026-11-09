#!/bin/sh
# Tag a release and push the tag. .github/workflows/release.yml then builds
# every frontend for Windows and Linux and publishes a GitHub release.
#
#   scripts/release.sh 0.2.0      (or: make release V=0.2.0)
set -eu
v=${1#v}
echo "$v" | grep -Eq '^[0-9]+\.[0-9]+\.[0-9]+(-[0-9A-Za-z.]+)?$' ||
    { echo "version must look like 1.2.3 or 1.2.3-rc1, got '$v'" >&2; exit 1; }
[ -z "$(git status --porcelain --untracked-files=no)" ] ||
    { echo "commit or stash your changes first" >&2; exit 1; }
git rev-parse -q --verify "refs/tags/v$v" >/dev/null &&
    { echo "tag v$v already exists" >&2; exit 1; }
remote=${REMOTE:-origin}
git tag -a "v$v" -m "memslammer $v"
git push "$remote" HEAD "v$v"
echo "pushed v$v; follow the build with: gh run watch"
