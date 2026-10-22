#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
source_dir="$root/source"
cross_compile=${CROSS_COMPILE:-xtensa-lx106-elf-}
host_cc=${CC:-cc}
tmp=$(mktemp -d "${TMPDIR:-/tmp}/esp8089-firmware.XXXXXX")
trap 'rm -rf "$tmp"' EXIT HUP INT TERM

"$host_cc" -O2 -Wall -Wextra -Werror \
	-Wno-int-to-pointer-cast -Wno-pointer-to-int-cast \
	"$source_dir/attempts-host-test.c" -o "$tmp/attempts-host-test"
"$tmp/attempts-host-test"

"$host_cc" -O2 -Wall -Wextra -Werror \
	"$source_dir/ba-window-host-test.c" -o "$tmp/ba-window-host-test"
"$tmp/ba-window-host-test"

python3 "$source_dir/build-overlay.py" \
	--toolchain-prefix "$cross_compile"
python3 "$source_dir/test-overlay.py"
python3 "$source_dir/test-link-layout.py" \
	--toolchain-prefix "$cross_compile"

cmp "$source_dir/esp8089-txr8-v1.bin" \
	"$root/espressif/esp8089-txr8-v1.bin"
cmp "$source_dir/overlay-manifest.json" "$root/overlay-manifest.json"
(cd "$root" && sha256sum -c SHA256SUMS)

echo "PASS: source rebuild matches the published TXR8-BAW1 firmware"
