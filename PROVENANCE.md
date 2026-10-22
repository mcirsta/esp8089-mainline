# Source provenance

The public source snapshot corresponds to the locally tested **R35** candidate
from 22 October 2026.

## Lineage

- Original code copyright: Espressif Systems, 2009-2014, as retained in source.
- Cleanup baseline: `Icenowy/esp8089`, branch `cleanup`, commit
  `71c53ead1ce24fb5cee1f9c2eb251be1f3125e37`.
- Vendor-derived comparison source: `jwrdegoede/esp8089`, commit
  `38cb0c10d70754392932a52d9335a29eed9c3b94`.
- Current snapshot: local R35 candidate tested in RAM on Linux 7.2.11.

The repository intentionally excludes compiled kernel objects, kernel images,
initramfs images, captured traffic, logs, Wi-Fi profiles and device-specific
credentials. It includes only the three ESP8089 firmware binaries documented
under [`firmware/`](firmware/): the exact R35 runtime and calibration files plus
the fixed Apache-licensed vendor input required for a reproducible overlay
build.

## Firmware lineage

- Licensed input: Espressif `eagle_fw_first_init_v19.bin`, SHA-256
  `1b8b7f93f40d3c2dd7ab7f3e2d29cd118604249a5926a320163bf358254e4162`.
- Unmodified calibration image: `esp8089-cal-v19.bin`, SHA-256
  `8f2f3f1fe623e1fe0d4aa1376b4bb6a129479cea9bce6374855b4cec19ac1687`.
- Modified runtime: TXR8-BAW1 published as `esp8089-txr8-v1.bin`, SHA-256
  `15b9b23c3e6dad7e071618e2bb09177f4e0ec3fe458f44ba1f0e81222694cb88`.
- Espressif redistribution grant and Apache-2.0 license addition:
  [`al177/esp8089` commit `0fd883f`](https://github.com/al177/esp8089/commit/0fd883fe43d969d1fcf159d2e4a4a8e7c04200b0).

The original vendor source and chip ROM source are unavailable. All new
overlay code and its deterministic builder are included. The build manifest
records source hashes, placement, installation plan and protocol properties.

## Evidence limits

The source was verified against its retained local R35 manifest before export.
The detailed hardware logs and private test environment are not part of this
public repository. Passing local tests does not imply upstream acceptance or
compatibility with arbitrary ESP8089 firmware and boards.

