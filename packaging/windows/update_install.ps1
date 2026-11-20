# Installed-app update for Windows, bundled in the client as :/updates/install-windows.ps1.
#
# The client has already verified the Ed25519-signed manifest and the installer's
# SHA-256, and stopped the backend. It starts this script detached, waits for
# "ready", then quits. The per-user Inno Setup installer upgrades the existing
# installation in place (same AppId and folder) and keeps user data, which lives
# in %APPDATA%\Magnite. Inno Setup undoes a failed installation itself; a version
# that installs but does not start is reported, not rolled back.
param(
    [Parameter(Mandatory = $true)][string]$Installer,
    [Parameter(Mandatory = $true)][string]$AppDir,
    [Parameter(Mandatory = $true)][string]$Transaction,
    [Parameter(Mandatory = $true)][int]$ParentPid,
    [Parameter(Mandatory = $true)][string]$Version
)
$ErrorActionPreference = 'Stop'

function Set-Status([string]$Text) {
    $temporary = Join-Path $Transaction 'status.tmp'
    [IO.File]::WriteAllText($temporary, $Text)
    Move-Item -LiteralPath $temporary -Destination (Join-Path $Transaction 'status') -Force
}
function Write-Log([string]$Text) {
    Add-Content -LiteralPath (Join-Path $Transaction 'installer.log') -Value ("{0:o} {1}" -f (Get-Date).ToUniversalTime(), $Text)
}
function Test-Cancelled { Test-Path -LiteralPath (Join-Path $Transaction 'cancel') }
function Quote([string]$Value) { '"' + $Value + '"' }

$app = Join-Path $AppDir 'magnite-app.exe'
try {
    if (-not (Test-Path -LiteralPath $Installer -PathType Leaf) -or -not (Test-Path -LiteralPath $app -PathType Leaf)) {
        Set-Status 'failed: the installer or the installed app is missing'
        exit 2
    }
    Set-Status 'ready'
    Write-Log "waiting for process $ParentPid to exit before installing $Version"
    $deadline = (Get-Date).AddSeconds(120)
    while ((Get-Process -Id $ParentPid -ErrorAction SilentlyContinue) -and (Get-Date) -lt $deadline) {
        if (Test-Cancelled) { exit 3 }
        Start-Sleep -Milliseconds 200
    }
    if (Get-Process -Id $ParentPid -ErrorAction SilentlyContinue) { Set-Status 'cancelled: application did not exit'; exit 3 }
    if (Test-Cancelled) { exit 3 }

    # The bundled Python backend must be gone too, or its files stay locked.
    $prefix = [IO.Path]::GetFullPath($AppDir).TrimEnd('\') + '\'
    $deadline = (Get-Date).AddSeconds(30)
    while ((Get-Date) -lt $deadline) {
        $running = @(Get-Process -ErrorAction SilentlyContinue | Where-Object {
            try { $_.Path -and $_.Path.StartsWith($prefix, [StringComparison]::OrdinalIgnoreCase) } catch { $false }
        })
        if ($running.Count -eq 0) { break }
        Start-Sleep -Milliseconds 500
    }

    Set-Status 'installing'
    $setupLog = Join-Path $Transaction 'setup.log'
    $arguments = @('/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART', '/NOCANCEL', '/CLOSEAPPLICATIONS',
                   ('/DIR=' + (Quote $AppDir)), ('/LOG=' + (Quote $setupLog)))
    $setup = Start-Process -FilePath $Installer -ArgumentList $arguments -Wait -PassThru
    if ($setup.ExitCode -ne 0) {
        Write-Log "installer exited with $($setup.ExitCode)"
        Set-Status "failed: the installer stopped with code $($setup.ExitCode); the previous version was kept"
        Start-Process -FilePath $app | Out-Null
        exit 4
    }
    Set-Status 'installed; checking startup'
    Write-Log "installed $Version, starting it"

    $health = Join-Path $Transaction 'healthy'
    Remove-Item -LiteralPath $health -Force -ErrorAction SilentlyContinue
    $started = Start-Process -FilePath $app -ArgumentList @('--update-health-file', (Quote $health)) -PassThru
    $deadline = (Get-Date).AddSeconds(120)
    while ((Get-Date) -lt $deadline) {
        if ((Test-Path -LiteralPath $health) -and ((Get-Content -LiteralPath $health -Raw) -eq $Version)) {
            Set-Status "success: $Version"
            Write-Log 'new version became ready'
            exit 0
        }
        if ($started.HasExited) { break }
        Start-Sleep -Milliseconds 250
    }
    Set-Status "failed: $Version was installed but did not become ready. Reinstall the previous version from its release page if it keeps failing."
    exit 5
} catch {
    Write-Log $_.Exception.ToString()
    Set-Status ('failed: ' + $_.Exception.Message)
    exit 1
}
