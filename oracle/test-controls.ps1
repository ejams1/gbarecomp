param(
    [Parameter(Mandatory = $true)] [string] $RomPath,
    [string] $BiosPath,
    [string] $Image = 'gbarecomp-mgba-oracle:0.10.5'
)

$ErrorActionPreference = 'Stop'
$romResolved = (Resolve-Path -LiteralPath $RomPath).Path
$runId = [guid]::NewGuid().ToString('N')
$scratchRoot = Join-Path ([IO.Path]::GetTempPath()) "mgba-oracle-controls-$runId"
$workers = @()

function Send-Request($Worker, [string] $Json) {
    $Worker.Writer.WriteLine($Json)
    $line = $Worker.Reader.ReadLine()
    if ($null -eq $line) { throw 'Oracle disconnected before replying.' }
    return $line | ConvertFrom-Json
}

function Assert-Ok($Response, [string] $Context) {
    if (-not $Response.ok) { throw "$Context failed: $($Response.error)" }
}

try {
    New-Item -ItemType Directory -Path $scratchRoot -Force | Out-Null
    for ($i = 0; $i -lt 2; $i++) {
        $scratch = Join-Path $scratchRoot "worker-$i"
        New-Item -ItemType Directory -Path $scratch -Force | Out-Null
        Copy-Item -LiteralPath $romResolved -Destination (Join-Path $scratch 'rom.gba')
        $probe = [System.Net.Sockets.TcpListener]::new([Net.IPAddress]::Loopback, 0)
        $probe.Start(); $hostPort = ([System.Net.IPEndPoint] $probe.LocalEndpoint).Port; $probe.Stop()
        $name = "mgba-oracle-controls-$runId-$i"
        $args = @('run', '--detach', '--name', $name,
            '--publish', "127.0.0.1:${hostPort}:19853",
            '--mount', "type=bind,source=$scratch,target=/work", $Image)
        if ($BiosPath) {
            $bios = (Resolve-Path -LiteralPath $BiosPath).Path
            Copy-Item -LiteralPath $bios -Destination (Join-Path $scratch 'bios.bin')
            $args += @('--bios', '/work/bios.bin')
        } else { $args += '--no-bios' }
        $args += @('--rom', '/work/rom.gba', '--port', '19853', '--bind', '0.0.0.0',
            '--rtc-mode', 'fixed', '--rtc-epoch-ms', '946684800000')
        & docker @args | Out-Null
        if ($LASTEXITCODE -ne 0) { throw "Could not start oracle worker $i." }
        $workers += [pscustomobject]@{ Name = $name; Port = $hostPort; Client = $null; Reader = $null; Writer = $null; Save = $null }
    }

    foreach ($worker in $workers) {
        $ready = $false
        for ($attempt = 0; $attempt -lt 40; $attempt++) {
            $client = [System.Net.Sockets.TcpClient]::new()
            try {
                $client.Connect('127.0.0.1', $worker.Port)
                $stream = $client.GetStream(); $stream.ReadTimeout = 30000; $stream.WriteTimeout = 30000
                $worker.Client = $client
                $worker.Reader = [IO.StreamReader]::new($stream, [Text.Encoding]::ASCII, $false, 1024, $true)
                $worker.Writer = [IO.StreamWriter]::new($stream, [Text.Encoding]::ASCII, 1024, $true)
                $worker.Writer.NewLine = "`n"; $worker.Writer.AutoFlush = $true
                $ping = Send-Request $worker '{"ping":true}'
                if ($ping.ok) { $ready = $true; break }
            } catch {
                if ($client) { $client.Dispose() }
                $worker.Client = $null
            }
            Start-Sleep -Milliseconds 250
        }
        if (-not $ready) { throw "Worker $($worker.Name) did not become ready." }
        $worker.Writer.Write('{"ping":'); $worker.Writer.Flush()
        Start-Sleep -Milliseconds 20
        $worker.Writer.Write('true}'); $worker.Writer.WriteLine(); $worker.Writer.Flush()
        $fragmented = $worker.Reader.ReadLine() | ConvertFrom-Json
        Assert-Ok $fragmented 'fragmented request framing'
    }

    foreach ($i in 0, 1) {
        $worker = $workers[$i]
        $caps = Send-Request $worker '{"emu_capabilities":true}'; Assert-Ok $caps 'capabilities'
        if ($caps.protocol -ne 2 -or $caps.mgba -ne '0.10.5' -or
            $caps.positive_key_mask -ne 1023 -or $caps.max_memory_read_bytes -ne 4096 -or
            $caps.max_audio_capture_samples -ne 16384 -or
            $caps.emulator_state_slots -ne 1 -or
            $caps.max_emulator_state_bytes -ne 4194304 -or
            $caps.state_api -ne 'mcore-in-memory-single-slot-v1') { throw 'Capability identity/limit mismatch.' }
        $compat = Send-Request $worker '{"cmd":"emu_phase"}'; Assert-Ok $compat 'cmd-field compatibility'
        $phase0 = Send-Request $worker '{"emu_phase":true}'; Assert-Ok $phase0 'initial phase'
        if ($phase0.reset_mode -ne $(if ($BiosPath) { 'bios' } else { 'hle' })) { throw 'Unexpected reset mode.' }
        $rtc = Send-Request $worker '{"emu_rtc_state":true}'; Assert-Ok $rtc 'fixed RTC'
        if ($rtc.mode -ne 'fixed' -or $rtc.epoch_seconds -ne 946684800) { throw 'Fixed RTC did not report the selected epoch.' }
        $mask = if ($i -eq 0) { 1 } else { 512 }
        $keys = Send-Request $worker ('{"emu_set_keys":true,"keys":' + $mask + '}'); Assert-Ok $keys 'positive key mask'
        if ($keys.keys_pressed -ne $mask) { throw 'Key mask did not round trip.' }
        $step = Send-Request $worker '{"emu_step_frames":true,"count":2}'; Assert-Ok $step 'two frame steps'
        if ($step.run_frame_calls -ne 2 -or $step.frame -ne 2) { throw 'Frame call count mismatch.' }
        $phase = Send-Request $worker '{"emu_phase":true}'; Assert-Ok $phase 'stepped phase'
        if ($phase.run_frame_calls -ne 2 -or $phase.video_frame_end_events -lt 1 -or
            $phase.core_frame_counter -lt 1 -or
            $phase.keys_pressed -ne $mask) {
            throw "Guest phase/input observations did not advance: calls=$($phase.run_frame_calls), core=$($phase.core_frame_counter), video=$($phase.video_frame_end_events), key_reads=$($phase.key_reads), keys=$($phase.keys_pressed), expected=$mask."
        }
        $savedState = Send-Request $worker '{"emu_state_save":true}'; Assert-Ok $savedState 'in-memory emulator state save'
        if ($savedState.state_id -le 0 -or $savedState.size -le 0 -or $savedState.size -gt 4194304) { throw 'Emulator state size/identity was invalid.' }
        $savedRegs = Send-Request $worker '{"emu_registers":true}'; Assert-Ok $savedRegs 'registers before state round trip'
        $advanced = Send-Request $worker '{"emu_step_frames":true,"count":2}'; Assert-Ok $advanced 'advance after state save'
        $advancedPhase = Send-Request $worker '{"emu_phase":true}'; Assert-Ok $advancedPhase 'phase after state advance'
        if ($advancedPhase.core_frame_counter -le $phase.core_frame_counter) { throw 'Guest core frame counter did not advance after state save.' }
        $restored = Send-Request $worker ('{"emu_state_load":true,"state_id":' + $savedState.state_id + '}'); Assert-Ok $restored 'in-memory emulator state restore'
        $restoredRegs = Send-Request $worker '{"emu_registers":true}'; Assert-Ok $restoredRegs 'registers after state restore'
        $restoredPhase = Send-Request $worker '{"emu_phase":true}'; Assert-Ok $restoredPhase 'phase after state restore'
        if ($restoredRegs.r15 -ne $savedRegs.r15 -or
            $restoredPhase.core_frame_counter -ne $phase.core_frame_counter) {
            throw 'mGBA core registers/frame counter did not round trip.'
        }
        $replacementState = Send-Request $worker '{"emu_state_save":true}'; Assert-Ok $replacementState 'replace in-memory emulator state'
        if ($replacementState.state_id -eq $savedState.state_id) { throw 'Replacing the emulator state did not issue a new ID.' }
        $replacedState = Send-Request $worker ('{"emu_state_load":true,"state_id":' + $savedState.state_id + '}')
        if ($replacedState.ok) { throw 'Superseded in-memory state ID remained valid.' }
        $staleState = Send-Request $worker '{"emu_state_load":true,"state_id":4294967295}'
        if ($staleState.ok) { throw 'Unknown in-memory state ID was accepted.' }
        $epoch = if ($i -eq 0) { 1700000000000 } else { 1800000000000 }
        $setRtc = Send-Request $worker ('{"emu_set_rtc":true,"mode":"tick","epoch_ms":' + $epoch + '}'); Assert-Ok $setRtc 'tick RTC setup'
        $beforeRtc = Send-Request $worker '{"emu_rtc_state":true}'; Assert-Ok $beforeRtc 'tick RTC state'
        $oneStep = Send-Request $worker '{"emu_step_frames":true,"count":1}'; Assert-Ok $oneStep 'tick RTC frame'
        $afterRtc = Send-Request $worker '{"emu_rtc_state":true}'; Assert-Ok $afterRtc 'advanced tick RTC'
        if ($afterRtc.epoch_seconds -lt $beforeRtc.epoch_seconds) { throw 'Tick RTC moved backwards.' }
        $reset = Send-Request $worker '{"emu_reset":true}'; Assert-Ok $reset 'mode-preserving reset'
        if ($reset.reset_mode -ne $phase0.reset_mode) { throw 'Reset changed BIOS/HLE mode.' }
        $afterReset = Send-Request $worker '{"emu_phase":true}'; Assert-Ok $afterReset 'reset phase'
        if ($afterReset.run_frame_calls -ne 0 -or $afterReset.video_frame_end_events -ne 0 -or $afterReset.keys_pressed -ne 0) { throw 'Reset did not clear phase counters and release input.' }
        $resetRtc = Send-Request $worker '{"emu_rtc_state":true}'; Assert-Ok $resetRtc 'RTC after reset'
        if ($resetRtc.epoch_seconds -ne [Math]::Floor($epoch / 1000)) { throw 'Reset did not restore the selected RTC anchor.' }
        $screen = Send-Request $worker '{"emu_screenshot":true}'; Assert-Ok $screen 'framebuffer capture'
        if ($screen.w -ne 240 -or $screen.h -ne 160 -or $screen.data.Length -ne 230400) { throw 'Screenshot capture shape mismatch.' }
        if ($i -eq 0) {
            $held = Send-Request $worker ('{"emu_set_keys":true,"keys":' + $mask + '}'); Assert-Ok $held 'held input after reset'
            $audioStep = Send-Request $worker '{"emu_step_frames":true,"count":700}'; Assert-Ok $audioStep 'bounded audio run'
            $polled = Send-Request $worker '{"emu_phase":true}'; Assert-Ok $polled 'guest key polling phase'
            if ($polled.key_reads -lt 1 -or $polled.keys_pressed -ne $mask) {
                $regs = Send-Request $worker '{"emu_registers":true}'
                throw "Guest did not poll the held positive input: key_reads=$($polled.key_reads), keys=$($polled.keys_pressed), pc=$($regs.r15), cpsr=$($regs.cpsr)."
            }
            $audio = Send-Request $worker '{"emu_audio_samples":true,"max":1}'; Assert-Ok $audio 'bounded audio capture'
            if ($audio.samples_dropped -eq 0) { throw 'Audio queue did not report bounded overflow after 700 frames.' }
        }

        $seed = if ($i -eq 0) { '00' } else { 'a5' }
        $saveHex = $seed * 131072
        $begin = Send-Request $worker '{"emu_save_import_begin":true,"size":131072}'; Assert-Ok $begin 'save import begin'
        for ($offset = 0; $offset -lt 131072; $offset += 2048) {
            $chunk = $saveHex.Substring($offset * 2, 2048 * 2)
            $request = '{"emu_save_import_chunk":true,"offset":' + $offset + ',"data":"' + $chunk + '"}'
            $reply = Send-Request $worker $request; Assert-Ok $reply 'save import chunk'
            if ($reply.next_offset -ne ($offset + 2048)) { throw 'Save import chunk offset mismatch.' }
        }
        $commit = Send-Request $worker '{"emu_save_import_commit":true}'; Assert-Ok $commit 'save import commit'
        $export = Send-Request $worker '{"emu_save_export_begin":true}'; Assert-Ok $export 'save export begin'
        if ($export.size -ne 131072) { throw "Expected 131072 byte exported save, got $($export.size)." }
        $parts = [Collections.Generic.List[string]]::new()
        for ($offset = 0; $offset -lt $export.size; $offset += 2048) {
            $count = [Math]::Min(2048, $export.size - $offset)
            $part = Send-Request $worker ('{"emu_save_export_chunk":true,"offset":' + $offset + ',"count":' + $count + '}')
            Assert-Ok $part 'save export chunk'; $parts.Add($part.data)
        }
        $null = Send-Request $worker '{"emu_save_export_end":true}'
        $worker.Save = [string]::Concat($parts)
        if ($worker.Save -ne $saveHex) { throw "Worker $i did not export its imported save bytes." }
    }

    $badMask = Send-Request $workers[0] '{"emu_set_keys":true,"keys":1024}'
    if ($badMask.ok) { throw 'Out of range key mask was accepted.' }
    $badWidth = Send-Request $workers[0] '{"emu_write":true,"addr":0,"data":"00","width":24}'
    if ($badWidth.ok) { throw 'Unsupported write width was accepted.' }
    $badBatch = Send-Request $workers[0] '{"emu_step_frames":true,"count":3601}'
    if ($badBatch.ok) { throw 'Oversized frame batch was accepted.' }
    $badAudio = Send-Request $workers[0] '{"emu_audio_samples":true,"max":16385}'
    if ($badAudio.ok) { throw 'Oversized audio capture request was accepted.' }
    $badRegs = Send-Request $workers[0] '{"emu_set_regs":true,"r0":4294967296}'
    if ($badRegs.ok) { throw 'Out of range register value was accepted.' }
    $malformed = Send-Request $workers[0] '{"emu_set_keys":true,"keys":1,"keys":2}'
    if ($malformed.ok) { throw 'Duplicate JSON property was accepted.' }
    if ($workers[0].Save -eq $workers[1].Save) { throw 'Workers unexpectedly exported identical save fixtures.' }

    $disconnectWorker = $workers[1]
    $disconnectWorker.Writer.Write('{"ping":'); $disconnectWorker.Writer.Flush()
    $disconnectWorker.Reader.Dispose(); $disconnectWorker.Writer.Dispose(); $disconnectWorker.Client.Dispose()
    $disconnectWorker.Client = $null
    $reconnected = $false
    for ($attempt = 0; $attempt -lt 20; $attempt++) {
        $client = [System.Net.Sockets.TcpClient]::new()
        try {
            $client.Connect('127.0.0.1', $disconnectWorker.Port)
            $stream = $client.GetStream(); $stream.ReadTimeout = 30000; $stream.WriteTimeout = 30000
            $disconnectWorker.Client = $client
            $disconnectWorker.Reader = [IO.StreamReader]::new($stream, [Text.Encoding]::ASCII, $false, 1024, $true)
            $disconnectWorker.Writer = [IO.StreamWriter]::new($stream, [Text.Encoding]::ASCII, 1024, $true)
            $disconnectWorker.Writer.NewLine = "`n"; $disconnectWorker.Writer.AutoFlush = $true
            $ping = Send-Request $disconnectWorker '{"cmd":"ping"}'
            if ($ping.ok) { $reconnected = $true; break }
        } catch {
            if ($client) { $client.Dispose() }
            $disconnectWorker.Client = $null
        }
        Start-Sleep -Milliseconds 100
    }
    if (-not $reconnected) { throw 'Oracle did not accept a fresh client after a partial-request disconnect.' }

    foreach ($worker in $workers) {
        $quit = Send-Request $worker '{"quit":true}'; Assert-Ok $quit 'quit'
        $worker.Reader.Dispose(); $worker.Writer.Dispose(); $worker.Client.Dispose(); $worker.Client = $null
    }
    foreach ($worker in $workers) {
        $exitCode = & docker wait $worker.Name
        if ($LASTEXITCODE -ne 0 -or $exitCode -ne '0') { throw "Oracle exited with $exitCode" }
    }
    $modeName = if ($BiosPath) { 'BIOS' } else { 'HLE' }
    Write-Output "PASS oracle controls: two isolated $modeName workers; phase/input/RTC/reset/capture/save/malformed-request checks."
}
finally {
    foreach ($worker in $workers) {
        if ($worker.Client) { $worker.Client.Dispose() }
        & docker rm -f $worker.Name 2>$null | Out-Null
    }
    $tempRoot = [IO.Path]::GetFullPath([IO.Path]::GetTempPath())
    $full = [IO.Path]::GetFullPath($scratchRoot)
    if ($full.StartsWith($tempRoot, [StringComparison]::OrdinalIgnoreCase) -and
        (Split-Path -Leaf $full) -eq "mgba-oracle-controls-$runId") {
        Remove-Item -LiteralPath $full -Recurse -Force
    }
}
