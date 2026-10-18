#!/usr/bin/env bash
# Print the Image workflow's cache keys as GitHub-output name=value lines.
#
# Each value is one SHA-1 over the `git ls-tree -r HEAD` listings of every
# file its stage reads, straight from HEAD. That matches what reproduce.sh
# snapshots with `git archive HEAD`, it cannot be tripped by the dangling
# busybox symlink in the rootfs overlay (`hashFiles` in an expression throws
# on those) or by uncommitted working-tree edits, and one hash per key stays
# inside the 512-character cache-key limit however many paths a stage reads.
#
# Every output cache also depends on the build procedure and the pinned
# revisions, and each later stage's key includes the inputs of the stages
# before it, so one stage's cache can never be restored while an earlier
# stage's inputs changed. The download and tools entries hold only upstream
# archives and get a prefix fallback on top of this key.
#
# Add a path here when a stage starts reading it; a file missing from a list
# means a cache can resume a stage whose input has changed.
set -euo pipefail
cd "$(git rev-parse --show-toplevel)"

digest() {
	local path
	for path in "$@"; do
		git cat-file -e "HEAD:$path" 2>/dev/null ||
			{ echo "cache-keys: no such path in HEAD: $path" >&2; return 1; }
	done
	git ls-tree -r HEAD -- "$@" | git hash-object --stdin
}

common='build/sources.lock build/container-build.sh build/pin-toolchain.py'
rootfs_extra='new-files/board new-files/configs new-files/toplevel/apply-local-changes.sh patches/03-buildroot-tracked-changes.patch'
firmware_extra='patches/02-firmware-network-adapter.patch patches/05-firmware-usb-keyboard.patch new-files/esp-hosted paperboard/st7789 paperboard/linux/pb_console.c paperboard/linux/usb_kbd.c paperboard/common/terminal.c paperboard/common/terminal.h paperboard/common/hid_keymap.c paperboard/common/hid_keymap.h paperboard/common/font8x16_basic.h'
userspace_extra='experiments paperboard/linux/epd-shell.c'

# shellcheck disable=SC2086
{
	echo "toolchain=$(digest $common)"
	echo "buildroot_dl=$(digest build/sources.lock)"
	echo "rootfs=$(digest $common $rootfs_extra)"
	echo "firmware=$(digest $common $rootfs_extra $firmware_extra)"
	echo "idf_tools=$(digest build/sources.lock)"
	echo "userspace=$(digest $common $rootfs_extra $userspace_extra)"
}
