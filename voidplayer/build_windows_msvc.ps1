param(
    [string]$Configuration = "analysis-minimal",
    [string]$MsysBash = $env:MSYS2_BASH,
    [string]$NasmPath = $env:VOID_NASM,
    [string]$NasmDir = $env:VOID_NASM_DIR,
    [switch]$SkipConfigure,
    [switch]$Clean,
    [string]$VcVars = "",
    [string]$CMakeGenerator = $env:CMAKE_GENERATOR,
    [string]$CMakePlatform = "x64"
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

if ([string]::IsNullOrWhiteSpace($MsysBash)) {
    $MsysBash = Resolve-FirstExistingPath @(
        "C:\msys64\usr\bin\bash.exe",
        "C:\msys64\ucrt64\bin\bash.exe",
        (Resolve-CommandPath "bash.exe")
    )
}

function Resolve-NasmExe {
    if (![string]::IsNullOrWhiteSpace($NasmPath)) {
        if (Test-Path $NasmPath) {
            return (Resolve-Path $NasmPath).Path
        }
        throw "NASM not found at VOID_NASM/NasmPath: $NasmPath"
    }

    if (![string]::IsNullOrWhiteSpace($NasmDir)) {
        $candidate = Join-Path $NasmDir "nasm.exe"
        if (Test-Path $candidate) {
            return (Resolve-Path $candidate).Path
        }
        throw "NASM not found at VOID_NASM_DIR/NasmDir: $candidate"
    }

    $nasmExe = Resolve-FirstExistingPath @(
        (Resolve-CommandPath "nasm.exe"),
        "C:\Program Files\NASM\nasm.exe",
        "C:\Program Files (x86)\NASM\nasm.exe",
        "C:\msys64\usr\bin\nasm.exe",
        "C:\msys64\mingw64\bin\nasm.exe",
        "C:\msys64\ucrt64\bin\nasm.exe",
        "$env:LOCALAPPDATA\bin\NASM\nasm.exe"
    )
    if ($nasmExe) {
        return $nasmExe
    }

    throw "NASM not found. Install NASM and add nasm.exe to PATH, or set VOID_NASM to nasm.exe / VOID_NASM_DIR to its directory."
}

if ([string]::IsNullOrWhiteSpace($VcVars)) {
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

$NasmExe = Resolve-NasmExe
$NasmDir = Split-Path -Parent $NasmExe

if (!(Test-Path $MsysBash)) {
    throw "MSYS2 bash not found: $MsysBash"
}
if (!(Test-Path $VcVars)) {
    throw "Visual Studio vcvars64.bat not found: $VcVars"
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
  --extra-cflags="-DVOIDPLAYER_VACHUNK_ZSTD=1 -I$zstdIncludeMsys" \
  --extra-ldexeflags="$zstdLibMsys" \
  --enable-decoder=vvc,hevc,h264,av1,vp9,mpeg2video \
  --enable-parser=vvc,hevc,h264,av1,vp9,mpegvideo \
  --enable-demuxer=mov,matroska,flv,vvc,hevc,h264,ivf,mpegvideo,mpegts \
  --enable-bsf=vvc_mp4toannexb,hevc_mp4toannexb,h264_mp4toannexb \
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
command -v nasm.exe || command -v nasm
cd "$buildDirMsys"
$configureCommand
# FFmpeg generates component registries during configure. Some MSVC dependency
# files are empty here, so config changes can leave stale registry objects in
# incremental builds.
rm -f \
      libavformat/allformats.o libavformat/allformats.d \
      libavcodec/allcodecs.o libavcodec/allcodecs.d \
      libavcodec/bitstream_filters.o libavcodec/bitstream_filters.d \
      libavcodec/cbs.o libavcodec/cbs.d \
      libavcodec/codec_list.o libavcodec/codec_list.d \
      libavcodec/parser_list.o libavcodec/parser_list.d \
      libavcodec/bsf_list.o libavcodec/bsf_list.d
make -j`$(nproc) tools/void_ffmpeg_analyzer.exe
"@

Set-Content -LiteralPath $bashFile -Value $bashScript -Encoding ASCII

$zstdConfigureArgs = @(
    '-S', "`"$($ZstdRoot)\build\cmake`"",
    '-B', "`"$ZstdBuildDir`""
)
if (![string]::IsNullOrWhiteSpace($CMakeGenerator)) {
    $zstdConfigureArgs += @('-G', "`"$CMakeGenerator`"")
}
if (![string]::IsNullOrWhiteSpace($CMakePlatform)) {
    $zstdConfigureArgs += @('-A', $CMakePlatform)
}
$zstdConfigureArgs += @(
    '-DZSTD_BUILD_SHARED=OFF',
    '-DZSTD_BUILD_STATIC=ON',
    '-DZSTD_BUILD_PROGRAMS=OFF',
    '-DZSTD_BUILD_TESTS=OFF',
    '-DZSTD_BUILD_CONTRIB=OFF',
    '-DZSTD_LEGACY_SUPPORT=OFF',
    '-DZSTD_MULTITHREAD_SUPPORT=OFF',
    '-DZSTD_USE_STATIC_RUNTIME=ON',
    '-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded'
)
$zstdConfigureCommand = "cmake " + ($zstdConfigureArgs -join " ")

$cmdScript = @"
call "$VcVars"
$zstdConfigureCommand
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
