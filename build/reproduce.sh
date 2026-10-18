#!/usr/bin/env bash
set -euo pipefail
repo=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
cd "$repo"
dirty=$(git status --porcelain)
if [ -n "$dirty" ]; then
	echo 'The snapshot is taken from HEAD, so the tree must be clean.' >&2
	echo 'Commit tracked changes; move or ignore untracked files, including' >&2
	echo 'any build log written into the repository root:' >&2
	printf '%s\n' "$dirty" >&2
	exit 1
fi
test "$(id -u)" != 0 || { echo 'Run as a regular user with Docker access.' >&2; exit 1; }
mkdir -p build-output
if [ -n "${REPRODUCE_DIR:-}" ]; then
	# Continuous integration restores its caches into a fixed directory and
	# resumes the stages whose inputs did not change. Local runs keep a fresh
	# directory unless they ask for one explicitly.
	mkdir -p "$REPRODUCE_DIR"
	work=$(cd -- "$REPRODUCE_DIR" && pwd)
else
	work=$(mktemp -d "$repo/build-output/reproduce.XXXXXX")
fi
mkdir -p "$work/Linux-on-esp32-S3" "$work/logs"
git archive HEAD | tar -x --exclude=images -C "$work/Linux-on-esp32-S3"
git rev-parse HEAD > "$work/source-commit.txt"
git ls-tree -r HEAD > "$work/source-tree.txt"
image=linux-esp32s3-reproduce:local
docker build --network host --build-arg BUILDER_UID="$(id -u)" --build-arg BUILDER_GID="$(id -g)" \
    -f build/Dockerfile -t "$image" . 2>&1 | tee "$work/logs/container-image.log"
docker inspect --format '{{.Id}}' "$image" > "$work/container-image-id.txt"
printf 'Build directory: %s\n' "$work"
docker run --network host --name "esp32-reproduce-$(basename "$work")" --rm \
    --mount "type=bind,source=$work,target=/work" \
    --env JOBS="${JOBS:-8}" \
    --env GIT_AUTHOR_NAME="$(git config user.name)" \
    --env GIT_AUTHOR_EMAIL="$(git config user.email)" \
    --env GIT_COMMITTER_NAME="$(git config user.name)" \
    --env GIT_COMMITTER_EMAIL="$(git config user.email)" \
    "$image" 2>&1 | tee "$work/logs/build.log"
printf 'Complete local build: %s/artifacts\nNo board access or push was performed.\n' "$work"
