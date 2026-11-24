# Windows PowerShell 5.1 compatible. Native argv quoting, bounded execution,
# concurrent stdout/stderr drain and automatic password input without a PTY.
function ConvertTo-NativeArgument([string]$Value) {
    '"' + [regex]::Replace([regex]::Replace($Value, '(\\*)"', '$1$1\"'), '(\\+)$', '$1$1') + '"'
}

function Invoke-ResetTool {
    param([string]$File, [string[]]$Arguments, [string]$InputText = '',
          [int]$TimeoutSeconds = 60, [switch]$Quiet)
    $process = New-Object System.Diagnostics.Process
    $process.StartInfo.FileName = $File
    $process.StartInfo.Arguments = (($Arguments | ForEach-Object { ConvertTo-NativeArgument $_ }) -join ' ')
    $process.StartInfo.UseShellExecute = $false
    $process.StartInfo.CreateNoWindow = $true
    $process.StartInfo.RedirectStandardOutput = $true
    $process.StartInfo.RedirectStandardError = $true
    $process.StartInfo.RedirectStandardInput = $true
    $process.StartInfo.StandardOutputEncoding = [Text.Encoding]::UTF8
    $process.StartInfo.StandardErrorEncoding = [Text.Encoding]::UTF8
    $lines = New-Object 'System.Collections.Generic.List[string]'
    $started = $false
    try {
        $started = $process.Start()
        # sudo -S reads a POSIX line: Windows WriteLine would append CRLF,
        # making the CR part of the password when plink runs without a PTY.
        if ($InputText) { $process.StandardInput.Write($InputText + "`n") }
        $process.StandardInput.Close()
        $reads = @($process.StandardOutput.ReadLineAsync(), $process.StandardError.ReadLineAsync())
        $streams = @($process.StandardOutput, $process.StandardError)
        $watch = [Diagnostics.Stopwatch]::StartNew()
        while ($null -ne $reads[0] -or $null -ne $reads[1] -or -not $process.HasExited) {
            if ($watch.Elapsed.TotalSeconds -gt $TimeoutSeconds) { throw "Connection/command timed out after $TimeoutSeconds seconds. Completion is UNKNOWN." }
            for ($index = 0; $index -lt 2; $index++) {
                if ($null -ne $reads[$index] -and $reads[$index].IsCompleted) {
                    $line = $reads[$index].GetAwaiter().GetResult()
                    if ($null -eq $line) { $reads[$index] = $null }
                    else {
                        # Do not echo credentials even if a remote error includes them.
                        $line = $line.Replace('goodlife', '[REDACTED]')
                        $lines.Add($line)
                        if (-not $Quiet) { Write-ResetLog $line }
                        $reads[$index] = $streams[$index].ReadLineAsync()
                    }
                }
            }
            Start-Sleep -Milliseconds 10
        }
        $process.WaitForExit()
        return [pscustomobject]@{ ExitCode = $process.ExitCode; Lines = $lines.ToArray() }
    } finally {
        if ($started -and -not $process.HasExited) { $process.Kill(); $process.WaitForExit() }
        $process.Dispose()
    }
}

function Select-ClawStageDevice([object[]]$Candidates) {
    # A host may answer via more than one address; never silently pick among them.
    $unique = @($Candidates | Sort-Object Hostname,IPv4 -Unique)
    if ($unique.Count -eq 0) { throw 'No ClawStage found. Connect PC and device to the same Wi-Fi; check power and Wi-Fi client isolation.' }
    foreach ($item in $unique) {
        if ($item.Hostname -notmatch '^ClawStage-[a-zA-Z0-9-]+\.local$' -or
            $item.IPv4 -notmatch '^\d{1,3}(\.\d{1,3}){3}$') { throw 'Invalid discovery response.' }
    }
    if ($unique.Count -eq 1) { return $unique[0] }
    Write-ResetLog 'Multiple devices found. Select exactly ONE device to erase:'
    for ($i = 0; $i -lt $unique.Count; $i++) {
        Write-ResetLog ("{0}: {1}  {2}" -f ($i + 1), $unique[$i].Hostname, $unique[$i].IPv4)
    }
    $selection = Read-Host 'Device number (or close this window to cancel)'
    $number = 0
    if (-not [int]::TryParse($selection, [ref]$number) -or $number -lt 1 -or $number -gt $unique.Count) {
        throw 'Invalid selection. No reset started.'
    }
    return $unique[$number - 1]
}
