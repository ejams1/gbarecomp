# Oracle build dependency record

The Linux oracle container is an isolated reference backend. The native host
does not link against libmgba. Build context is `gbarecomp/oracle/`; generated
source and build output stay inside the image. The container exposes TCP 19843.

| Dependency | Pin / selection | License or role |
| --- | --- | --- |
| Base OS | `debian:bookworm-slim@sha256:7ff6ba31527b9e336162fa061746c366c4b2032f4d2cff2078bea43f78efe3f8`; bookworm, updates, and security package indexes from snapshot `20261024T000000Z` | Reproducible compiler environment; see `Dockerfile`. |
| Compiler/build | Debian `build-essential`, CMake, Ninja, pkg-config, Git from that snapshot | Builds only mGBA and `gbarecomp_oracle`. |
| mGBA | Commit `26b7884bc25a5933960f3cdcd98bac1ae14d42e2` (0.10.5) | MPL-2.0; independent reference process. |
| Linux system libraries | zlib and libpng development packages from the same snapshot | Static libmgba dependencies. |
| Windows compatibility patches | SHA-256 manifest `dependency-hashes.sha256`; not applied to Linux builds | Retained for the Windows/MSYS2 path. |

The successful build used GCC 12.2.0, CMake 3.25.1, Ninja 1.11.1, zlib
1.2.13, and libpng 1.6.39. mGBA emitted existing compiler warnings for a
signedness conversion in ARM decoding and possibly uninitialized `block0` in
the e-Reader cartridge parser; the oracle source compiled without warnings.
Resulting local image identity:
`sha256:9afbfb7198529ddd33cbc703b2e74b6ba28d6b72ed42e8b71e575f7764052655`
(853,746,096 bytes). These output identities describe the verified build;
rebuilds may have a different image digest while retaining the pinned inputs.

From `gbarecomp/oracle/`, build the image:

```powershell
docker build --pull -t gbarecomp-mgba-oracle:0.10.5 -f Dockerfile .
```

Publish the service only on host loopback. Docker Desktop forwards to the
container's `0.0.0.0` listener, while the oracle default remains loopback-only:

```powershell
$scratchDirectory = Join-Path $env:TEMP 'adult-oracle-scratch'
New-Item -ItemType Directory -Force -Path $scratchDirectory | Out-Null
Copy-Item ../../pokeemerald-modern-adult/pokeemerald_modern.gba "$scratchDirectory/rom.gba"
docker run --rm --publish 127.0.0.1:19843:19843 `
  --mount "type=bind,source=$scratchDirectory,target=/work" `
  gbarecomp-mgba-oracle:0.10.5 --no-bios --rom /work/rom.gba `
  --port 19843 --bind 0.0.0.0
```

Run the smoke route with the current fork ROM; the script makes and removes a
private temporary ROM copy, waits for an oracle ping, steps one frame, reads
IWRAM, checks the full RGB888 frame, then requests clean shutdown:

```powershell
cd gbarecomp/oracle
./smoke-oracle.ps1 -RomPath ../../pokeemerald-modern-adult/pokeemerald_modern.gba
```

The verified result used ROM SHA-1
`CF56B97299739FFB39230C4258D243D9AED39AD7`, one frame, a four-byte IWRAM
read, a 240x160 RGB888 frame (115,200 decoded bytes), and exit code 0. This is
an oracle process smoke check, not a gameplay-route qualification.

Put a private ROM copy in `$scratchDirectory`; the mount is per-run so any
automatically discovered sibling save stays isolated. BIOS is omitted in this
example, selecting mGBA's HLE startup path. If a BIOS is available, mount a
private copy and pass `--bios /work/bios.bin`. Do not put personal BIOS/save
files in the build context.

The image tag is a local convenience, not the source identity. Image builds
verify the exact mGBA Git commit and the checked-in patch hashes. The Debian
package indexes are pinned by snapshot date; apt verifies their signed Release
metadata. HTTP avoids an unpinned CA bootstrap in the minimal base image.
Record the base-layer and image digests with run reports (`docker image inspect`).
