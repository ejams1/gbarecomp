# Fork symbol import evidence

This records the M0-4a import from the modern fork's pinned build. The generated
files are local build artifacts under `games/emerald_adult/symbols/` and are
ignored by Git. Never import the raw debugger `.sym` file; the importer consumes
ELF `readelf` text, the map, and the byte-matching ROM.

## Repeat the import

From the workspace root, ensure the game outputs were produced together by
`make modern` in `adult-emerald-build:r63-libpng`, then run:

```powershell
./gbarecomp/games/emerald_adult/tools/import-symbols.ps1
```

The script checks the pinned ROM SHA-1 and ELF/map SHA-256 before doing any
import, captures `readelf -sW` and `readelf -SW` inside the container, invokes the importer with
`--data-source auto`, checks its six expected files, and writes
`symbols/import-manifest.json` with ROM/ELF/map/readelf/imported-output hashes,
the importer hash and framework commit, and the local Docker image ID. Set
`-GameRepo`, `-DockerImage`, `-ExpectedRomSha1`, `-ExpectedElfSha256`, or
`-ExpectedMapSha256` only as a reviewed update to the recorded build inputs;
any identity mismatch fails closed by default.

Equivalent manual commands (run from the workspace root):

```powershell
docker run --rm -v "${PWD}:/workspace" -w /workspace/pokeemerald-modern-adult `
  adult-emerald-build:r63-libpng sh -c `
  'set -eu; arm-none-eabi-readelf -sW pokeemerald_modern.elf > ../gbarecomp/games/emerald_adult/symbols/readelf-symbols.txt; arm-none-eabi-readelf -SW pokeemerald_modern.elf > ../gbarecomp/games/emerald_adult/symbols/readelf-sections.txt'
python ./gbarecomp/tools/symbol_import/import_decomp_symbols.py `
  --id POKEMON_MODERN_EMERALD --name "Pokemon Modern Emerald (USA)" `
  --syms ./gbarecomp/games/emerald_adult/symbols/readelf-symbols.txt `
  --sections ./gbarecomp/games/emerald_adult/symbols/readelf-sections.txt `
  --map ./pokeemerald-modern-adult/pokeemerald_modern.map `
  --rom ./pokeemerald-modern-adult/pokeemerald_modern.gba `
  --out ./gbarecomp/games/emerald_adult/symbols --data-source auto
```

## Imported build identity

| Input | SHA-1 / SHA-256 | Evidence |
|---|---|---|
| ROM | SHA-1 `57d3561262bf5337293a2d31f6b79b5879f7cff7`; SHA-256 `ab1caeec3781be5cf68128157aa8f70a879b85566f5dfe7462206efa5cf42a8` | Matches the M0-13 qualified fixture ROM identity. |
| ELF | SHA-256 `37305ee0df463968c99057aba54742265484ec449a6c26ead80f90d52f6632d1` | `pokeemerald_modern.elf`, built with the same `make modern` invocation as the ROM/map. |
| Link map | SHA-256 `79f0799dc6aa1bdc9920efeb2ec44d3c846f31c54155f36de245ede3da40e4cb` | `pokeemerald_modern.map` from that invocation. |
| ELF symbol text | SHA-256 `7ee3bcf4a56f8061342bca3069fe214cb832e12adb1d1962d8ab6619bdd6018a` | Container-side `arm-none-eabi-readelf -sW`. |
| ELF section text | SHA-256 `54b2dfb38add73da44a974737d349927ca0708b28fe127dcf75e89f101d20c49` | Container-side `arm-none-eabi-readelf -SW`. |

The pinned reader is GNU readelf from binutils 2.41 in the game build image;
the game build notes pin devkitARM r63 / GCC 13.2.0. The map reports the same
ROM-linked section sizes in the fresh build. The ROM is 32 MiB.

## Classification and importer result

The ELF section table gives exact flags and addresses: `.text` and `lib_text`
are executable `PROGBITS`; `script_data`, `.rodata`, and `song_data` are
non-executable ROM `PROGBITS`; `ewram` and `iwram` are RAM `NOBITS`. The
importer found three non-executable ROM sections, used `sections` through the
explicit `auto` policy, and emitted two coalesced data ranges spanning
`0x13FDB20` bytes:

| Start | End (exclusive) | Length | Source sections |
|---|---|---|---|
| `0x0821ED18` | `0x08334434` | `0x11571C` | `script_data` |
| `0x08336728` | `0x0961EB2C` | `0x12E8404` | `.rodata`, `song_data` |

The first range stops before executable `lib_text` at `0x08334438`; the
four-byte linker alignment gap stays unclassified. The 1,404 map input sections were examined as a cross-check;
the map was not selected because the section table already provided a direct
code/data split. Mapping symbols report 16,022 fine-grained data regions,
including in-function literal pools. No address classification was inferred
from symbol names.

M0-4 tested the mapping-symbol overlay against the complete ROM. The generator
rejected it with 114 control-flow targets inside `$d` ranges, including
`0x08011D60` reached from `MgbaAssert`. These conflicts need instruction-level
review before changing the classification; M0-4 therefore uses the two
section-derived ranges above and records the mapping result as an unresolved
coverage limit.

The import produced 17,097 function seeds: 52 ARM and 17,045 Thumb. All names
were meaningful; zero function seeds overlapped the selected data ranges, so
the importer dropped zero functions. It produced 33,228 data-symbol rows.
`imported_symbols.tsv` stores Thumb addresses with bit 0 cleared and records
`thumb` mode separately, as required by the recompiler.

Representative symbol-to-seed checks:

| ELF symbol | ELF value/type | Imported seed | Disassembly check |
|---|---|---|---|
| `AgbMain` | `0x080003AD FUNC` | `0x080003AC thumb` | `objdump -d` shows Thumb startup at the even code address; ELF bit 0 identifies Thumb state. |
| `VBlankIntr` | `0x08000795 FUNC` | `0x08000794 thumb` | The odd ELF value is normalized to an even code address with Thumb mode preserved. |
| `HBlankIntr` | `0x08000865 FUNC` | `0x08000864 thumb` | Same ELF/seed mode normalization. |
| `SerialIntr` | `0x080008C9 FUNC` | `0x080008C8 thumb` | Same ELF/seed mode normalization. |
| `IntrMain` | `0x08000250 NOTYPE` | explicit ARM root in `emerald.toml` | The ARM disassembly at `0x08000250` reads pending IRQ bits and branches indirectly through `gIntrTable` (`0x030019B4`); `IntrMain_Buffer` is separately at `0x030019F0`. It is an interrupt dispatch root but not `STT_FUNC`, so M0-4 added it as a reviewed explicit seed. |

The checked map and symbol table contain no `STT_FUNC` entries in EWRAM or
IWRAM. Their RAM sections are `NOBITS`; this build provides no evidenced
source/destination code-copy pair for the importer's `--code-copy-pair` option.
Accordingly, the importer emitted no code-copy annotation rather than guessing
that any RAM-resident data is executable. M0-4's generation found no evidence
that warrants changing that classification.

## Generated files

`symbols/` contains `readelf-symbols.txt`, `readelf-sections.txt`,
`imported_symbols.tsv`, `imported_data_symbols.tsv`, `function_boundaries.tsv`,
`POKEMON_MODERN_EMERALD_symbols.toml`, and `import-manifest.json`. The manifest
records SHA-256 for the three generated import products and overlay:

| Output | SHA-256 |
|---|---|
| `imported_symbols.tsv` | `6b8afca34adf6bca86e4cfa117f20541eed9b0452250810e190724b90ae8fb3a` |
| `imported_data_symbols.tsv` | `ae81f4dac3e6b5698a76f80fabed6e350b8aa87567ab13b716c264d3d37c7579` |
| `function_boundaries.tsv` | `041463d7a5f6d9ea4e5c1d7bcefbfe23ff4ef9332a4de75202e0966041344972` |
| `POKEMON_MODERN_EMERALD_symbols.toml` | `f77ee42ccbcad74c773c0c89c617cf962b3436023217a2dfd845be172cd2e1ce` |

These artifacts are ignored and can be regenerated with the helper above. The
TOML overlay carries the same ROM SHA-1 identity and two measured data ranges;
it does not replace the hand-maintained `emerald.toml` planned in M0-4.
