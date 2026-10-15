# Requires Visual Studio C++ x64 tools, Windows SDK, CMake, Ninja and Python 3.
# Run ./build.ps1; output is build/bin/bcrypt.dll. No installation is performed.
param([string]$BuildDirectory = (Join-Path $PSScriptRoot 'build'))
$ErrorActionPreference = 'Stop'
$taskSource = [IO.Path]::GetFullPath($PSScriptRoot)
$taskBuild = [IO.Path]::GetFullPath($BuildDirectory)
New-Item -ItemType Directory -Path $taskBuild -Force | Out-Null
$taskVsWhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
if (-not (Test-Path -LiteralPath $taskVsWhere)) { throw 'Visual Studio C++ tools are required.' }
$taskVs = & $taskVsWhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $taskVs) { throw 'Visual Studio C++ x64 toolchain not found.' }
$taskDev = Join-Path $taskVs 'Common7/Tools/VsDevCmd.bat'
$taskScript = Join-Path $taskBuild 'build.cmd'
@"
@echo off
call "$taskDev" -arch=x64 -host_arch=x64
if errorlevel 1 exit /b 1
set _CL_=/Z7
cmake -S "$taskSource" -B "$taskBuild" -G Ninja -DCMAKE_BUILD_TYPE=Release
if errorlevel 1 exit /b 1
cmake --build "$taskBuild" --target parsec_hdr_shim -j 4
"@ | Set-Content -LiteralPath $taskScript -Encoding ASCII
& $env:ComSpec /d /c $taskScript
if ($LASTEXITCODE -ne 0) { throw 'HDR DLL build failed.' }
Write-Output "Built: $taskBuild/bin/bcrypt.dll"
