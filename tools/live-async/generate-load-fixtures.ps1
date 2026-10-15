param(
    [string]$Generator = 'C:\program1\ffmpeg.exe',
    [string]$OutputDirectory = (Join-Path $PSScriptRoot '..\..\..\fixtures-load'),
    [int]$Frames = 192,
    [string[]]$Resolutions = @('1080p', '4k'),
    [string[]]$Codecs = @('h264', 'hevc'),
    [int[]]$BFrames = @(3, 7)
)
$ErrorActionPreference = 'Stop'
$OutputDirectory = [IO.Path]::GetFullPath($OutputDirectory)
New-Item -ItemType Directory -Force -Path $OutputDirectory | Out-Null
foreach ($resolution in $Resolutions) {
    $dimensions = if ($resolution -eq '1080p') { '1920x1080' } else { '3840x2160' }
    foreach ($codec in $Codecs) {
        foreach ($bframes in $BFrames) {
            $stem = "$codec-$resolution-b$bframes"
            $target = Join-Path $OutputDirectory "$stem.mp4"
            if (Test-Path -LiteralPath $target) { continue }
            $encoder = if ($codec -eq 'h264') { 'libx264' } else { 'libx265' }
            $parameterName = if ($codec -eq 'h264') { '-x264-params' } else { '-x265-params' }
            $parameters = if ($codec -eq 'h264') {
                "threads=8:bframes=${bframes}:b-adapt=0:b-pyramid=normal:keyint=1000:min-keyint=1000:scenecut=0"
            } else {
                "pools=8:frame-threads=4:bframes=${bframes}:b-adapt=0:b-pyramid=1:keyint=1000:min-keyint=1000:scenecut=0:rc-lookahead=20:log-level=error"
            }
            # Temporal noise increases residual complexity, with a fixed random seed.
            $inputSpec = "testsrc2=size=${dimensions}:rate=30,noise=alls=10:allf=t+u:all_seed=12345"
            Write-Output "Generating $stem ($Frames frames)"
            & $Generator -hide_banner -loglevel warning -nostdin -y -f lavfi -i $inputSpec `
                -frames:v $Frames -c:v $encoder -preset medium -crf 20 -pix_fmt yuv420p `
                $parameterName $parameters -an -movflags +faststart $target 2> (Join-Path $OutputDirectory "$stem-encoder.log")
            if ($LASTEXITCODE -ne 0) { throw "Generating $stem failed" }
            Get-FileHash -Algorithm SHA256 -LiteralPath $target | Format-List
        }
    }
}
