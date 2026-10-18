#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")"

has_changes() { [ -d "$1/patches" ] && [ -d "$1/new-files" ]; }

LOCAL=""
if [ -n "${LOCAL_CHANGES:-}" ]; then
	has_changes "$LOCAL_CHANGES" || {
		echo "error: LOCAL_CHANGES=$LOCAL_CHANGES has no patches/ and new-files/" >&2
		exit 1
	}
	LOCAL="$(cd "$LOCAL_CHANGES" && pwd)"
elif has_changes local-changes; then
	LOCAL="$(cd local-changes && pwd)"
else
	for d in ../* ../../*; do
		if has_changes "$d"; then LOCAL="$(cd "$d" && pwd)"; break; fi
	done
fi

if [ -z "$LOCAL" ]; then
	cat >&2 <<-EOF
	error: cannot find this project's patches/ and new-files/.
	Clone https://github.com/paulneja/Linux-on-esp32-S3 next to this tree, or
	point us at an existing checkout:
	    LOCAL_CHANGES=/path/to/Linux-on-esp32-S3 $0 ${1:-buildroot}
	EOF
	exit 1
fi
echo ">>> using local changes from: $LOCAL"

case "${1:-}" in
buildroot)
	[ -d build/buildroot ] || { echo "error: build/buildroot missing -- clone it first" >&2; exit 1; }
	echo ">>> buildroot: applying tracked-file changes"
	cd build/buildroot
	if git apply --check "$LOCAL/patches/03-buildroot-tracked-changes.patch" 2>/dev/null; then
		git apply "$LOCAL/patches/03-buildroot-tracked-changes.patch"
	elif git apply --check --reverse "$LOCAL/patches/03-buildroot-tracked-changes.patch" 2>/dev/null; then
		echo "    already applied -- skipping"
	else
		echo "error: 03-buildroot-tracked-changes.patch does not apply and is" >&2
		echo "       not already applied. Building on would silently produce an" >&2
		echo "       incomplete image. Details:" >&2
		git apply --check "$LOCAL/patches/03-buildroot-tracked-changes.patch" >&2 || true
		exit 1
	fi
	cd ../..
	echo ">>> buildroot: copying new files (board profile, espctl, init scripts)"
	rm -rf build/buildroot/board/espressif/esp32s3/patches
	rsync -a "$LOCAL/new-files/board/" build/buildroot/board/
	rsync -a "$LOCAL/new-files/configs/" build/buildroot/configs/
	;;
esp-hosted)
	[ -d build/esp-hosted/esp_hosted_ng ] || { echo "error: build/esp-hosted/esp_hosted_ng missing -- clone it first" >&2; exit 1; }
	echo ">>> esp-hosted firmware: applying the firmware patch + committing locally"
	echo "    (esp_driver/CMakeLists.txt runs 'git reset --hard' on every cmake invocation;"
	echo "     a local commit is the only thing that survives that -- see DEVELOPMENT.md)"
	cd build/esp-hosted/esp_hosted_ng
	adapter=esp/esp_driver/network_adapter
	need_commit=1
	if git log --oneline -1 | grep -q "local-only, never push"; then
		if [ -d "$adapter/components/st7789" ] && [ ! -d "$adapter/components/epdiy" ] &&
		   [ -f "$adapter/main/font8x16_basic.h" ] && [ -f "$adapter/main/usb_kbd.c" ]; then
			echo "    already committed locally -- skipping"
			need_commit=0
		else
			echo "    local firmware commit is stale (sources changed) -- re-applying"
			git reset --hard HEAD~1
		fi
	fi
	if [ "$need_commit" = 1 ]; then
		git apply "$LOCAL/patches/02-firmware-network-adapter.patch"
		git apply "$LOCAL/patches/05-firmware-usb-keyboard.patch"
		cp "$LOCAL/new-files/esp-hosted/network_adapter/partition_table.esp32s3.16m8r" \
			"$LOCAL/new-files/esp-hosted/network_adapter/sdkconfig.defaults.esp32s3.16m8r" \
			"$adapter/"
		mkdir -p "$adapter/components/st7789"
		cp -a "$LOCAL/paperboard/st7789/." "$adapter/components/st7789/"
		cp "$LOCAL/paperboard/linux/pb_console.c" "$LOCAL/paperboard/linux/usb_kbd.c" "$adapter/main/"
		cp "$LOCAL/paperboard/common/terminal.c" "$LOCAL/paperboard/common/font8x16_basic.h" "$adapter/main/"
		cp "$LOCAL/paperboard/common/terminal.h" "$LOCAL/paperboard/common/hid_keymap.h" "$adapter/main/include/"
		cp "$LOCAL/paperboard/common/hid_keymap.c" "$adapter/main/"
		git add "$adapter/components/st7789"
		git add "$adapter/main/" \
			"$adapter/sdkconfig.defaults.esp32s3" \
			"$adapter/partition_table.esp32s3.16m8r" \
			"$adapter/sdkconfig.defaults.esp32s3.16m8r"
		git -c user.email="local@backup" -c user.name="local-backup" \
			commit -m "network_adapter: this project's firmware (local-only, never push)"
	fi

	;;
*)
	echo "usage: $0 buildroot|esp-hosted" >&2
	exit 1
	;;
esac
