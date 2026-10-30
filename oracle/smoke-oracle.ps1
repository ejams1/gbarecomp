param(
    [Parameter(Mandatory = $true)]
    [string] $RomPath,

    [string] $BiosPath,
    [string] $Image = 'gbarecomp-mgba-oracle:0.10.5'
)

$ErrorActionPreference = 'Stop'

$romResolved = (Resolve-Path -LiteralPath $RomPath).Path
$runId = [guid]::NewGuid().ToString('N')
$scratch = Join-Path ([IO.Path]::GetTempPath()) "mgba-oracle-smoke-$runId"
$containerName = "mgba-oracle-smoke-$runId"
$client = $null
$containerStarted = $false

try {
    New-Item -ItemType Directory -Path $scratch -Force | Out-Null
    Copy-Item -LiteralPath $romResolved -Destination (Join-Path $scratch 'rom.gba')
    $romSha1 = (Get-FileHash -LiteralPath (Join-Path $scratch 'rom.gba') -Algorithm SHA1).Hash

    $portProbe = [System.Net.Sockets.TcpListener]::new([Net.IPAddress]::Loopback, 0)
    $portProbe.Start()
    $hostPort = ([Net.IPEndPoint] $portProbe.LocalEndpoint).Port
    $portProbe.Stop()

    $dockerArgs = @(
        'run', '--detach', '--name', $containerName,
        '--publish', "127.0.0.1:${hostPort}:19853",
        '--mount', "type=bind,source=$scratch,target=/work",
        $Image
    )
    if ($BiosPath) {
        $biosResolved = (Resolve-Path -LiteralPath $BiosPath).Path
        Copy-Item -LiteralPath $biosResolved -Destination (Join-Path $scratch 'bios.bin')
        $dockerArgs += @('--bios', '/work/bios.bin')
    }
    else {
        $dockerArgs += '--no-bios'
    }
    $dockerArgs += @('--rom', '/work/rom.gba', '--port', '19853', '--bind', '0.0.0.0')

    & docker @dockerArgs | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "docker run failed with exit code $LASTEXITCODE" }
    $containerStarted = $true

    $ready = $false
    for ($attempt = 0; $attempt -lt 40; $attempt++) {
        $client = [System.Net.Sockets.TcpClient]::new()
        try {
            $client.Connect('127.0.0.1', $hostPort)
            $stream = $client.GetStream()
            $stream.ReadTimeout = 30000
            $reader = [IO.StreamReader]::new($stream, [Text.Encoding]::ASCII, $false, 1024, $true)
            $writer = [IO.StreamWriter]::new($stream, [Text.Encoding]::ASCII, 1024, $true)
            $writer.NewLine = "`n"
            $writer.AutoFlush = $true
            $writer.WriteLine('{"ping":true}')
            $ping = $reader.ReadLine() | ConvertFrom-Json
            if ($ping.ok -and $ping.who -eq 'gbarecomp_oracle') { $ready = $true; break }
        }
        catch {
            if ($client) { $client.Dispose(); $client = $null }
        }
        Start-Sleep -Milliseconds 250
    }
    if (-not $ready) { throw 'Oracle ping did not become ready on host loopback.' }

    $writer.WriteLine('{"emu_step_frames":true,"count":1}')
    $step = $reader.ReadLine() | ConvertFrom-Json
    if (-not $step.ok -or $step.frame -ne 1) { throw 'One-frame step assertion failed.' }

    $writer.WriteLine('{"read_emu_iwram":true,"addr":0,"len":4}')
    $memory = $reader.ReadLine() | ConvertFrom-Json
    if (-not $memory.ok -or $memory.len -ne 4 -or $memory.data -notmatch '^[0-9a-fA-F]{8}$') {
        throw 'Four-byte IWRAM read assertion failed.'
    }

    $writer.WriteLine('{"emu_screenshot":true}')
    $screen = $reader.ReadLine() | ConvertFrom-Json
    if (-not $screen.ok -or $screen.w -ne 240 -or $screen.h -ne 160 -or
        $screen.data -notmatch '^[0-9a-fA-F]{230400}$') {
        throw 'RGB888 screenshot size/encoding assertion failed.'
    }

    $writer.WriteLine('{"quit":true}')
    $quit = $reader.ReadLine() | ConvertFrom-Json
    if (-not $quit.ok -or -not $quit.bye) { throw 'Quit assertion failed.' }
    $reader.Dispose()
    $writer.Dispose()
    $client.Dispose()
    $client = $null

    $exitCode = & docker wait $containerName
    if ($LASTEXITCODE -ne 0 -or $exitCode -ne '0') { throw "Oracle container exited with $exitCode" }
    Write-Output "PASS mGBA oracle smoke: ROM SHA-1=$romSha1; frame=$($step.frame); IWRAM=4 bytes; screenshot=240x160 RGB888 (115200 bytes); exit=$exitCode"
}
finally {
    if ($client) { $client.Dispose() }
    if ($containerStarted) { & docker rm -f $containerName | Out-Null }
    $tempRoot = [IO.Path]::GetFullPath([IO.Path]::GetTempPath())
    $scratchFull = [IO.Path]::GetFullPath($scratch)
    if ($scratchFull.StartsWith($tempRoot, [StringComparison]::OrdinalIgnoreCase) -and
        (Split-Path -Leaf $scratchFull) -eq "mgba-oracle-smoke-$runId") {
        Remove-Item -LiteralPath $scratchFull -Recurse -Force
    }
}
