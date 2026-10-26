[CmdletBinding()]
param(
    [string]$GameRepo = (Join-Path $PSScriptRoot "../../../pokeemerald-modern-adult"),
    [string]$DockerImage = "adult-emerald-build:r63-libpng",
    [string]$HostDockerImage = "emerald-adult-host-build",
    [switch]$RegenerateIdentity
)

$ErrorActionPreference = "Stop"
if (Get-Variable -Name PSNativeCommandUseErrorActionPreference -ErrorAction SilentlyContinue) {
    $PSNativeCommandUseErrorActionPreference = $false
}
$script:NativeExitCode = $null

function Invoke-Native {
    param(
        [Parameter(Mandatory = $true)][string]$Executable,
        [Parameter(Mandatory = $true)][string[]]$Arguments,
        [Parameter(Mandatory = $true)][string]$Description
    )

    Write-Host "==> $Description"
    & $Executable @Arguments
    $exitCode = $LASTEXITCODE
    if ($exitCode -ne 0) {
        $script:NativeExitCode = $exitCode
        throw "$Description failed with exit code $exitCode."
    }
}

function Get-Hash {
    param([string]$Path, [ValidateSet("SHA1", "SHA256")][string]$Algorithm)
    return (Get-FileHash -LiteralPath $Path -Algorithm $Algorithm).Hash.ToLowerInvariant()
}

function Read-RomPin {
    param([string]$Path)
    $content = [IO.File]::ReadAllText($Path)
    $section = [regex]::Match($content, '(?ms)^\[identity\]\s*(?<body>.*?)(?=^\[|\z)')
    if (-not $section.Success) { throw "Missing [identity] section in $Path." }
    $pin = [regex]::Match($section.Groups["body"].Value, '(?m)^\s*sha1\s*=\s*"(?<hash>[0-9a-fA-F]{40})"\s*(?:#.*)?$')
    if (-not $pin.Success) { throw "Missing lowercase/uppercase SHA-1 identity.sha1 in $Path." }
    return $pin.Groups["hash"].Value.ToLowerInvariant()
}

function Write-RomPin {
    param([string]$Path, [string]$Sha1)
    $content = [IO.File]::ReadAllText($Path)
    foreach ($name in @("identity", "rom")) {
        $section = [regex]::Match($content, "(?ms)^\[$name\]\s*(?<body>.*?)(?=^\[|\z)")
        if (-not $section.Success) { throw "Missing [$name] section in $Path." }
        $body = $section.Groups["body"].Value
        $pinMatches = [regex]::Matches($body, '(?m)^(\s*sha1\s*=\s*")[0-9a-fA-F]{40}("\s*(?:#.*)?)$')
        if ($pinMatches.Count -ne 1) { throw "Expected exactly one $name.sha1 in $Path; found $($pinMatches.Count)." }
        $pinPattern = [regex]'(?m)^(\s*sha1\s*=\s*")[0-9a-fA-F]{40}("\s*(?:#.*)?)$'
        $newBody = $pinPattern.Replace($body, [System.Text.RegularExpressions.MatchEvaluator]{
            param($match)
            return $match.Groups[1].Value + $Sha1 + $match.Groups[2].Value
        })
        $content = $content.Substring(0, $section.Groups["body"].Index) + $newBody +
            $content.Substring($section.Groups["body"].Index + $section.Groups["body"].Length)
    }
    [IO.File]::WriteAllText($Path, $content, [Text.UTF8Encoding]::new($false))
}

function Assert-GeneratedPath {
    param([string]$Path, [string]$HostRoot)
    $full = [IO.Path]::GetFullPath($Path).TrimEnd([IO.Path]::DirectorySeparatorChar, [IO.Path]::AltDirectorySeparatorChar)
    $root = [IO.Path]::GetFullPath($HostRoot).TrimEnd([IO.Path]::DirectorySeparatorChar, [IO.Path]::AltDirectorySeparatorChar)
    $prefix = $root + [IO.Path]::DirectorySeparatorChar
    if (-not $full.StartsWith($prefix, [StringComparison]::OrdinalIgnoreCase)) {
        throw "Refusing generated-output cleanup outside the host project: $full"
    }
    $relative = $full.Substring($prefix.Length).Replace('\', '/').ToLowerInvariant()
    if ($relative -notin @("generated", "generated_bios", "build-pipeline")) {
        throw "Refusing cleanup of unexpected host path: $full"
    }
    return $full
}

function Remove-StaleGeneratedOutputs {
    param([string[]]$Paths, [string]$HostRoot)
    foreach ($path in $Paths) {
        $safe = Assert-GeneratedPath -Path $path -HostRoot $HostRoot
        if (Test-Path -LiteralPath $safe) {
            Write-Host "Removing stale generated output: $safe"
            Remove-Item -LiteralPath $safe -Recurse -Force
        }
    }
}

function Read-ImportManifest {
    param([string]$Path)
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) { return $null }
    try { return Get-Content -LiteralPath $Path -Raw | ConvertFrom-Json }
    catch { throw "Cannot read importer provenance ${Path}: $($_.Exception.Message)" }
}

function Test-ImportManifestMatches {
    param($Manifest, [string]$RomSha1, [string]$ElfSha256, [string]$MapSha256,
          [string]$FrameworkCommit, [string]$SymbolsDir)
    if ($null -eq $Manifest) { return $false }
    if ($Manifest.rom_sha1 -ne $RomSha1 -or $Manifest.elf_sha256 -ne $ElfSha256 -or
        $Manifest.map_sha256 -ne $MapSha256 -or $Manifest.framework_commit -ne $FrameworkCommit) { return $false }
    foreach ($name in $Manifest.outputs_sha256.PSObject.Properties.Name) {
        $path = Join-Path $SymbolsDir $name
        if (-not (Test-Path -LiteralPath $path -PathType Leaf) -or
            (Get-Hash $path SHA256) -ne $Manifest.outputs_sha256.$name) { return $false }
    }
    return $true
}

try {
    $hostRoot = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot ".")).Path
    $frameworkRoot = (Resolve-Path -LiteralPath (Join-Path $hostRoot "../..")).Path
    $workspaceRoot = (Resolve-Path -LiteralPath (Join-Path $frameworkRoot "..")).Path
    $gameRoot = (Resolve-Path -LiteralPath $GameRepo).Path
    $gameConfig = Join-Path $hostRoot "emerald.toml"
    $symbolsDir = Join-Path $hostRoot "symbols"
    $importManifestPath = Join-Path $symbolsDir "import-manifest.json"
    $pipelineManifestPath = Join-Path $symbolsDir "build-manifest.json"
    $rom = Join-Path $gameRoot "pokeemerald_modern.gba"
    $elf = Join-Path $gameRoot "pokeemerald_modern.elf"
    $map = Join-Path $gameRoot "pokeemerald_modern.map"
    $bios = Join-Path $frameworkRoot "bios/gba_bios.bin"
    $biosConfig = Join-Path $frameworkRoot "bios/gba_bios.toml"
    $recompilerBuild = Join-Path $frameworkRoot "build-pipeline/cli"
    $recompiler = Join-Path $recompilerBuild "gba_recompile"
    $cartOutput = Join-Path $hostRoot "generated"
    $biosOutput = Join-Path $hostRoot "generated_bios"
    $hostBuild = Join-Path $hostRoot "build-pipeline"
    $importScript = Join-Path $hostRoot "tools/import-symbols.ps1"
    $sceneStateGenerator = Join-Path $hostRoot "tools/generate-scene-state-address.py"
    $sceneStateAddressHeader = Join-Path $hostRoot "generated/host_scene_state_address.h"
    $pythonImporter = Join-Path $frameworkRoot "tools/symbol_import/import_decomp_symbols.py"

    foreach ($required in @($gameConfig, $bios, $biosConfig, $importScript, $pythonImporter, $sceneStateGenerator)) {
        if (-not (Test-Path -LiteralPath $required -PathType Leaf)) {
            throw "Required pipeline input is missing: $required"
        }
    }

    $null = Assert-GeneratedPath -Path $cartOutput -HostRoot $hostRoot
    $null = Assert-GeneratedPath -Path $biosOutput -HostRoot $hostRoot
    $null = Assert-GeneratedPath -Path $hostBuild -HostRoot $hostRoot

    $imageInfo = & docker image inspect --format '{{.Id}}' $DockerImage
    $dockerExitCode = $LASTEXITCODE
    if ($dockerExitCode -ne 0 -or -not $imageInfo) {
        $script:NativeExitCode = if ($dockerExitCode -ne 0) { $dockerExitCode } else { 1 }
        throw "Pinned ROM build image '$DockerImage' is unavailable. Build it using the game repo's docs/build-notes.md."
    }
    $dockerImageId = ([string]$imageInfo).Trim()

    $hostImageInfo = & docker image inspect --format '{{.Id}}' $HostDockerImage 2>$null
    $hostImageExitCode = $LASTEXITCODE
    if ($hostImageExitCode -ne 0 -or -not $hostImageInfo) {
        $dockerfile = Join-Path $hostRoot "Dockerfile"
        Invoke-Native docker @("build", "-f", $dockerfile, "-t", $HostDockerImage, $hostRoot) `
            "Build the pinned host toolchain image"
        $hostImageInfo = & docker image inspect --format '{{.Id}}' $HostDockerImage
        if ($LASTEXITCODE -ne 0 -or -not $hostImageInfo) {
            $script:NativeExitCode = if ($LASTEXITCODE -ne 0) { $LASTEXITCODE } else { 1 }
            throw "Host toolchain image '$HostDockerImage' was built but could not be inspected."
        }
    }
    $hostDockerImageId = ([string]$hostImageInfo).Trim()

    Invoke-Native docker @(
        "run", "--rm", "--mount", "type=bind,source=$gameRoot,target=/work",
        "-w", "/work", $DockerImage, "sh", "-lc", "make modern"
    ) "Pinned make modern ROM build"

    foreach ($artifact in @($rom, $elf, $map)) {
        if (-not (Test-Path -LiteralPath $artifact -PathType Leaf)) {
            throw "Pinned ROM build did not produce required artifact: $artifact"
        }
    }
    if (-not (Test-Path -LiteralPath $bios -PathType Leaf)) {
        throw "Required user-provided BIOS is missing: $bios. Supply the BIOS file locally; it is not distributed with this project."
    }

    $romSha1 = Get-Hash $rom SHA1
    $elfSha256 = Get-Hash $elf SHA256
    $mapSha256 = Get-Hash $map SHA256
    $biosSha1 = Get-Hash $bios SHA1
    $oldRomSha1 = Read-RomPin $gameConfig
    $frameworkCommit = (& git -C $frameworkRoot rev-parse HEAD).Trim()
    if ($LASTEXITCODE -ne 0) {
        $script:NativeExitCode = $LASTEXITCODE
        throw "Unable to read the framework Git revision."
    }
    $frameworkStatus = (& git -C $frameworkRoot status --porcelain | Out-String).Trim()
    if ($LASTEXITCODE -ne 0) {
        $script:NativeExitCode = $LASTEXITCODE
        throw "Unable to inspect framework working-tree state."
    }

    $mount = "type=bind,source=$workspaceRoot,target=/workspace"
    $gameMount = "type=bind,source=$gameRoot,target=/game"
    $hostToolInfo = & docker run --rm --mount $mount --entrypoint bash $HostDockerImage `
        -lc "cmake --version | head -n 1; ninja --version; clang --version | head -n 1"
    $hostToolExitCode = $LASTEXITCODE
    if ($hostToolExitCode -ne 0) {
        $script:NativeExitCode = $hostToolExitCode
        throw "Unable to query versions from the pinned host toolchain image."
    }
    $hostToolInfo = @($hostToolInfo | ForEach-Object { ([string]$_).Trim() }) -join "; "

    $importManifest = Read-ImportManifest $importManifestPath
    $importsMatch = Test-ImportManifestMatches $importManifest $romSha1 $elfSha256 $mapSha256 $frameworkCommit $symbolsDir
    $drift = @()
    if ($oldRomSha1 -ne $romSha1) { $drift += "ROM SHA-1 old=$oldRomSha1 new=$romSha1" }
    if ($importManifest -and $importManifest.rom_sha1 -ne $romSha1) {
        $drift += "imported ROM SHA-1 old=$($importManifest.rom_sha1) new=$romSha1"
    }
    if ($importManifest -and $importManifest.elf_sha256 -ne $elfSha256) {
        $drift += "ELF SHA-256 old=$($importManifest.elf_sha256) new=$elfSha256"
    }
    if ($importManifest -and $importManifest.map_sha256 -ne $mapSha256) {
        $drift += "link-map SHA-256 old=$($importManifest.map_sha256) new=$mapSha256"
    }
    if ($importManifest -and $importManifest.framework_commit -ne $frameworkCommit) {
        $drift += "framework commit old=$($importManifest.framework_commit) new=$frameworkCommit"
    }
    $generationManifest = $null
    if (Test-Path -LiteralPath $pipelineManifestPath -PathType Leaf) {
        try { $generationManifest = Get-Content -LiteralPath $pipelineManifestPath -Raw | ConvertFrom-Json }
        catch { throw "Cannot read pipeline provenance ${pipelineManifestPath}: $($_.Exception.Message)" }
    }

    if (-not $RegenerateIdentity) {
        if ($drift.Count -gt 0 -or -not $importsMatch -or $null -eq $generationManifest) {
            if ($drift.Count -eq 0) {
                $drift += "import or generation provenance is missing/stale for ROM SHA-1 old=$oldRomSha1 new=$romSha1"
            }
            Write-Host ($drift -join [Environment]::NewLine) -ForegroundColor Red
            Write-Host "Stop: derived artifacts are stale. To explicitly derive identities from this pinned make modern output, run:"
            Write-Host "  & `"$($MyInvocation.MyCommand.Path)`" -GameRepo `"$gameRoot`" -RegenerateIdentity"
            exit 1
        }
    }
    else {
        Write-Host "Explicit regeneration requested; deriving identities from pinned build output."
    }

    $importOutputHashes = $null
    if ($RegenerateIdentity) {
        $powerShellExe = (Get-Process -Id $PID).Path
        Invoke-Native $powerShellExe @(
            "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", $importScript,
            "-GameRepo", $gameRoot, "-DockerImage", $DockerImage,
            "-ExpectedRomSha1", $romSha1,
            "-ExpectedElfSha256", $elfSha256,
            "-ExpectedMapSha256", $mapSha256
        ) "ELF metadata capture and symbol import"
        Write-RomPin -Path $gameConfig -Sha1 $romSha1
        $importManifest = Read-ImportManifest $importManifestPath
        $importsMatch = Test-ImportManifestMatches $importManifest $romSha1 $elfSha256 $mapSha256 $frameworkCommit $symbolsDir
        if (-not $importsMatch) { throw "Symbol importer completed but its provenance does not match the built ROM/ELF/map/framework." }
        Remove-StaleGeneratedOutputs -Paths @($cartOutput, $biosOutput, $hostBuild) -HostRoot $hostRoot
    }
    elseif (-not $importsMatch) {
        throw "Imported symbol metadata is stale; run build-exe.ps1 -RegenerateIdentity explicitly."
    }

    Invoke-Native docker @(
        "run", "--rm", "--mount", $mount, "-w", "/workspace/gbarecomp",
        $HostDockerImage, "bash", "-lc",
        "cmake -S . -B build-pipeline/cli -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ && cmake --build build-pipeline/cli --target gba_recompile --parallel 8"
    ) "Configure and build recompiler CLI in pinned host container"
    if (-not (Test-Path -LiteralPath $recompiler -PathType Leaf)) {
        throw "CMake did not produce the recompiler CLI at $recompiler"
    }

    $recompilerContainerPath = "/workspace/gbarecomp/build-pipeline/cli/gba_recompile"
    $cliRevision = "gbarecomp@$frameworkCommit"
    $recompilerSha256 = Get-Hash $recompiler SHA256
    $importManifestSha256 = Get-Hash $importManifestPath SHA256
    $generatedInputs = [ordered]@{
        framework_commit = $frameworkCommit
        framework_worktree = if ($frameworkStatus) { "dirty" } else { "clean" }
        recompiler_version = $cliRevision
        recompiler_sha256 = $recompilerSha256
        rom_sha1 = $romSha1
        elf_sha256 = $elfSha256
        map_sha256 = $mapSha256
        bios_sha1 = $biosSha1
        docker_image = $DockerImage
        docker_image_id = $dockerImageId
        host_docker_image = $HostDockerImage
        host_docker_image_id = $hostDockerImageId
        host_tool_versions = $hostToolInfo
        import_manifest_sha256 = $importManifestSha256
        scene_state_generator_sha256 = Get-Hash $sceneStateGenerator SHA256
        base_config_sha256 = Get-Hash $gameConfig SHA256
        bios_config_sha256 = Get-Hash $biosConfig SHA256
    }
    $generatedInputsJson = $generatedInputs | ConvertTo-Json -Compress
    $fingerprintBytes = [Security.Cryptography.SHA256]::Create().ComputeHash([Text.Encoding]::UTF8.GetBytes($generatedInputsJson))
    $generationFingerprint = ([BitConverter]::ToString($fingerprintBytes)).Replace("-", "").ToLowerInvariant()
    if (-not $RegenerateIdentity -and
        $generationManifest.generation_fingerprint -ne $generationFingerprint) {
        Write-Host "Generation fingerprint drift: old=$($generationManifest.generation_fingerprint) new=$generationFingerprint" -ForegroundColor Red
        Write-Host "Stop: generated output identity changed. To explicitly regenerate it from this pinned build, run:"
        Write-Host "  & `"$($MyInvocation.MyCommand.Path)`" -GameRepo `"$gameRoot`" -RegenerateIdentity"
        exit 1
    }
    $reuseGenerated = $false
    if (-not $RegenerateIdentity -and $generationManifest -and
        $generationManifest.generation_fingerprint -eq $generationFingerprint -and
        (Test-Path -LiteralPath (Join-Path $cartOutput "dispatch_table.cpp")) -and
        (Test-Path -LiteralPath (Join-Path $biosOutput "bios_dispatch_table.cpp"))) {
        $reuseGenerated = $true
        Write-Host "Generated cartridge and BIOS sources match the recorded input fingerprint; reusing them."
    }
    if (-not $reuseGenerated) {
        Remove-StaleGeneratedOutputs -Paths @($cartOutput, $biosOutput, $hostBuild) -HostRoot $hostRoot
        New-Item -ItemType Directory -Force -Path $cartOutput, $biosOutput | Out-Null
        Invoke-Native docker @(
            "run", "--rm", "--mount", $mount, "-w", "/workspace/gbarecomp",
            $HostDockerImage, $recompilerContainerPath,
            "--bios", "/workspace/gbarecomp/bios/gba_bios.bin",
            "--config", "/workspace/gbarecomp/bios/gba_bios.toml",
            "--out", "/workspace/gbarecomp/games/emerald_adult/generated_bios"
        ) "Generate BIOS translation"
        $symbolOverlay = Join-Path $symbolsDir "POKEMON_MODERN_EMERALD_symbols.toml"
        $functionSymbols = Join-Path $symbolsDir "imported_symbols.tsv"
        $dataSymbols = Join-Path $symbolsDir "imported_data_symbols.tsv"
        foreach ($input in @($symbolOverlay, $functionSymbols, $dataSymbols)) {
            if (-not (Test-Path -LiteralPath $input -PathType Leaf)) { throw "Symbol importer output is missing: $input" }
        }
        Invoke-Native docker @(
            "run", "--rm", "--mount", $mount, "--mount", $gameMount, "-w", "/workspace/gbarecomp",
            $HostDockerImage, $recompilerContainerPath,
            "--rom", "/game/pokeemerald_modern.gba",
            "--symbols", "/workspace/gbarecomp/games/emerald_adult/symbols/imported_symbols.tsv",
            "--data-symbols", "/workspace/gbarecomp/games/emerald_adult/symbols/imported_data_symbols.tsv",
            "--config", "/workspace/gbarecomp/games/emerald_adult/emerald.toml",
            "--config", "/workspace/gbarecomp/games/emerald_adult/symbols/POKEMON_MODERN_EMERALD_symbols.toml",
            "--out", "/workspace/gbarecomp/games/emerald_adult/generated"
        ) "Generate cartridge translation"
        if (-not (Test-Path -LiteralPath (Join-Path $cartOutput "dispatch_table.cpp")) -or
            -not (Test-Path -LiteralPath (Join-Path $biosOutput "bios_dispatch_table.cpp"))) {
            throw "Recompiler did not produce the expected BIOS and cartridge dispatch tables."
        }
        $generationRecord = [ordered]@{
            schema_version = 1
            generated_utc = [DateTime]::UtcNow.ToString("o")
            generation_fingerprint = $generationFingerprint
            inputs = $generatedInputs
        }
        [IO.File]::WriteAllText($pipelineManifestPath,
            (($generationRecord | ConvertTo-Json -Depth 6) + [Environment]::NewLine),
            [Text.UTF8Encoding]::new($false))
    }

    # Cartridge generation replaces the entire generated directory. Emit the
    # derived scene-state header after that step so a clean regeneration keeps it.
    Invoke-Native python @(
        $sceneStateGenerator,
        "--symbols", (Join-Path $symbolsDir "imported_data_symbols.tsv"),
        "--out", $sceneStateAddressHeader
    ) "Resolve the versioned guest scene-state symbol from matching ELF metadata"

    Invoke-Native docker @(
        "run", "--rm", "--mount", $mount, "-w", "/workspace/gbarecomp/games/emerald_adult",
        $HostDockerImage, "bash", "-lc",
        "cmake -S . -B build-pipeline -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ -DGBARECOMP_GENERATED_BIOS_DIR=/workspace/gbarecomp/games/emerald_adult/generated_bios -DGBARECOMP_GENERATED_DIR=/workspace/gbarecomp/games/emerald_adult/generated && cmake --build build-pipeline --target EmeraldAdultHost --parallel 8"
    ) "Configure and build Emerald host in pinned host container"

    $exe = Join-Path $hostBuild "EmeraldAdultHost"
    if (-not (Test-Path -LiteralPath $exe -PathType Leaf)) { throw "Host build completed without an EmeraldAdultHost executable." }

    $smokePng = Join-Path $hostBuild "startup-smoke.png"
    Invoke-Native docker @(
        "run", "--rm", "--mount", $mount, "-w", "/workspace/gbarecomp/games/emerald_adult",
        "-e", "RECOMP_RTC_EPOCH=946684800",
        "-e", "GBARECOMP_INPUT_REPLAY=/workspace/gbarecomp/games/emerald_adult/tools/title-start.input.csv",
        "-e", "SDL_VIDEODRIVER=dummy", "-e", "SDL_AUDIODRIVER=dummy",
        $HostDockerImage, "./build-pipeline/EmeraldAdultHost",
        "--no-window", "--frames", "1", "--save", "/tmp/emerald-adult-pipeline.sav",
        "--dump-png", "/workspace/gbarecomp/games/emerald_adult/build-pipeline/startup-smoke.png"
    ) "Run the deterministic one-frame startup smoke check"
    if (-not (Test-Path -LiteralPath $smokePng -PathType Leaf)) {
        throw "Host smoke route completed without producing $smokePng"
    }
    $buildRecord = [ordered]@{
        schema_version = 1
        built_utc = [DateTime]::UtcNow.ToString("o")
        framework_commit = $frameworkCommit
        recompiler_version = $cliRevision
        recompiler_sha256 = $recompilerSha256
        host_tool_versions = $hostToolInfo
        docker_image = $DockerImage
        docker_image_id = $dockerImageId
        host_docker_image = $HostDockerImage
        host_docker_image_id = $hostDockerImageId
        rom_sha1 = $romSha1
        elf_sha256 = $elfSha256
        map_sha256 = $mapSha256
        bios_sha1 = $biosSha1
        executable_sha256 = Get-Hash $exe SHA256
        startup_smoke_sha256 = Get-Hash $smokePng SHA256
        generation_fingerprint = $generationFingerprint
    }
    [IO.File]::WriteAllText($pipelineManifestPath,
        (($buildRecord | ConvertTo-Json -Depth 6) + [Environment]::NewLine),
        [Text.UTF8Encoding]::new($false))
    Write-Host "Build complete: $exe"
    Write-Host "ROM SHA-1: $romSha1"
    Write-Host "Provenance: $pipelineManifestPath"
    exit 0
}
catch {
    Write-Host $_ -ForegroundColor Red
    if ($null -ne $script:NativeExitCode -and $script:NativeExitCode -ne 0) {
        exit $script:NativeExitCode
    }
    exit 1
}
