param(
    [string]$Configuration = "analysis-minimal",
    [switch]$SkipConfigure,
    [switch]$Clean
)

$ErrorActionPreference = "Stop"

$RepoRoot = Split-Path -Parent $PSScriptRoot
$BuildDir = Join-Path $RepoRoot "build/windows-msvc-$Configuration"
$OutputDir = Join-Path $RepoRoot "bin/windows-x64"
$MsysBash = "C:\msys64\usr\bin\bash.exe"
$NasmDir = "C:\Users\Nakiha\AppData\Local\bin\NASM"
$VcVars = "C:\Program Files\Microsoft Visual Studio\18\Enterprise\VC\Auxiliary\Build\vcvars64.bat"

if (!(Test-Path $MsysBash)) {
    throw "MSYS2 bash not found: $MsysBash"
}
if (!(Test-Path $VcVars)) {
    throw "Visual Studio vcvars64.bat not found: $VcVars"
}
if (!(Test-Path (Join-Path $NasmDir "nasm.exe"))) {
    throw "NASM not found: $NasmDir"
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
$bashFile = Join-Path $BuildDir "build_voidplayer.sh"
$bashFileMsys = Convert-ToMsysPath $bashFile

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
  --enable-decoder=hevc,h264,av1,vp9,mpeg2video \
  --enable-parser=hevc,h264,av1,vp9,mpegvideo \
  --enable-demuxer=mov,matroska,hevc,h264,ivf,mpegvideo,mpegts \
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

$cmd = "`"$VcVars`" && `"$MsysBash`" `"$bashFileMsys`""

cmd.exe /d /s /c $cmd
if ($LASTEXITCODE -ne 0) {
    throw "FFmpeg analyzer build failed with exit code $LASTEXITCODE"
}

$BuiltExe = Join-Path $BuildDir "tools/void_ffmpeg_analyzer.exe"
if (!(Test-Path $BuiltExe)) {
    throw "Expected analyzer was not produced: $BuiltExe"
}

Copy-Item -LiteralPath $BuiltExe -Destination (Join-Path $OutputDir "void_ffmpeg_analyzer.exe") -Force
Write-Host "Installed analyzer to $(Join-Path $OutputDir 'void_ffmpeg_analyzer.exe')"
