# Double-click Reset-ClawStage.cmd. Windows PowerShell 5.1+, Windows x64.
[CmdletBinding()]
param(
    [string]$Device,
    [ValidateRange(1,65535)][int]$Port = 22,
    [switch]$InspectOnly,
    [switch]$NewReset,
    [switch]$Follow,
    [switch]$VerifyOnly
)
$ErrorActionPreference = 'Stop'
$exitCode = 1
$pwFile = $null
$deviceMutex = $null
$ownsMutex = $false
function Get-ResetLogTime {
    [DateTimeOffset]::UtcNow.ToOffset([TimeSpan]::FromHours(8))
}
$logPath = Join-Path $PSScriptRoot ("Reset-ClawStage-{0}-{1}.log" -f (Get-ResetLogTime).ToString('yyyyMMdd-HHmmss'), [guid]::NewGuid().ToString('N').Substring(0,8))
$utf8 = New-Object Text.UTF8Encoding($false)
function Write-ResetLog([string]$Message) {
    $line = '[{0}+0800] {1}' -f (Get-ResetLogTime).ToString('yyyy-MM-ddTHH:mm:ss'), $Message.Replace('goodlife', '[REDACTED]')
    [IO.File]::AppendAllText($logPath, $line + [Environment]::NewLine, $utf8)
    Write-Host $line
}
function Save-ResetJson([string]$Path, $Value) {
    $temporary = "$Path.tmp"
    [IO.File]::WriteAllText($temporary, ($Value | ConvertTo-Json -Depth 8), $utf8)
    Move-Item -Force -LiteralPath $temporary -Destination $Path
}
try {
    if (@(@($InspectOnly, $NewReset, $VerifyOnly, $Follow) | Where-Object { $_ }).Count -gt 1) {
        throw 'Choose only one mode: -InspectOnly, -VerifyOnly, -Follow, or -NewReset.'
    }
    Write-ResetLog 'Automatic ClawStage factory reset. User/account/HA/Matter/Thread data will be erased.'
    Write-ResetLog "Log file: $logPath"
    Write-ResetLog 'Log timezone: Asia/Shanghai (UTC+08:00).'
    $metadata = Get-Content -Raw (Join-Path $PSScriptRoot 'package.json') | ConvertFrom-Json
    foreach ($required in @('payload.tar.gz', 'device-reset.py', 'Find-ClawStage.ps1', 'Reset-Transport.ps1', 'plink.exe', 'pscp.exe')) {
        if (-not $metadata.files.$required) { throw "Incomplete package manifest: $required" }
    }
    foreach ($entry in $metadata.files.PSObject.Properties) {
        $name = $entry.Name
        $path = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot $name))
        $prefix = $PSScriptRoot + [IO.Path]::DirectorySeparatorChar
        if (-not $path.StartsWith($prefix, [StringComparison]::OrdinalIgnoreCase)) { throw "Unsafe manifest path: $name" }
        $actual = (Get-FileHash -Algorithm SHA256 -LiteralPath $path).Hash.ToLowerInvariant()
        if ($actual -ne $entry.Value) { throw "Package checksum mismatch: $name" }
    }
    Write-ResetLog 'Package checksums verified.'
    if ($VerifyOnly) {
        Write-ResetLog 'Tool exit=0. Verification only; no device connection.'
        exit 0
    }
    . (Join-Path $PSScriptRoot 'Reset-Transport.ps1')
    $discoveredName = $null
    if (-not $Device) {
        Write-ResetLog 'Searching active Wi-Fi /24 for ClawStage via mDNS (about 10 seconds)...'
        $candidates = @(& (Join-Path $PSScriptRoot 'Find-ClawStage.ps1') -AsObject)
        $chosen = Select-ClawStageDevice $candidates
        $Device = $chosen.IPv4
        $discoveredName = $chosen.Hostname
    }
    $parsedIP = $null
    if (-not [Net.IPAddress]::TryParse($Device, [ref]$parsedIP) -or $parsedIP.AddressFamily -ne 'InterNetwork') {
        throw 'A valid IPv4 address is required.'
    }
    Write-ResetLog "Selected device: $Device $discoveredName port=$Port user=pi"
    $plink = Join-Path $PSScriptRoot 'plink.exe'
    $pscp = Join-Path $PSScriptRoot 'pscp.exe'
    $target = "pi@$Device"
    $pwFile = [IO.Path]::GetTempFileName()
    [IO.File]::WriteAllText($pwFile, "goodlife`n", [Text.Encoding]::ASCII)
    $connection = @('-batch', '-noagent', '-noshare', '-P', "$Port", '-pwfile', $pwFile)
    $keyPath = Join-Path $PSScriptRoot (".clawstage-key-{0}-{1}.json" -f $(if ($discoveredName) { $discoveredName } else { $Device }), $Port)
    if (Test-Path -LiteralPath $keyPath) {
        $fingerprint = (Get-Content -Raw $keyPath | ConvertFrom-Json).fingerprint
        if ($fingerprint -notmatch '^SHA256:[A-Za-z0-9+/]{43}$') { throw 'Invalid saved SSH host key.' }
    } else {
        # Deliberately reject the first key BEFORE authentication, then pin the
        # advertised fingerprint. Equivalent to trust-on-first-use on this LAN.
        $rejectKey = 'SHA256:' + ('A' * 43)
        $probe = Invoke-ResetTool $plink (@('-v', '-ssh', '-T', '-hostkey', $rejectKey) + $connection + @($target, 'true')) -Quiet
        $keys = @([regex]::Matches(($probe.Lines -join "`n"), 'SHA256:[A-Za-z0-9+/]{43}') |
            ForEach-Object { $_.Value } | Where-Object { $_ -ne $rejectKey } | Select-Object -Unique)
        if ($probe.ExitCode -eq 0 -or $keys.Count -ne 1) { throw ('Cannot obtain one SSH host key: ' + ($probe.Lines -join ' ')) }
        $fingerprint = $keys[0]
        Write-ResetLog "First connection: pinning SSH host key $fingerprint"
    }
    $connection += @('-hostkey', $fingerprint)
    function Invoke-Device([string]$Command, [int]$Timeout = 90) {
        $result = Invoke-ResetTool $plink (@('-ssh', '-T') + $connection + @($target, $Command)) -InputText 'goodlife' -TimeoutSeconds $Timeout
        if ($result.ExitCode -ne 0) { throw "Device command failed (exit $($result.ExitCode))." }
        return $result
    }
    $identity = Invoke-Device 'hostname'
    $names = @($identity.Lines | Where-Object { $_ -match '^ClawStage-[a-zA-Z0-9-]+$' })
    if ($names.Count -ne 1 -or ($discoveredName -and "$($names[0]).local" -ine $discoveredName)) {
        throw 'SSH hostname does not match the discovered ClawStage. No reset started.'
    }
    $deviceMutex = [Threading.Mutex]::new($false, "Local\ClawStageReset-$($names[0])-$Port")
    try { $ownsMutex = $deviceMutex.WaitOne(0) }
    catch [Threading.AbandonedMutexException] { $ownsMutex = $true }
    if (-not $ownsMutex) { throw 'Another reset window is already working with this device. No reset started.' }
    Save-ResetJson $keyPath @{ fingerprint = $fingerprint }
    $sessionPath = Join-Path $PSScriptRoot (".clawstage-session-{0}-{1}.json" -f $names[0], $Port)
    $runId = $null
    if ($Follow) {
        if (-not (Test-Path -LiteralPath $sessionPath)) { throw 'No saved task to follow; no reset started.' }
        $session = Get-Content -Raw $sessionPath | ConvertFrom-Json
        if ($session.fingerprint -ne $fingerprint -or $session.runId -notmatch '^[a-f0-9]{32}$') { throw 'Saved reset session does not match this device.' }
        $runId = $session.runId
        Write-ResetLog "Reconnecting to saved run $runId. No new reset will be started."
    } else {
        if (-not $InspectOnly) { Write-ResetLog 'Starting a NEW full reset; previous saved results will not be reused.' }
        $runId = [guid]::NewGuid().ToString('N')
        $upload = "/home/pi/clawstage-support-upload-$runId"
        $null = Invoke-Device "umask 077; mkdir '$upload'"
        $result = Invoke-ResetTool $pscp ($connection + @('-sftp', (Join-Path $PSScriptRoot 'device-reset.py'), "${target}:$upload/device-reset.py"))
        if ($result.ExitCode -ne 0) { throw 'Worker upload failed.' }
        $null = Invoke-Device "sudo -S -p '' /usr/bin/python3 '$upload/device-reset.py' inspect"
        if ($InspectOnly) {
            Write-ResetLog 'Preflight passed. No reset started.'
            $exitCode = 0
        } else {
            Write-ResetLog 'Preflight passed. Automatically starting the bundled factory reset on the active release.'
            $result = Invoke-ResetTool $pscp ($connection + @('-sftp', (Join-Path $PSScriptRoot 'payload.tar.gz'), "${target}:$upload/payload.tar.gz")) -TimeoutSeconds 180
            if ($result.ExitCode -ne 0) { throw 'Payload upload failed. No reset started.' }
            # Save before launch so -Follow works even when the launch response is lost.
            Save-ResetJson $sessionPath @{ runId = $runId; fingerprint = $fingerprint; device = $Device; hostname = $names[0] }
            Write-ResetLog "Run ID: $runId. Use -Follow to reconnect without resetting again."
            $hash = $metadata.files.'payload.tar.gz'
            $startCommand = "sudo -S -p '' /usr/bin/python3 '$upload/device-reset.py' start --run-id $runId --archive '$upload/payload.tar.gz' --sha256 $hash"
            $started = Invoke-ResetTool $plink (@('-ssh', '-T') + $connection + @($target, $startCommand)) -InputText 'goodlife' -TimeoutSeconds 90
            if ($started.ExitCode -eq 4) {
                $busyRuns = @($started.Lines | Where-Object { $_ -match '^BUSY run_id=([a-f0-9]{32})$' } | ForEach-Object { $_.Substring(12) })
                if ($busyRuns.Count -eq 1) {
                    Save-ResetJson $sessionPath @{ runId = $busyRuns[0]; fingerprint = $fingerprint; device = $Device; hostname = $names[0] }
                }
                throw 'A reset is already running on this device. No new reset was queued. Use -Follow to read it.'
            }
            if ($started.ExitCode -ne 0) { throw "Device start failed (exit=$($started.ExitCode)); use -Follow to check the requested task." }
        }
    }
    if (-not $InspectOnly) {
        $worker = "/var/lib/clawstage-support-reset/$runId/device-reset.py"
        Write-ResetLog "Following durable device log for run $runId..."
        $result = Invoke-ResetTool $plink (@('-ssh', '-T') + $connection + @($target, "sudo -S -p '' /usr/bin/python3 '$worker' follow --run-id $runId")) -InputText 'goodlife' -TimeoutSeconds 2700
        $exitCode = $result.ExitCode
        if ($Follow) { Write-ResetLog "Saved task result exit=$exitCode; NO new reset was performed by this invocation." }
        elseif ($exitCode -eq 0) { Write-ResetLog 'SUCCESS: This new factory reset completed. Cloud account deletion is not guaranteed by local success.' }
        else { Write-ResetLog "FAILED or UNKNOWN (exit=$exitCode). Use -Follow to read this task without resetting again." }
    }
} catch {
    Write-ResetLog "ERROR: $($_.Exception.Message)"
    Write-ResetLog 'No automatic retry. Use -Follow to inspect a saved task; a normal launch requests a new reset.'
    $exitCode = 1
} finally {
    if ($pwFile -and (Test-Path -LiteralPath $pwFile)) { Remove-Item -Force -LiteralPath $pwFile }
    if ($ownsMutex) { $deviceMutex.ReleaseMutex() }
    if ($deviceMutex) { $deviceMutex.Dispose() }
}
Write-ResetLog "Tool exit=$exitCode. Send this .log file to support@hoorii.io if needed."
exit $exitCode
