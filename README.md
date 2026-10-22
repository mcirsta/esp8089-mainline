# ESP8089 SDIO Linux driver

This repository contains an in-tree Linux `mac80211` driver candidate for the
Espressif ESP8089 SDIO Wi-Fi chip (`6666:1111`). It is the R35 source tested on
the Xiaomi/Mijia KQJCY02QP air monitor with Linux 7.2.11.

The driver is derived from Espressif's GPL driver and the cleanup work in
[`Icenowy/esp8089`](https://github.com/Icenowy/esp8089), based on commit
`71c53ead1ce24fb5cee1f9c2eb251be1f3125e37`. The original vendor-derived
source was also compared with
[`jwrdegoede/esp8089`](https://github.com/jwrdegoede/esp8089) commit
`38cb0c10d70754392932a52d9335a29eed9c3b94`.

## Status

This is working development code, not an accepted upstream Linux driver. The
tested R35 candidate supports station mode, WPA2/CCMP, SDIO transport,
aggregation, recovery, firmware TX reports, transmit-power control and KUnit
coverage for protocol, ownership, lifecycle, RX, key and BSSID behavior.

On the tested board it passed:

- Linux 7.2.11 normal and four-core KASAN/lockdep builds;
- 41 KUnit cases;
- WPA2 association, DHCP and bounded bidirectional traffic;
- repeated unload/reload and reassociation testing from RAM;
- W=1, Sparse and focused checkpatch checks.

The driver is **not yet mainline-ready**. Remaining work includes firmware ABI
and redistribution provenance, normal kernel-tree packaging and maintainership,
broader RF/cipher interoperability evidence, and resolution of parked broadcast
and in-place rekey investigations. A one-off SMP Qt startup KASAN report occurred
in the core futex/hrtimer path before association; it has not been attributed to
this driver and has not been reproduced.

## Firmware

The source expects these files through the kernel firmware loader:

```text
espressif/esp8089-cal-v19.bin
espressif/esp8089-txr8-v1.bin
```

They implement the driver-specific TXR8 and PWR1 interfaces. They are not
included here because their redistribution terms and provenance have not been
resolved. An incompatible firmware image is rejected during probe.

## Kernel integration

The repository contains the driver directory as it is intended to appear under:

```text
drivers/net/wireless/espressif/esp8089/
```

Add the directory to the parent Espressif Kconfig and Makefile, then enable:

```text
CONFIG_ESP8089=m
```

For the KUnit suite also enable:

```text
CONFIG_KUNIT=y
CONFIG_ESP8089_KUNIT_TEST=m
```

This source uses current in-tree `mac80211`, KUnit visibility and firmware APIs.
It is not packaged as a DKMS or arbitrary-kernel-headers module.

## Scope of the R35 candidate

R35 preserves the authentication target BSSID supplied by mac80211 before
association. Explicit `BSS_CHANGED_BSSID` notifications replace or clear the
target, while association-only updates change AID and association state without
erasing it. The accompanying KUnit cases inspect the emitted firmware BSS and
default-key commands.

Earlier hardening in this source includes bounded firmware parsing, explicit
SDIO logical and rounded lengths, zeroed bus padding, RX metadata validation,
transactional key handling, lifecycle and ownership synchronization, transport
failure recovery, aggregation limits and compact TX completion validation.

## Licensing

The kernel module is GPL code. Individual files retain their SPDX identifiers;
the repository includes the GNU GPL version 2 text in [`LICENSE`](LICENSE).
The firmware files described above are separate works and are not covered or
distributed by this repository.

