<#
.SYNOPSIS
    Periodically moves objects from an S3-compatible bucket to an SMB share using rclone.

.DESCRIPTION
    Downloads and installs a local copy of rclone (if not already present next to this
    script), writes an rclone remote config from the supplied S3 settings, maps the SMB
    destination with the supplied credentials, then runs `rclone move` on a timer.

    All connection settings (S3 + SMB + the sync interval) live in a separate JSON file
    so credentials never need to be hardcoded here. See settings.example.json for the
    expected shape.

.PARAMETER ConfigPath
    Path to the JSON settings file. Required fields:
    sync_period, s3_id, s3_key, s3_backet, s3_endpoint, smb_path, smb_user, smb_pass

.PARAMETER RunOnce
    Perform a single sync pass and exit instead of looping forever. Use this if you'd
    rather drive the schedule from Windows Task Scheduler than leave the script running.

.EXAMPLE
    .\Sync-S3ToSmb.ps1 -ConfigPath C:\config\s3-to-smb.json

.EXAMPLE
    .\Sync-S3ToSmb.ps1 -ConfigPath C:\config\s3-to-smb.json -RunOnce
#>

[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$ConfigPath,

    [switch]$RunOnce
)

$ErrorActionPreference = 'Stop'

$ScriptDir       = Split-Path -Parent $MyInvocation.MyCommand.Path
$RcloneDir       = Join-Path $ScriptDir 'rclone'
$RcloneExe       = Join-Path $RcloneDir 'rclone.exe'
$RcloneConfPath  = Join-Path $ScriptDir 'rclone.conf'
$LogPath         = Join-Path $ScriptDir 'sync.log'
$RcloneRemoteName = 's3remote'

function Write-Log {
    param([string]$Message)
    $line = "[{0}] {1}" -f (Get-Date -Format 'yyyy-MM-dd HH:mm:ss'), $Message
    Write-Host $line
    Add-Content -Path $LogPath -Value $line
}

function Get-Settings {
    if (-not (Test-Path -LiteralPath $ConfigPath)) {
        throw "Config file not found: $ConfigPath"
    }

    $settings = Get-Content -LiteralPath $ConfigPath -Raw | ConvertFrom-Json

    $required = @('sync_period', 's3_id', 's3_key', 's3_backet', 's3_endpoint', 'smb_path', 'smb_user', 'smb_pass')
    foreach ($field in $required) {
        if (-not ($settings.PSObject.Properties.Name -contains $field) -or [string]::IsNullOrEmpty($settings.$field.ToString())) {
            throw "Config file '$ConfigPath' is missing required field '$field'"
        }
    }

    $period = 0
    if (-not [int]::TryParse($settings.sync_period.ToString(), [ref]$period) -or $period -le 0) {
        throw "sync_period must be a positive integer number of seconds (got '$($settings.sync_period)')"
    }

    if ($settings.smb_path -notmatch '^\\\\[^\\]+\\') {
        throw "smb_path must be a UNC path starting with \\server\share (got '$($settings.smb_path)') - " + `
              "a path missing the leading backslashes is treated as a LOCAL relative path by Windows, " + `
              "which would silently move files onto this machine instead of the SMB share."
    }

    return $settings
}

function Install-Rclone {
    if (Test-Path -LiteralPath $RcloneExe) {
        return
    }

    Write-Log 'rclone not found locally, downloading...'

    $zipPath    = Join-Path $ScriptDir 'rclone-download.zip'
    $extractDir = Join-Path $ScriptDir 'rclone-download-tmp'

    Invoke-WebRequest -Uri 'https://downloads.rclone.org/rclone-current-windows-amd64.zip' -OutFile $zipPath -UseBasicParsing

    if (Test-Path -LiteralPath $extractDir) {
        Remove-Item -LiteralPath $extractDir -Recurse -Force
    }
    Expand-Archive -LiteralPath $zipPath -DestinationPath $extractDir -Force

    $exeFound = Get-ChildItem -Path $extractDir -Filter 'rclone.exe' -Recurse | Select-Object -First 1
    if (-not $exeFound) {
        throw 'Downloaded rclone archive did not contain rclone.exe'
    }

    New-Item -ItemType Directory -Force -Path $RcloneDir | Out-Null
    Copy-Item -LiteralPath $exeFound.FullName -Destination $RcloneExe -Force

    Remove-Item -LiteralPath $zipPath -Force
    Remove-Item -LiteralPath $extractDir -Recurse -Force

    Write-Log "rclone installed at $RcloneExe"
}

function Write-RcloneRemoteConfig {
    param($Settings)

    $conf = @"
[$RcloneRemoteName]
type = s3
provider = Other
access_key_id = $($Settings.s3_id)
secret_access_key = $($Settings.s3_key)
endpoint = $($Settings.s3_endpoint)
"@
    Set-Content -LiteralPath $RcloneConfPath -Value $conf -Encoding ASCII

    # rclone.conf holds S3 credentials in plain text - restrict it to the current user.
    icacls $RcloneConfPath /inheritance:r /grant:r "$($env:USERDOMAIN)\$($env:USERNAME):F" | Out-Null
}

function Connect-SmbShare {
    param($Settings)

    if (Test-Path -LiteralPath $Settings.smb_path) {
        # Already reachable - e.g. another instance/loop iteration already holds the
        # session. Windows allows only one credential session per server, so touching
        # New-SmbMapping again here would just collide with our own working connection.
        return
    }

    # Windows allows only one credential session per server at a time - drop *any*
    # existing connection to this server (including the hidden IPC$ admin session
    # Windows creates alongside a share mapping and can leave behind after it's
    # removed) before establishing ours, or New-SmbMapping fails with "Multiple
    # connections... using more than one user name".
    $serverMatch = [regex]::Match($Settings.smb_path, '^\\\\([^\\]+)\\')
    if ($serverMatch.Success) {
        $server = $serverMatch.Groups[1].Value
        Get-SmbMapping -ErrorAction SilentlyContinue | Where-Object { $_.RemotePath -like "\\\\$server\*" } | ForEach-Object {
            Remove-SmbMapping -RemotePath $_.RemotePath -Force -ErrorAction SilentlyContinue
        }
        net use "\\$server\IPC$" /delete /y 2>$null | Out-Null
    }

    $securePass = ConvertTo-SecureString -String $Settings.smb_pass -AsPlainText -Force
    $cred = New-Object System.Management.Automation.PSCredential($Settings.smb_user, $securePass)

    try {
        New-SmbMapping -RemotePath $Settings.smb_path -UserName $cred.UserName -Password $Settings.smb_pass -Persistent $true -ErrorAction Stop | Out-Null
        Write-Log "Mapped SMB share $($Settings.smb_path) as $($Settings.smb_user)"
    } catch {
        if (Test-Path -LiteralPath $Settings.smb_path) {
            Write-Log "New-SmbMapping reported an error ($($_.Exception.Message)) but $($Settings.smb_path) is reachable anyway - continuing"
        } else {
            throw
        }
    }
}

function Invoke-SyncPass {
    param($Settings)

    if (-not (Test-Path -LiteralPath $Settings.smb_path)) {
        Write-Log "SMB path $($Settings.smb_path) not reachable, attempting to (re)connect..."
        Connect-SmbShare -Settings $Settings
    }

    $remote = "${RcloneRemoteName}:$($Settings.s3_backet)"
    Write-Log "Starting sync pass: $remote -> $($Settings.smb_path)"

    & $RcloneExe move $remote $Settings.smb_path `
        --config $RcloneConfPath `
        --log-file $LogPath `
        --log-level INFO `
        --retries 3 `
        --low-level-retries 10

    if ($LASTEXITCODE -eq 0) {
        Write-Log 'Sync pass completed successfully'
    } else {
        Write-Log "Sync pass failed with exit code $LASTEXITCODE"
    }
}

# --- main ---

Install-Rclone

$settings = Get-Settings
Write-RcloneRemoteConfig -Settings $settings
Connect-SmbShare -Settings $settings

if ($RunOnce) {
    Invoke-SyncPass -Settings $settings
    exit 0
}

Write-Log "Entering sync loop, period = $($settings.sync_period)s (Ctrl+C to stop)"
while ($true) {
    try {
        # Re-read settings each pass so credential/period changes take effect without a restart.
        $settings = Get-Settings
        Write-RcloneRemoteConfig -Settings $settings
        Invoke-SyncPass -Settings $settings
    } catch {
        Write-Log "Sync pass errored: $($_.Exception.Message)"
    }

    Start-Sleep -Seconds $settings.sync_period
}
