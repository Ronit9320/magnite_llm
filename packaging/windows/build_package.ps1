param(
    [Parameter(Mandatory=$true)][string]$QtRoot,
    [Parameter(Mandatory=$true)][string]$VcpkgRoot,
    [Parameter(Mandatory=$true)][string]$InnoCompiler
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$repo = (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
Set-Location $repo
function Invoke-Native([string]$File, [string[]]$CommandArgs) {
    & $File @CommandArgs
    if ($LASTEXITCODE -ne 0) { throw "$File failed with exit code $LASTEXITCODE" }
}
if ($env:PROCESSOR_ARCHITECTURE -ne 'AMD64') { throw 'Windows x64 host required' }
if ($env:MAGNITE_REQUIRE_CLEAN -eq '1' -and (git status --porcelain)) { throw 'Clean checkout required' }
$stage = Join-Path $repo 'build/packaging/windows/stage'
$nativeBuild = Join-Path $repo 'build-windows'
if (Test-Path $stage) { Remove-Item -Recurse -Force $stage }
New-Item -ItemType Directory -Force $stage | Out-Null
Invoke-Native 'cmake' @('-S', 'app', '-B', $nativeBuild, '-G', 'Ninja', '-DCMAKE_BUILD_TYPE=Release',
    "-DCMAKE_PREFIX_PATH=$QtRoot", "-DCMAKE_TOOLCHAIN_FILE=$VcpkgRoot/scripts/buildsystems/vcpkg.cmake", '-DVCPKG_TARGET_TRIPLET=x64-windows')
Invoke-Native 'cmake' @('--build', $nativeBuild, '-j', '2')
Copy-Item "$nativeBuild/magnite-app.exe" $stage
Invoke-Native "$QtRoot/bin/windeployqt.exe" @('--release', '--compiler-runtime', '--no-translations', "$stage/magnite-app.exe")
# App-local CRT DLLs make deployment independent of the runner's system-wide
# VC redistributable installation.
$crt = Get-ChildItem (Join-Path $env:VCToolsRedistDir 'x64') -Directory -Filter 'Microsoft.VC*.CRT' | Select-Object -First 1
if (-not $crt) { throw 'MSVC redistributable CRT directory was not found' }
Copy-Item (Join-Path $crt.FullName '*.dll') $stage
# windeployqt deploys Qt; explicitly stage non-Qt OpenSSL dependencies too.
Copy-Item "$VcpkgRoot/installed/x64-windows/bin/*.dll" $stage
New-Item -ItemType Directory -Force "$stage/platforms" | Out-Null
Copy-Item "$QtRoot/plugins/platforms/qoffscreen.dll" "$stage/platforms/"
Invoke-Native 'python' @('packaging/prepare_runtime.py', '--destination', $stage,
    '--requirements', 'packaging/windows/requirements-windows-x86_64.txt')
New-Item -ItemType Directory -Force "$stage/licenses/openssl" | Out-Null
Copy-Item "$VcpkgRoot/installed/x64-windows/share/openssl/copyright" "$stage/licenses/openssl/"
Invoke-Native 'python' @('packaging/windows/qt_notices.py', "$stage/licenses/qt")
$identity = (& python packaging/identity.py | ConvertFrom-Json)
if ($LASTEXITCODE -ne 0) { throw 'Cannot read canonical identity' }
$dist = Join-Path $repo 'dist'
New-Item -ItemType Directory -Force $dist | Out-Null
Invoke-Native $InnoCompiler @("/DStageDir=$stage", "/DOutputDir=$dist",
    "/DReleaseVersion=$($identity.version)", "/DNumericVersion=$($identity.windows_version)",
    'packaging/windows/magnite.iss')
Write-Host "Windows installer built from $($identity.version); source/runtime checks follow before artifact upload."
