# Building and installing ESP8089

This guide builds the driver into a Linux source tree and stages the modules
and required firmware for the target device. The tested combination is Linux
**7.2.11**, R35 driver source and the included **TXR8-BAW1** firmware, on the
Mijia KQJCY02QP. Other kernel versions may require API changes.

The repository is a driver directory for in-tree Kbuild. Running `make` in this
repository or installing a DKMS package is not the supported build procedure.
The board must already have working SDIO host support, pinmux, clocks, power and
reset sequencing so the ESP8089 enumerates as SDIO device `6666:1111`. This
repository does not supply the board DTS, bootloader, kernel configuration or
complete boot image.

## 1. Prepare the source and tools

Install the dependencies required by your kernel build: Git, GNU make, a C
compiler/binutils for the target, a host C compiler, bc, flex, bison, pkg-config,
and the development headers for OpenSSL and libelf. Menuconfig also needs the
ncurses development headers. The exact package names depend on the build host.

Clone this repository and choose **absolute paths** for a compatible Linux
source tree, a separate kernel output directory and a target root filesystem:

```sh
git clone https://github.com/mcirsta/esp8089-mainline.git
export ESP8089_REPO="$(pwd)/esp8089-mainline"
export KSRC=/path/to/linux-7.2.11
export KBUILD_DIR=/path/to/linux-build
export TARGET_ROOT=/path/to/target-rootfs
```

For the 32-bit ARM Mijia, select an ARM kernel toolchain, for example:

```sh
export ARCH=arm
export CROSS_COMPILE=arm-linux-gnueabihf-
```

For a native build, leave `ARCH` and `CROSS_COMPILE` unset. Use the same
architecture, compiler settings, kernel configuration and release string for
the kernel and its modules. The Xtensa toolchain mentioned in the firmware
documentation is only for rebuilding firmware; it does not build this ARM
Linux driver. The supplied firmware binaries need no firmware toolchain.

## 2. Integrate with Kconfig and Kbuild

Copy only the driver sources, tests and build files into the kernel tree:

```sh
mkdir -p "$KSRC/drivers/net/wireless/espressif/esp8089"
cp "$ESP8089_REPO"/*.c "$ESP8089_REPO"/*.h \
   "$ESP8089_REPO/Kconfig" "$ESP8089_REPO/Makefile" \
   "$KSRC/drivers/net/wireless/espressif/esp8089/"
```

If the kernel does not already have an Espressif vendor directory, create
`drivers/net/wireless/espressif/Kconfig` with:

```kconfig
# SPDX-License-Identifier: GPL-2.0-only
config WLAN_VENDOR_ESPRESSIF
	bool "Espressif devices"
	default y

if WLAN_VENDOR_ESPRESSIF
source "drivers/net/wireless/espressif/esp8089/Kconfig"
endif
```

Create `drivers/net/wireless/espressif/Makefile` with:

```make
# SPDX-License-Identifier: GPL-2.0-only
obj-$(CONFIG_ESP8089) += esp8089/
```

Then add the following line inside the existing `if WLAN` block in
`drivers/net/wireless/Kconfig`, alongside the other vendor entries:

```kconfig
source "drivers/net/wireless/espressif/Kconfig"
```

Add this line to `drivers/net/wireless/Makefile`:

```make
obj-$(CONFIG_WLAN_VENDOR_ESPRESSIF) += espressif/
```

If an Espressif vendor directory already exists, add the driver `source` and
`obj-$(CONFIG_ESP8089)` lines to its existing files and retain its other
entries. Add each parent entry only once.

## 3. Configure and build the matching kernel

Start with a known working configuration for the target board. For example,
when you already have that configuration saved:

```sh
mkdir -p "$KBUILD_DIR"
cp /path/to/working-board.config "$KBUILD_DIR/.config"
```

Enable networking, the SDIO/Wi-Fi stack and the driver as a module:

```sh
"$KSRC/scripts/config" --file "$KBUILD_DIR/.config" \
    --enable MODULES --enable NET --enable NETDEVICES --enable WLAN \
    --enable WLAN_VENDOR_ESPRESSIF --enable MMC \
    --enable RFKILL --enable CFG80211 --enable MAC80211 --enable FW_LOADER \
    --module ESP8089 --disable ESP8089_KUNIT_TEST
make -C "$KSRC" O="$KBUILD_DIR" olddefconfig
```

Keep the board's MMC/SDIO host controller driver enabled; on the Mijia this is
`CONFIG_MMC_SUNXI`. Check the resulting `.config`, since Kconfig can turn off
options whose dependencies are missing. The key settings are:

```text
CONFIG_MODULES=y
CONFIG_NET=y
CONFIG_NETDEVICES=y
CONFIG_WLAN=y
CONFIG_WLAN_VENDOR_ESPRESSIF=y
CONFIG_MMC=y
CONFIG_RFKILL=y
CONFIG_CFG80211=y
CONFIG_MAC80211=y
CONFIG_FW_LOADER=y
CONFIG_ESP8089=m
```

Build the kernel and its modules:

```sh
make -C "$KSRC" O="$KBUILD_DIR" -j"$(nproc)"
```

The driver module is produced at:

```text
<kernel-output>/drivers/net/wireless/espressif/esp8089/esp8089.ko
```

A full build supplies the generated headers and symbol versions needed for
matching modules. `modules_prepare` alone does not generate `Module.symvers`.
After changing driver sources, rerun the same build command; make rebuilds
the changed objects and their dependents. Add `W=1` for extra compiler warnings.
For an ARM device, also build its DTB with `make -C "$KSRC" O="$KBUILD_DIR"
dtbs` if it is not already supplied by your board build.

Do not force-load a module into a kernel with a different release or symbol
ABI. This source does not provide compatibility shims for older vendor kernels.

## 4. Stage modules and both firmware files

The following commands populate `TARGET_ROOT` on the build host. Set it to the
target image's root directory, not the build host's `/`. Use a writable staging
directory; no administrator access is needed for an ordinary user-owned one.

```sh
KREL=$(make -s -C "$KSRC" O="$KBUILD_DIR" kernelrelease)
make -C "$KSRC" O="$KBUILD_DIR" \
    INSTALL_MOD_PATH="$TARGET_ROOT" DEPMOD=true modules_install
depmod -b "$TARGET_ROOT" "$KREL"

(cd "$ESP8089_REPO/firmware" && sha256sum -c SHA256SUMS)
install -d "$TARGET_ROOT/lib/firmware/espressif"
install -m 0644 \
    "$ESP8089_REPO/firmware/espressif/esp8089-cal-v19.bin" \
    "$ESP8089_REPO/firmware/espressif/esp8089-txr8-v1.bin" \
    "$TARGET_ROOT/lib/firmware/espressif/"

install -d "$TARGET_ROOT/usr/share/doc/esp8089-firmware"
install -m 0644 "$ESP8089_REPO/firmware/LICENSE" \
    "$ESP8089_REPO/firmware/NOTICE" \
    "$ESP8089_REPO/firmware/NOTICE-BAW1" \
    "$TARGET_ROOT/usr/share/doc/esp8089-firmware/"
```

`depmod` is supplied by the build host's kmod package and can generate the
target's module dependency files without executing target code. Staging all
built modules also supplies any modular SDIO, crypto or wireless dependencies.
For a local installation on the target instead, use its normal privileged
module/firmware installation procedure and run `depmod -a` for that kernel.

The kernel requests these exact paths relative to `/lib/firmware`:

| File | Purpose | SHA-256 |
| --- | --- | --- |
| `espressif/esp8089-cal-v19.bin` | Unchanged v19 calibration stage | `8f2f3f1fe623e1fe0d4aa1376b4bb6a129479cea9bce6374855b4cec19ac1687` |
| `espressif/esp8089-txr8-v1.bin` | TXR8-BAW1 runtime | `15b9b23c3e6dad7e071618e2bb09177f4e0ec3fe458f44ba1f0e81222694cb88` |

The file under `firmware/vendor/` is a build input, not an additional runtime
file. Keep the published filenames: stock ESP8089 firmware does not implement
the TXR8/PWR1 ABI required by this driver.

The target also needs `iw`, module-loading tools such as kmod, an IP
configuration tool and a Wi-Fi connection manager such as wpa_supplicant.
Include your target distribution's `wireless-regdb` package (`regulatory.db`
and its signature when required by cfg80211) for regulatory channel/power data.
That database is separate from the two device firmware files in this repository.

## 5. Boot, load and verify on the target

Boot the kernel you just built, with its matching board DTB and staged root
filesystem, using the board's normal boot procedure. Modules belong under
`/lib/modules/<that-kernel-release>/`. If the driver loads in an initramfs,
include the modules, their dependencies and both firmware files in that
initramfs before boot. A built-in `CONFIG_ESP8089=y` driver also needs firmware
available when it probes; module mode is the simpler starting point.

For the Mijia RAM-only workflow, include the staged files in the RAM initramfs
and load the matching kernel/DTB/initramfs through the tested FEL loader. This
guide does not require writing NAND. The Wi-Fi firmware is downloaded to the
ESP8089's RAM at probe time.

On the running target:

```sh
uname -r
modinfo esp8089
modinfo -F firmware esp8089
modprobe esp8089
dmesg | grep -E 'esp8089|TXR8|PWR1|firmware'
iw dev
ip link
```

Run `modprobe` as root. `modinfo -F firmware` should list both paths above.
Successful startup logs include `TXR8 firmware negotiated`, `PWR1 firmware`
and `ESP8089 initialized through two firmware stages`. `iw dev` should show a
managed interface; its name depends on the target's naming policy. Use the
target's network manager or wpa_supplicant plus a DHCP client to connect. The
driver does not store Wi-Fi credentials or configure IP addresses.

## Troubleshooting

| Symptom | Check |
| --- | --- |
| `CONFIG_ESP8089` disappears after `olddefconfig` | Check all parent Kconfig/Makefile entries and MMC, MAC80211 and WLAN dependencies. |
| No SDIO device / no probe log | Inspect `/sys/bus/sdio/devices/` and its `uevent` files for `SDIO_ID=6666:1111`; check board DTS, host controller, power/reset and pinmux. |
| Firmware load fails with `-2` (`ENOENT`) | Check both filenames in the target rootfs or early initramfs. Version-specific firmware directories and `firmware_class.path` can override `/lib/firmware`. |
| TXR8/PWR1 negotiation or image parsing fails | Reinstall the exact included pair and check their SHA-256 values; renaming stock firmware is insufficient. |
| `Invalid module format` / unknown symbols | Match the running kernel, config, release string and `Module.symvers`; install dependencies and rerun `depmod`. |
| Module signature rejected | Use the target kernel's normal trusted module-signing process. |
| Interface exists but has no IP address | Check userspace association and DHCP; these are separate from module/firmware loading. |

The [kernel Kbuild documentation](https://docs.kernel.org/kbuild/modules.html)
explains module build prerequisites and symbol versions; the
[firmware search-path documentation](https://docs.kernel.org/driver-api/firmware/fw_search_path.html)
describes path precedence.

## Optional developer tests and firmware rebuild

For the driver KUnit suite, enable `CONFIG_KUNIT=y` and
`CONFIG_ESP8089_KUNIT_TEST=m` alongside `CONFIG_ESP8089=m`, rerun `olddefconfig`,
rebuild, and install the matching kernel/modules as above. On that target,
`modprobe esp8089-test` runs the seven `esp8089-*` suites and prints KTAP
results to the kernel log. These tests need no SDIO hardware or firmware;
they do not replace runtime Wi-Fi tests.

Installing the supplied firmware needs no firmware rebuild. To reproduce it,
see the [firmware build instructions](firmware/README.md#rebuilding). The
firmware build uses a separate Xtensa LX106 toolchain and verifies exact output
bytes against the published binaries.
