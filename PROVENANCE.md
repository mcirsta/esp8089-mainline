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

The repository intentionally excludes compiled objects, kernel images,
initramfs images, firmware binaries, captured traffic, logs, Wi-Fi profiles and
device-specific credentials.

## Evidence limits

The source was verified against its retained local R35 manifest before export.
The detailed hardware logs and private test environment are not part of this
public repository. Passing local tests does not imply upstream acceptance or
compatibility with arbitrary ESP8089 firmware and boards.

