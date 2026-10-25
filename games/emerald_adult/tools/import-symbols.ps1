[CmdletBinding()]
param(
    [string]$GameRepo = (Join-Path $PSScriptRoot "../../../../pokeemerald-modern-adult"),
    [string]$DockerImage = "adult-emerald-build:r63-libpng",
    [string]$ExpectedRomSha1 = "57d3561262bf5337293a2d31f6b79b5879f7cff7",
    [string]$ExpectedElfSha256 = "37305ee0df463968c99057aba54742265484ec449a6c26ead80f90d52f6632d1",
    [string]$ExpectedMapSha256 = "79f0799dc6aa1bdc9920efeb2ec44d3c846f31c54155f36de245ede3da40e4cb"
)

$ErrorActionPreference = "Stop"
$hostRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$frameworkRoot = (Resolve-Path (Join-Path $hostRoot "../..")).Path
$gameRoot = (Resolve-Path $GameRepo).Path
$symbolsDir = Join-Path $hostRoot "symbols"
New-Item -ItemType Directory -Force -Path $symbolsDir | Out-Null

$rom = Join-Path $gameRoot "pokeemerald_modern.gba"
$elf = Join-Path $gameRoot "pokeemerald_modern.elf"
$map = Join-Path $gameRoot "pokeemerald_modern.map"
foreach ($path in @($rom, $elf, $map)) {
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        throw "Required matching build artifact is missing: $path. Build with make modern in the pinned game Docker image."
    }
}

$romSha1 = (Get-FileHash -LiteralPath $rom -Algorithm SHA1).Hash.ToLowerInvariant()
if ($romSha1 -ne $ExpectedRomSha1.ToLowerInvariant()) {
    throw "ROM identity drift: expected SHA-1 $ExpectedRomSha1, found $romSha1. Review and explicitly update the identity before importing."
}
$elfSha256 = (Get-FileHash -LiteralPath $elf -Algorithm SHA256).Hash.ToLowerInvariant()
if ($elfSha256 -ne $ExpectedElfSha256.ToLowerInvariant()) {
    throw "ELF identity drift: expected SHA-256 $ExpectedElfSha256, found $elfSha256. Rebuild and explicitly review the matching outputs before importing."
}
$mapSha256 = (Get-FileHash -LiteralPath $map -Algorithm SHA256).Hash.ToLowerInvariant()
if ($mapSha256 -ne $ExpectedMapSha256.ToLowerInvariant()) {
    throw "Link-map identity drift: expected SHA-256 $ExpectedMapSha256, found $mapSha256. Rebuild and explicitly review the matching outputs before importing."
}

$imageId = (& docker image inspect --format '{{.Id}}' $DockerImage 2>$null)
if ($LASTEXITCODE -ne 0 -or -not $imageId) {
    throw "Docker image '$DockerImage' is not available. Build it using the game's docs/build-notes.md instructions."
}

# Keep readelf output capture inside the pinned Linux container. PowerShell
# redirection can change the output encoding and obscure the native exit code.
$capture = @'
set -eu
arm-none-eabi-readelf -sW pokeemerald_modern.elf > /host/symbols/readelf-symbols.txt
arm-none-eabi-readelf -SW pokeemerald_modern.elf > /host/symbols/readelf-sections.txt
'@
& docker run --rm -v "${gameRoot}:/game" -v "${hostRoot}:/host" `
    -w /game $DockerImage sh -c $capture
if ($LASTEXITCODE -ne 0) {
    throw "ELF metadata capture failed in '$DockerImage' (exit $LASTEXITCODE)."
}

$importer = Join-Path $frameworkRoot "tools/symbol_import/import_decomp_symbols.py"
& python $importer `
    --id POKEMON_MODERN_EMERALD `
    --name "Pokemon Modern Emerald (USA)" `
    --syms (Join-Path $symbolsDir "readelf-symbols.txt") `
    --sections (Join-Path $symbolsDir "readelf-sections.txt") `
    --map $map `
    --rom $rom `
    --out $symbolsDir `
    --data-source auto
if ($LASTEXITCODE -ne 0) {
    throw "Symbol import failed (exit $LASTEXITCODE). Review warnings before proceeding."
}

$outputs = @(
    "readelf-symbols.txt",
    "readelf-sections.txt",
    "imported_symbols.tsv",
    "imported_data_symbols.tsv",
    "function_boundaries.tsv",
    "POKEMON_MODERN_EMERALD_symbols.toml"
)
$outputHashes = [ordered]@{}
foreach ($name in $outputs) {
    $path = Join-Path $symbolsDir $name
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        throw "Importer did not produce expected artifact: $path"
    }
    $outputHashes[$name] = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant()
}

$frameworkCommit = (& git -C $frameworkRoot rev-parse HEAD).Trim()
$manifest = [ordered]@{
    generated_utc = [DateTime]::UtcNow.ToString("o")
    framework_commit = $frameworkCommit
    helper_sha256 = (Get-FileHash -LiteralPath $MyInvocation.MyCommand.Path -Algorithm SHA256).Hash.ToLowerInvariant()
    importer_sha256 = (Get-FileHash -LiteralPath $importer -Algorithm SHA256).Hash.ToLowerInvariant()
    docker_image = $DockerImage
    docker_image_id = $imageId.Trim()
    rom_sha1 = $romSha1
    rom_sha256 = (Get-FileHash -LiteralPath $rom -Algorithm SHA256).Hash.ToLowerInvariant()
    elf_sha256 = $elfSha256
    map_sha256 = $mapSha256
    outputs_sha256 = $outputHashes
}
$json = $manifest | ConvertTo-Json -Depth 5
[IO.File]::WriteAllText(
    (Join-Path $symbolsDir "import-manifest.json"),
    $json + [Environment]::NewLine,
    [Text.UTF8Encoding]::new($false)
)
Write-Host "Wrote import provenance: $(Join-Path $symbolsDir 'import-manifest.json')"
