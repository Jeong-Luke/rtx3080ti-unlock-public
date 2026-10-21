# Pinned 610.43.03 firmware inputs — RTX 3080 Ti source port

This directory contains extraction scripts, provenance, and SHA-256 metadata;
it does not contain NVIDIA firmware binaries or vendor archives. The names
`ga102` and `ga10x` retain the upstream shared-architecture asset naming.

From the repository root, `make prepare` verifies local files in
`audit-inputs/`, generates the pinned firmware assets under `build/firmware/`,
and writes `build/offline-audit.json`. It does not download, install, or
execute firmware. The `firmware/fetch_artifacts.py` helper can populate the
local cache from the HTTPS URLs pinned in
[`artifacts-610.43.03.json`](artifacts-610.43.03.json); each downloaded and
cached file is SHA-256 checked. The NVIDIA source archive and NVIDIA binary
package remain local inputs and are not redistributed here.

`generate_v67.py` extracts `gsp_ga10x.bin` and the NVIDIA license from the
pinned package, and creates the 64,000-byte V67 payload. `extract_boot_assets.py`
builds `booter_load_ga102.bin`, `gsp_boot_ga102.bin`, and
`gsp_boot_desc_ga102.bin` from the pinned source archive. The inputs and
derived outputs are checked against the recorded hashes.

See [`NOTICE.md`](NOTICE.md) for provenance and licensing boundaries and
[`docs/BUILD.md`](../docs/BUILD.md) for reproducible build and offline-test
commands. No NVIDIA firmware or license redistribution approval is claimed.
