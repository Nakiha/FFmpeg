param(
    [string]$Configuration = "analysis-minimal",
    [switch]$SkipConfigure,
    [switch]$Clean,
    [string]$MsysBash = "",
    [string]$NasmDir = "",
    [string]$VcVars = ""
)

$ErrorActionPreference = "Stop"

$RepoRoot = Split-Path -Parent $PSScriptRoot
$VendorRoot = Split-Path -Parent $RepoRoot
$ZstdRoot = Join-Path $VendorRoot "zstd"
$BuildDir = Join-Path $RepoRoot "build/windows-msvc-$Configuration"
$ZstdBuildDir = Join-Path $BuildDir "zstd"
$OutputDir = Join-Path $RepoRoot "bin/windows-x64"
function Resolve-FirstExistingPath([string[]]$Candidates) {
    foreach ($candidate in $Candidates) {
        if ($candidate -and (Test-Path $candidate)) {
            return (Resolve-Path $candidate).Path
        }
    }
    return ""
}

function Resolve-CommandPath([string]$Name) {
    $command = Get-Command $Name -ErrorAction SilentlyContinue
    if ($command) {
        return $command.Source
    }
    return ""
}

if (!$MsysBash) {
    $MsysBash = Resolve-FirstExistingPath @(
        "C:\msys64\usr\bin\bash.exe",
        "C:\msys64\ucrt64\bin\bash.exe",
        (Resolve-CommandPath "bash.exe")
    )
}

if (!$NasmDir) {
    $nasmExe = Resolve-FirstExistingPath @(
        (Resolve-CommandPath "nasm.exe"),
        "C:\msys64\ucrt64\bin\nasm.exe",
        "C:\msys64\mingw64\bin\nasm.exe",
        "C:\Program Files\NASM\nasm.exe",
        "$env:LOCALAPPDATA\bin\NASM\nasm.exe"
    )
    if ($nasmExe) {
        $NasmDir = Split-Path -Parent $nasmExe
    }
}

if (!$VcVars) {
    $vsRoots = @(
        "C:\Program Files\Microsoft Visual Studio",
        "C:\Program Files (x86)\Microsoft Visual Studio"
    )
    $vcVarsCandidates = @()
    foreach ($root in $vsRoots) {
        if (Test-Path $root) {
            $vcVarsCandidates += Get-ChildItem -LiteralPath $root -Recurse -Filter vcvars64.bat -ErrorAction SilentlyContinue |
                Sort-Object FullName -Descending |
                Select-Object -ExpandProperty FullName
        }
    }
    $VcVars = Resolve-FirstExistingPath $vcVarsCandidates
}

if (!(Test-Path $MsysBash)) {
    throw "MSYS2 bash not found: $MsysBash"
}
if (!(Test-Path $VcVars)) {
    throw "Visual Studio vcvars64.bat not found: $VcVars"
}
if (!(Test-Path (Join-Path $NasmDir "nasm.exe"))) {
    throw "NASM not found: $NasmDir"
}
if (!(Test-Path (Join-Path $ZstdRoot "build\cmake\CMakeLists.txt"))) {
    throw "zstd source tree not found or incomplete: $ZstdRoot"
}

if ($Clean -and (Test-Path $BuildDir)) {
    Remove-Item -LiteralPath $BuildDir -Recurse -Force
}

New-Item -ItemType Directory -Force -Path $BuildDir | Out-Null
New-Item -ItemType Directory -Force -Path $OutputDir | Out-Null

function Convert-ToMsysPath([string]$Path) {
    $converted = $Path -replace '\\', '/'
    $converted = $converted -replace '^([A-Za-z]):', '/$1'
    return $converted.ToLowerInvariant()
}

$repoRootMsys = Convert-ToMsysPath $RepoRoot
$buildDirMsys = Convert-ToMsysPath $BuildDir
$nasmDirMsys = Convert-ToMsysPath $NasmDir
$zstdIncludeMsys = Convert-ToMsysPath (Join-Path $ZstdRoot "lib")
$zstdLib = Join-Path $ZstdBuildDir "lib\Release\zstd_static.lib"
$zstdLibMsys = Convert-ToMsysPath $zstdLib
$bashFile = Join-Path $BuildDir "build_voidplayer.sh"
$bashFileMsys = Convert-ToMsysPath $bashFile
$cmdFile = Join-Path $BuildDir "build_voidplayer.cmd"

$configureCommand = ""
if (!$SkipConfigure) {
    $configureCommand = @"
"$repoRootMsys/configure" \
  --toolchain=msvc \
  --arch=x86_64 \
  --target-os=win64 \
  --disable-shared \
  --enable-static \
  --disable-programs \
  --disable-doc \
  --disable-avdevice \
  --disable-avfilter \
  --disable-swresample \
  --disable-swscale \
  --disable-everything \
  --extra-cflags="-DVOIDPLAYER_VBS4_ZSTD=1 -I$zstdIncludeMsys" \
  --extra-ldexeflags="$zstdLibMsys" \
  --enable-decoder=hevc,h264,av1,vp9,mpeg2video \
  --enable-parser=hevc,h264,av1,vp9,mpegvideo \
  --enable-demuxer=mov,matroska,flv,hevc,h264,ivf,mpegvideo,mpegts \
  --enable-protocol=file
"@
}

$bashScript = @"
set -euo pipefail
export MSYSTEM=UCRT64
export CHERE_INVOKING=1
export PATH="/usr/bin:${nasmDirMsys}:`$PATH"
command -v cl.exe
command -v make
command -v nasm.exe
cd "$buildDirMsys"
$configureCommand
make -j`$(nproc) tools/void_ffmpeg_analyzer.exe
"@

Set-Content -LiteralPath $bashFile -Value $bashScript -Encoding ASCII

$cmdScript = @"
call "$VcVars"
cmake -S "$($ZstdRoot)\build\cmake" -B "$ZstdBuildDir" -G "Visual Studio 18 2026" -A x64 -DZSTD_BUILD_SHARED=OFF -DZSTD_BUILD_STATIC=ON -DZSTD_BUILD_PROGRAMS=OFF -DZSTD_BUILD_TESTS=OFF -DZSTD_BUILD_CONTRIB=OFF -DZSTD_LEGACY_SUPPORT=OFF -DZSTD_MULTITHREAD_SUPPORT=OFF -DZSTD_USE_STATIC_RUNTIME=ON -DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded
if errorlevel 1 exit /b %errorlevel%
cmake --build "$ZstdBuildDir" --config Release --target libzstd_static
if errorlevel 1 exit /b %errorlevel%
"$MsysBash" "$bashFileMsys"
"@

Set-Content -LiteralPath $cmdFile -Value $cmdScript -Encoding ASCII

cmd.exe /d /s /c "`"$cmdFile`""
if ($LASTEXITCODE -ne 0) {
    throw "FFmpeg analyzer build failed with exit code $LASTEXITCODE"
}

$BuiltExe = Join-Path $BuildDir "tools/void_ffmpeg_analyzer.exe"
if (!(Test-Path $BuiltExe)) {
    throw "Expected analyzer was not produced: $BuiltExe"
}

Copy-Item -LiteralPath $BuiltExe -Destination (Join-Path $OutputDir "void_ffmpeg_analyzer.exe") -Force
Write-Host "Installed analyzer to $(Join-Path $OutputDir 'void_ffmpeg_analyzer.exe')"
