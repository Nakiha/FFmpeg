param(
    [string]$Generator = 'C:\program1\ffmpeg.exe',
    [string]$OutputDirectory = (Join-Path $PSScriptRoot '..\..\..\results-msys2-baseline')
)
$ErrorActionPreference = 'Stop'
$OutputDirectory = [IO.Path]::GetFullPath($OutputDirectory)
New-Item -ItemType Directory -Force -Path $OutputDirectory | Out-Null
foreach ($codec in @('h264', 'hevc')) {
    $encoder = if ($codec -eq 'h264') { 'libx264' } else { 'libx265' }
    $parameterName = if ($codec -eq 'h264') { '-x264-params' } else { '-x265-params' }
    $parameters = if ($codec -eq 'h264') { 'threads=2:bframes=0:keyint=10:scenecut=0' } else { 'pools=2:frame-threads=2:bframes=0:keyint=10:scenecut=0:log-level=error' }
    $parts = @()
    foreach ($dimensions in @('640x360', '960x540')) {
        $part = Join-Path $OutputDirectory "$codec-$dimensions.$codec"
        & $Generator -hide_banner -loglevel error -nostdin -y -f lavfi -i "testsrc2=size=${dimensions}:rate=25" -frames:v 20 -c:v $encoder -preset ultrafast -pix_fmt yuv420p $parameterName $parameters -an -f $codec $part
        if ($LASTEXITCODE -ne 0) { throw "Generating $part failed" }
        $parts += $part
    }
    $combined = [IO.File]::Create((Join-Path $OutputDirectory "$codec-resize.$codec"))
    try {
        foreach ($part in $parts) {
            $inputFile = [IO.File]::OpenRead($part)
            try { $inputFile.CopyTo($combined) } finally { $inputFile.Dispose() }
        }
    } finally { $combined.Dispose() }
    & $Generator -hide_banner -loglevel error -nostdin -y -f lavfi -i 'testsrc2=size=960x540:rate=25' -frames:v 20 -c:v $encoder -preset ultrafast -pix_fmt yuv420p10le $parameterName $parameters -an (Join-Path $OutputDirectory "$codec-10bit.mp4")
    if ($LASTEXITCODE -ne 0) { throw "Generating $codec 10-bit fixture failed" }
}
& $Generator -hide_banner -loglevel error -nostdin -y -f lavfi -i 'testsrc2=size=960x540:rate=25' -frames:v 50 -c:v libx264 -preset ultrafast -pix_fmt yuv420p -x264-params 'threads=2:bframes=0:keyint=1' -an (Join-Path $OutputDirectory 'h264-intra.mp4')
if ($LASTEXITCODE -ne 0) { throw 'Generating intra fixture failed' }
