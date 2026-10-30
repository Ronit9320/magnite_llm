param(
    [string]$VcpkgRoot = $(if ($env:VCPKG_ROOT) { $env:VCPKG_ROOT } else { 'C:/vcpkg' }),
    [string]$Generator = 'Visual Studio 18 2026'
)
$ErrorActionPreference = 'Stop'
Set-Location $PSScriptRoot
function Invoke-Native([string]$File, [string[]]$CommandArgs) {
    & $File @CommandArgs
    if ($LASTEXITCODE -ne 0) { throw "$File failed with exit code $LASTEXITCODE" }
}
$venv = Join-Path $PSScriptRoot '.venv312'
if (-not (Test-Path "$venv/Scripts/python.exe")) {
    Invoke-Native 'py' @('-3.12', '-m', 'venv', $venv)
}
Invoke-Native "$venv/Scripts/python.exe" @('-m', 'pip', 'install', '-e', 'backend[ml]')
Invoke-Native 'cmake' @('-S', 'app', '-B', 'build', '-G', $Generator, '-A', 'x64',
    "-DCMAKE_TOOLCHAIN_FILE=$VcpkgRoot/scripts/buildsystems/vcpkg.cmake")
Invoke-Native 'cmake' @('--build', 'build', '--config', 'Release')
Write-Host 'Run build/Release/magnite-app.exe. For isolated installable packages, see packaging/windows/README.md.'
