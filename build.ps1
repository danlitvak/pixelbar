# build.ps1 - compile pixelbar with MSVC (VS Build Tools); no CMake needed.
#   .\build.ps1          release build into bin\
#   .\build.ps1 -Run     build, then start it (replaces the running copy)
#   .\build.ps1 -Dbg     unoptimised build with symbols
# Keep this file ASCII: Windows PowerShell 5.1 reads unmarked scripts as ANSI.
param([switch]$Run, [switch]$Dbg)

$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$vcvars = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
    -find 'VC\Auxiliary\Build\vcvars64.bat' | Select-Object -First 1
if (-not $vcvars) { throw 'MSVC not found (vcvars64.bat)' }
# vcvars64.bat calls vswhere itself, and the Installer folder is not on PATH here.
$env:PATH = (Split-Path $vswhere) + ';' + $env:PATH

$bin = Join-Path $root 'bin'
$obj = Join-Path $root 'obj'
New-Item -ItemType Directory -Force -Path $bin, $obj | Out-Null
$exe = Join-Path $bin 'pixelbar.exe'

# The linker cannot overwrite a running exe, so close the live copy first.
if (Test-Path $exe) { Start-Process $exe -ArgumentList '--quit' -Wait }

$opt = if ($Dbg) { '/Od /Zi /MTd' } else { '/O2 /MT /DNDEBUG' }
$cl = "cl /nologo /std:c++20 /EHsc /permissive- /utf-8 /W3 /MP $opt /DUNICODE /D_UNICODE " +
      "/Fo`"$obj\\`" /Fd`"$obj\\`" /Fe`"$exe`" src\*.cpp /link /SUBSYSTEM:WINDOWS"
cmd /c "`"$vcvars`" >nul && cd /d `"$root`" && $cl"
if ($LASTEXITCODE -ne 0) { throw "build failed ($LASTEXITCODE)" }
Write-Output "built $exe"

# Start it through Explorer: a copy launched from an agent shell lives inside the Claude
# app's container and closes whenever the app restarts.
if ($Run) {
    $session = (Get-Process -Id $PID).SessionId
    if (@(Get-Process -Name explorer -ErrorAction SilentlyContinue | Where-Object { $_.SessionId -eq $session }).Count) {
        Start-Process -FilePath (Join-Path $env:WINDIR 'explorer.exe') -ArgumentList ('"' + $exe + '"')
    } else {
        Start-Process $exe
    }
}
