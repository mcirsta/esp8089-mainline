# ESP8089 firmware for the R35 driver

The R35 driver needs the two files in `espressif/`:

```text
/lib/firmware/espressif/esp8089-cal-v19.bin
/lib/firmware/espressif/esp8089-txr8-v1.bin
```

`esp8089-cal-v19.bin` is the unchanged Espressif v19 calibration image.
`esp8089-txr8-v1.bin` is the tested TXR8-BAW1 runtime. It adds the TXR8 compact
completion and PWR1 interfaces used by the driver and enforces the negotiated
TX block-ack window, up to the existing eight-frame firmware ceiling.

## License and provenance

The firmware is distributed under the Apache License 2.0 in [`LICENSE`](LICENSE).
Espressif authorized redistribution of the ESP8089 firmware under Apache-2.0;
the permission and license addition are recorded in
[`al177/esp8089` commit `0fd883f`](https://github.com/al177/esp8089/commit/0fd883fe43d969d1fcf159d2e4a4a8e7c04200b0).
The inherited and modification notices are in [`NOTICE`](NOTICE) and
[`NOTICE-BAW1`](NOTICE-BAW1).

The vendor firmware source has not been recovered. The `source/` directory is
the preferred form for the extensions written for TXR8-BAW1. The build applies
them as a deterministic overlay to the fixed Apache-licensed input in `vendor/`.
It does not contain an Espressif SDK, chip ROM, Xiaomi firmware, device
calibration dump, credentials, or captures.

The Linux driver is a separate GPL-2.0 work. Its license is at the repository
root.

## Rebuilding

The build requires a GNU Xtensa LX106 toolchain and a host C compiler. Set
`CROSS_COMPILE` when the cross tools are outside `PATH`, then run:

```sh
CROSS_COMPILE=/path/to/xtensa-lx106-elf- ./firmware/build.sh
```

The script runs the native behavior tests, builds the overlay, validates image
layout and checksums, and requires byte-for-byte equality with the published
runtime and manifest. [`SHA256SUMS`](SHA256SUMS) identifies all three binary
artifacts. [`PROTOCOL.txt`](PROTOCOL.txt) describes the TXR8 completion ABI;
the BA-window delta is recorded in [`firmware-window.patch`](firmware-window.patch).

These are experimental development firmware files tested on the Mijia
KQJCY02QP. They are not an upstream `linux-firmware` release.
