[CmdletBinding()]
param([switch]$AsObject)
# Uses only Windows PowerShell and .NET. No administrator rights required.
# Discovers ClawStage names using mDNS reverse lookups on the active Wi-Fi /24.
$ErrorActionPreference = 'Stop'

function Read-DnsName([byte[]]$Packet, [ref]$Offset) {
    $pos = $Offset.Value
    $labels = @()
    $jumped = $false
    for ($steps = 0; $steps -lt 128; $steps++) {
        if ($pos -ge $Packet.Length) { throw 'Truncated DNS name' }
        $length = [int]$Packet[$pos]
        if ($length -eq 0) {
            if (-not $jumped) { $Offset.Value = $pos + 1 }
            return ($labels -join '.')
        }
        if (($length -band 192) -eq 192) {
            if ($pos + 1 -ge $Packet.Length) { throw 'Truncated DNS pointer' }
            if (-not $jumped) { $Offset.Value = $pos + 2 }
            $pos = (($length -band 63) * 256) + $Packet[$pos + 1]
            $jumped = $true
        } else {
            if ($length -gt 63 -or $pos + $length -ge $Packet.Length) { throw 'Invalid DNS label' }
            $labels += [Text.Encoding]::UTF8.GetString($Packet, $pos + 1, $length)
            $pos += $length + 1
        }
    }
    throw 'DNS pointer loop'
}

function Read-U16([byte[]]$Packet, [int]$Offset) {
    if ($Offset + 1 -ge $Packet.Length) { throw 'Truncated DNS number' }
    return ([int]$Packet[$Offset] * 256 + $Packet[$Offset + 1])
}

$addresses = @(
    foreach ($nic in [Net.NetworkInformation.NetworkInterface]::GetAllNetworkInterfaces()) {
        if ($nic.OperationalStatus -ne 'Up' -or $nic.NetworkInterfaceType -ne 'Wireless80211') { continue }
        foreach ($address in $nic.GetIPProperties().UnicastAddresses) {
            if ($address.Address.AddressFamily -eq 'InterNetwork' -and $address.Address.ToString() -notlike '169.254.*') {
                $address
            }
        }
    }
)
if ($addresses.Count -ne 1) { throw 'Expected one active Wi-Fi IPv4 address. Check your Wi-Fi connection.' }
if ($addresses[0].IPv4Mask.ToString() -ne '255.255.255.0') {
    throw 'This script supports Wi-Fi networks with subnet mask 255.255.255.0 only.'
}
$localIP = $addresses[0].Address
$octets = $localIP.ToString().Split('.')
$prefix = $octets[0..2] -join '.'
$found = @{}
$queries = @{}
$udp = [Net.Sockets.UdpClient]::new([Net.IPEndPoint]::new($localIP, 0))
$udp.Client.SetSocketOption([Net.Sockets.SocketOptionLevel]::IP, [Net.Sockets.SocketOptionName]::MulticastInterface, $localIP.GetAddressBytes())
$udp.Client.ReceiveBufferSize = 262144
$destination = [Net.IPEndPoint]::new([Net.IPAddress]::Parse('224.0.0.251'), 5353)
Write-Verbose "Searching $prefix.0/24 for ClawStage-* via mDNS (about 10 seconds)..."
try {
    for ($round = 0; $round -lt 2; $round++) {
        foreach ($hostNumber in 1..254) {
            $reverse = "$hostNumber.$($octets[2]).$($octets[1]).$($octets[0]).in-addr.arpa"
            $queries[$reverse] = "$prefix.$hostNumber"
            # Transaction ID = host number; one PTR/IN question, legacy unicast reply.
            $packet = [Collections.Generic.List[byte]]::new()
            $packet.AddRange([byte[]]@(0,$hostNumber,0,0,0,1,0,0,0,0,0,0))
            foreach ($label in $reverse.Split('.')) {
                $bytes = [Text.Encoding]::ASCII.GetBytes($label)
                $packet.Add([byte]$bytes.Length)
                $packet.AddRange($bytes)
            }
            $packet.AddRange([byte[]]@(0,0,12,0,1))
            [void]$udp.Send($packet.ToArray(), $packet.Count, $destination)
            Start-Sleep -Milliseconds 5
        }
        $deadline = [DateTime]::UtcNow.AddSeconds(3)
        while ([DateTime]::UtcNow -lt $deadline) {
            if (-not $udp.Client.Poll(100000, [Net.Sockets.SelectMode]::SelectRead)) { continue }
            $sender = [Net.IPEndPoint]::new([Net.IPAddress]::Any, 0)
            $reply = $udp.Receive([ref]$sender)
            if ($sender.Port -ne 5353 -or $reply.Length -lt 12 -or ($reply[2] -band 128) -eq 0) { continue }
            try {
                $offset = 12
                $questionCount = Read-U16 $reply 4
                for ($q = 0; $q -lt $questionCount; $q++) {
                    $null = Read-DnsName $reply ([ref]$offset)
                    $offset += 4
                }
                $recordCount = (Read-U16 $reply 6) + (Read-U16 $reply 8) + (Read-U16 $reply 10)
                for ($r = 0; $r -lt $recordCount; $r++) {
                    $owner = Read-DnsName $reply ([ref]$offset)
                    $type = Read-U16 $reply $offset
                    $length = Read-U16 $reply ($offset + 8)
                    $offset += 10
                    $end = $offset + $length
                    if ($end -gt $reply.Length) { throw 'Truncated DNS record' }
                    if ($type -eq 12 -and $queries.ContainsKey($owner)) {
                        $nameOffset = $offset
                        $name = Read-DnsName $reply ([ref]$nameOffset)
                        if ($name -match '^ClawStage-[^.]+\.local$') {
                            $ip = $queries[$owner]
                            $found["$name|$ip"] = [pscustomobject]@{ Hostname = $name; IPv4 = $ip }
                        }
                    }
                    $offset = $end
                }
            } catch {
                Write-Verbose "Ignored malformed mDNS response: $_"
            }
        }
    }
} finally {
    $udp.Close()
}
if ($found.Count) {
    if ($AsObject) { $found.Values | Sort-Object Hostname,IPv4 }
    else { $found.Values | Sort-Object Hostname,IPv4 | Format-Table -AutoSize }
} else {
    if (-not $AsObject) { Write-Host 'No matching mDNS replies. Check device power, mDNS service, and Wi-Fi client isolation.' }
}
