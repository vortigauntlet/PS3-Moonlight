<#
.SYNOPSIS
    Configure a Sunshine / Apollo / Vibepollo host for PS3-Moonlight.

.DESCRIPTION
    The PS3 can only decode 1080p60 when the host encodes with x264 in a mode
    that leaves out CABAC and the deblocking filter.  Hardware encoders
    (NVENC/AMF/QuickSync) always include them, so their 1080p streams are too
    expensive for the PS3 to decode.  This script applies:

      encoder   = software
      sw_preset = veryfast                  (quality per bit)
      sw_tune   = zerolatency,fastdecode    (fastdecode = no CABAC, no deblock)

    How it is applied depends on the host, which the script detects:

      Vibepollo  per-client override for the PS3 only, plus the permissions a
                 new client needs (Launch, View, List, Controller, Mouse,
                 Keyboard).  Optionally -Pacing for Wi-Fi hosts.
      Apollo     permissions per client; the encoder settings are GLOBAL.
      Sunshine   the encoder settings are GLOBAL (every client gets them).

    It talks to the host's web UI API with your web UI username/password
    (HTTP Basic, the same login as the web page).  Nothing else is changed.

.PARAMETER HostAddress
    The host's address.  Default: localhost (run it on the streaming PC).

.PARAMETER Port
    Web UI port.  Default: 47990.

.PARAMETER ClientName
    Name of the paired PS3 as shown in the host's client list.  If omitted
    you are asked to pick one (Vibepollo/Apollo only).

.PARAMETER Preset
    x264 preset.  veryfast (default) is the quality sweet spot; use faster
    CPU budgets with 'superfast' or 'ultrafast' if the host drops frames.

.PARAMETER Pacing
    Vibepollo only: set pacing_max_bitrate_kbps (global).  Spreads each frame
    over its frame interval instead of sending it at ~800 Mbps, which helps a
    host on Wi-Fi and the PS3's small network buffer.  0 = leave unchanged.
    Suggested: 60000.

.PARAMETER Revert
    Remove what this script sets (Vibepollo: the PS3's overrides and the
    pacing key; Sunshine/Apollo: the three global encoder keys).
    Permissions are left as they are.

.PARAMETER DryRun
    Show what would change and change nothing.

.PARAMETER Force
    Do not ask before changing a GLOBAL setting.

.EXAMPLE
    .\setup-host.ps1
.EXAMPLE
    .\setup-host.ps1 -ClientName PS3 -Pacing 60000
.EXAMPLE
    .\setup-host.ps1 -HostAddress 192.168.0.194 -DryRun
#>
[CmdletBinding()]
param(
    [string]$HostAddress = 'localhost',
    [int]$Port = 47990,
    [string]$ClientName,
    [ValidateSet('ultrafast', 'superfast', 'veryfast', 'faster', 'fast')]
    [string]$Preset = 'veryfast',
    [int]$Pacing = 0,
    [switch]$Revert,
    [switch]$DryRun,
    [switch]$Force,
    [pscredential]$Credential
)

$ErrorActionPreference = 'Stop'

$EncoderSettings = [ordered]@{
    encoder   = 'software'
    sw_preset = $Preset
    sw_tune   = 'zerolatency,fastdecode'
}

# Apollo/Vibepollo permission bits (Apollo src/crypto.h).
$PERM_CONTROLLER = 0x00000100
$PERM_MOUSE      = 0x00000800
$PERM_KEYBOARD   = 0x00001000
$PERM_LIST       = 0x01000000
$PERM_VIEW       = 0x02000000
$PERM_LAUNCH     = 0x04000000
$PermNeeded = $PERM_CONTROLLER -bor $PERM_MOUSE -bor $PERM_KEYBOARD -bor $PERM_LIST -bor $PERM_VIEW -bor $PERM_LAUNCH

# --------------------------------------------------------------------------
# HTTP helpers.  The web UI uses a self-signed certificate; trust it for this
# host only, for the lifetime of this script.
# --------------------------------------------------------------------------
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
$base = "https://${HostAddress}:${Port}"
$trustedHost = $HostAddress
if (-not ('PS3MoonlightCertTrust' -as [type])) {
    Add-Type @"
using System.Net;
using System.Net.Security;
using System.Security.Cryptography.X509Certificates;
public static class PS3MoonlightCertTrust {
    public static string Host;
    public static bool Check(object sender, X509Certificate cert, X509Chain chain, SslPolicyErrors errors) {
        if (errors == SslPolicyErrors.None) return true;
        var req = sender as HttpWebRequest;
        return req != null && req.RequestUri.Host == Host;
    }
    // Installed from C#: Windows PowerShell 5.1 cannot convert a static
    // method to RemoteCertificateValidationCallback by assignment.
    public static void Install(string host) {
        Host = host;
        ServicePointManager.ServerCertificateValidationCallback = Check;
    }
}
"@
}
[PS3MoonlightCertTrust]::Install($trustedHost)

if (-not $Credential) {
    $Credential = Get-Credential -Message "Web UI login for $base"
}
$pair = '{0}:{1}' -f $Credential.UserName, $Credential.GetNetworkCredential().Password
$authHeader = 'Basic ' + [Convert]::ToBase64String([Text.Encoding]::UTF8.GetBytes($pair))

function Invoke-Host([string]$Method, [string]$Path, $Body) {
    $params = @{
        Uri         = "$base$Path"
        Method      = $Method
        Headers     = @{ Authorization = $authHeader }
        ContentType = 'application/json'
    }
    if ($null -ne $Body) {
        $params.Body = ($Body | ConvertTo-Json -Depth 10 -Compress)
    }
    try {
        return Invoke-RestMethod @params
    } catch {
        $code = $null
        if ($_.Exception.Response) { $code = [int]$_.Exception.Response.StatusCode }
        if ($code -eq 401) { throw "The host rejected the login (401). Check the web UI username/password." }
        if ($code -eq 404) { return $null }
        throw
    }
}

function Confirm-Global([string]$What) {
    if ($Force -or $DryRun) { return $true }
    Write-Host ""
    Write-Warning "$What is a GLOBAL setting on this host: every client will use it, not just the PS3."
    $a = Read-Host "Apply it anyway? (y/N)"
    return ($a -match '^(y|yes)$')
}

function Show-Plan([string]$Text) {
    if ($DryRun) { Write-Host "  [dry run] $Text" -ForegroundColor Yellow }
    else { Write-Host "  $Text" -ForegroundColor Green }
}

# --------------------------------------------------------------------------
# Detect the host family
# --------------------------------------------------------------------------
Write-Host "Connecting to $base ..."
$clients = Invoke-Host GET '/api/clients/list' $null
$records = @()
if ($clients -and $clients.named_certs) { $records = @($clients.named_certs) }

$hasPerm      = @($records | Where-Object { $_.PSObject.Properties.Name -contains 'perm' }).Count -gt 0
$hasOverrides = @($records | Where-Object { $_.PSObject.Properties.Name -contains 'config_overrides' }).Count -gt 0
$config       = Invoke-Host GET '/api/config' $null
$hasPacingKey = $false
if ($config) { $hasPacingKey = $config.PSObject.Properties.Name -contains 'pacing_max_bitrate_kbps' }

if ($hasOverrides)  { $family = 'Vibepollo' }
elseif ($hasPerm)   { $family = 'Apollo' }
else                { $family = 'Sunshine' }
Write-Host "Host type: $family"

# --------------------------------------------------------------------------
# Pick the PS3 client (Apollo family: permissions and overrides are per client)
# --------------------------------------------------------------------------
$client = $null
if ($family -ne 'Sunshine') {
    if ($records.Count -eq 0) {
        throw "No paired clients. Pair the PS3 first (Moonlight PS3 > Connect / Pair), then run this again."
    }
    if ($ClientName) {
        $client = $records | Where-Object { $_.name -eq $ClientName } | Select-Object -First 1
        if (-not $client) { throw "No paired client named '$ClientName'. Paired: $(($records | ForEach-Object name) -join ', ')" }
    } elseif ($records.Count -eq 1) {
        $client = $records[0]
    } else {
        Write-Host "Paired clients:"
        for ($i = 0; $i -lt $records.Count; $i++) { Write-Host ("  [{0}] {1}" -f ($i + 1), $records[$i].name) }
        $pick = [int](Read-Host "Which one is the PS3?") - 1
        if ($pick -lt 0 -or $pick -ge $records.Count) { throw "No such client." }
        $client = $records[$pick]
    }
    Write-Host "PS3 client: $($client.name)"
}

function Update-Client($Rec, [hashtable]$Changes) {
    $body = [ordered]@{}
    foreach ($p in $Rec.PSObject.Properties) {
        if ($p.Name -in @('connected', 'last_seen')) { continue }
        $body[$p.Name] = $p.Value
    }
    foreach ($k in $Changes.Keys) { $body[$k] = $Changes[$k] }
    if ($DryRun) { return }
    $r = Invoke-Host POST '/api/clients/update' $body
    if (-not $r -or -not $r.status) { throw "The host refused the client update." }
}

# --------------------------------------------------------------------------
# Apply / revert
# --------------------------------------------------------------------------
Write-Host ""
Write-Host ($(if ($Revert) { 'Reverting:' } else { 'Applying:' }))

if ($family -eq 'Vibepollo') {
    $ov = [ordered]@{}
    if ($client.config_overrides) {
        foreach ($p in $client.config_overrides.PSObject.Properties) { $ov[$p.Name] = $p.Value }
    }
    $perm = [int64]$client.perm
    if ($Revert) {
        foreach ($k in $EncoderSettings.Keys) { $ov.Remove($k) }
        Show-Plan "remove encoder overrides from '$($client.name)'"
    } else {
        foreach ($k in $EncoderSettings.Keys) { $ov[$k] = $EncoderSettings[$k] }
        Show-Plan ("'$($client.name)' overrides: " + (($EncoderSettings.GetEnumerator() | ForEach-Object { "$($_.Key)=$($_.Value)" }) -join ', '))
        if (($perm -band $PermNeeded) -ne $PermNeeded) {
            $perm = $perm -bor $PermNeeded
            Show-Plan "'$($client.name)' permissions: + Launch, View, List, Controller, Mouse, Keyboard"
        }
    }
    Update-Client $client @{ config_overrides = $ov; perm = $perm }

    if ($Revert) {
        # A null value makes Vibepollo delete the key; harmless if it is not set.
        Show-Plan "remove pacing_max_bitrate_kbps (if set)"
        if (-not $DryRun) { Invoke-Host PATCH '/api/config' @{ pacing_max_bitrate_kbps = $null } | Out-Null }
    } elseif ($Pacing -gt 0) {
        if (Confirm-Global "pacing_max_bitrate_kbps") {
            Show-Plan "pacing_max_bitrate_kbps = $Pacing (never below 110% of a session's own bitrate)"
            if (-not $DryRun) { Invoke-Host PATCH '/api/config' @{ pacing_max_bitrate_kbps = "$Pacing" } | Out-Null }
        }
    }
} else {
    if ($family -eq 'Apollo' -and -not $Revert) {
        $perm = [int64]$client.perm
        if (($perm -band $PermNeeded) -ne $PermNeeded) {
            Show-Plan "'$($client.name)' permissions: + Launch, View, List, Controller, Mouse, Keyboard"
            Update-Client $client @{ perm = ($perm -bor $PermNeeded) }
        }
    }
    if ($Pacing -gt 0) {
        Write-Host "  (skipping -Pacing: $family has no pacing setting; its send rate is fixed)"
    }
    if (-not $config) { throw "Could not read the host configuration." }
    $merged = [ordered]@{}
    foreach ($p in $config.PSObject.Properties) {
        # Status fields the GET adds; they are not configuration.
        if ($p.Name -in @('status', 'platform', 'version', 'restart_required')) { continue }
        $merged[$p.Name] = $p.Value
    }
    if ($Revert) {
        foreach ($k in $EncoderSettings.Keys) { $merged.Remove($k) }
        Show-Plan "remove global encoder, sw_preset, sw_tune"
        $go = $true
    } else {
        foreach ($k in $EncoderSettings.Keys) { $merged[$k] = $EncoderSettings[$k] }
        $go = Confirm-Global "encoder/sw_preset/sw_tune"
        if ($go) { Show-Plan ("global: " + (($EncoderSettings.GetEnumerator() | ForEach-Object { "$($_.Key)=$($_.Value)" }) -join ', ')) }
    }
    if ($go -and -not $DryRun) {
        # Sunshine's POST /api/config replaces the whole file, so the full
        # merged configuration is sent back, not just the changed keys.
        $r = Invoke-Host POST '/api/config' $merged
        if ($r -and $r.PSObject.Properties.Name -contains 'status' -and -not $r.status) { throw "The host refused the configuration." }
        Write-Host "  Restart the host (or its service) for the encoder change to take effect."
    }
}

Write-Host ""
if ($DryRun) { Write-Host "Dry run: nothing was changed." }
else { Write-Host "Done. Start a new stream from the PS3 for the settings to apply." }
