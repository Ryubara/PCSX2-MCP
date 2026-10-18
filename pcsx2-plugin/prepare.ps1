# Prepares a PCSX2 source tree with the DebugServer patch, ready to build on Windows with MSVC.
#
#   powershell -NoProfile -ExecutionPolicy Bypass -File pcsx2-plugin\prepare.ps1 [-Tag v2.9.129] [-Source pcsx2-src]
#
# Then, from an "x64 Native Tools Command Prompt for VS 2022" in the source folder:
#
#   cmake --preset windows-msvc
#   cmake --build --preset windows-msvc
#
# The runnable program lands in <Source>\bin (pcsx2-qt.exe with its DLLs, Qt plugins and resources).
param(
    [string]$Tag = 'v2.9.129',
    [string]$Source = 'pcsx2-src'
)
$ErrorActionPreference = 'Stop'
$here = $PSScriptRoot

# PCSX2's prebuilt Windows dependencies. The release named "latest" is rebuilt from PCSX2's main branch; the one this
# patch was built and checked with has the hash below, and matches the build-dependencies script of $Tag.
$depsUrl = 'https://github.com/PCSX2/pcsx2-windows-dependencies/releases/download/latest-windows-dependencies/pcsx2-windows-dependencies.7z'
$depsSha256 = 'a3133c7841e8fbdab6f4d1d5ad8efec31fb1ecda95ffed39f4d1f36bb7eb0007'
$sevenZip = 'C:\Program Files\7-Zip\7z.exe'

if (Test-Path -LiteralPath $Source) { throw "$Source exists already: remove it, or pass another -Source" }
if (-not (Test-Path -LiteralPath $sevenZip)) { throw "7-Zip is needed at $sevenZip to unpack the dependencies" }

git clone --depth 1 --branch $Tag https://github.com/PCSX2/pcsx2.git $Source
if ($LASTEXITCODE -ne 0) { throw "git clone of PCSX2 $Tag failed" }

Copy-Item -LiteralPath "$here\DebugServer.cpp", "$here\DebugServer.h" -Destination "$Source\pcsx2\DebugTools\"
git -C $Source apply --whitespace=nowarn "$here\pcsx2-integration.patch"
if ($LASTEXITCODE -ne 0) { throw "the integration patch does not apply to PCSX2 $Tag" }
Copy-Item -LiteralPath "$here\CMakeUserPresets.json" -Destination "$Source\"

$archive = Join-Path $Source 'pcsx2-windows-dependencies.7z'
Invoke-WebRequest -Uri $depsUrl -OutFile $archive
$got = (Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash.ToLowerInvariant()
if ($got -ne $depsSha256) {
    Write-Warning "the dependencies differ from the ones this patch was checked with ($got); they may not match $Tag"
}
& $sevenZip x -y "-o$Source" $archive | Out-Null  # the archive holds deps\
if ($LASTEXITCODE -ne 0) { throw 'unpacking the dependencies failed' }
Remove-Item -LiteralPath $archive

Write-Host "Ready: $Source (PCSX2 $Tag with the DebugServer patch). Build it with the two cmake commands above."
