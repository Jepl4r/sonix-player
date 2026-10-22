[CmdletBinding()]
param(
    [Parameter(Mandatory = $true, Position = 0)]
    [ValidateSet('tap', 'swipe', 'wake', 'screenshot', 'status')]
    [string]$Command,

    [int]$X,
    [int]$Y,
    [int]$X2,
    [int]$Y2,
    [int]$Duration = 0,
    [string]$Output = ''
)

$ErrorActionPreference = 'Stop'
$fifo = '/tmp/sonix-control'
$statusFile = '/tmp/sonix-control.status'

function Invoke-Adb([string[]]$Arguments) {
    & adb @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "adb failed with exit code $LASTEXITCODE"
    }
}

function Send-Control([string]$Line) {
    & adb shell "test -p '$fifo'"
    if ($LASTEXITCODE -ne 0) {
        throw 'The connected Sonix build does not expose the UI-control FIFO.'
    }

    # Every command argument is parsed as an integer or chosen from the fixed
    # set above, so the single-quoted shell payload contains no user text.
    Invoke-Adb @('shell', "printf '%s\n' '$Line' > '$fifo'")
    Start-Sleep -Milliseconds 180
    Invoke-Adb @('shell', "cat '$statusFile'")
}

switch ($Command) {
    'tap' {
        $line = "tap $X $Y"
        if ($Duration -gt 0) { $line += " $Duration" }
        Send-Control $line
    }
    'swipe' {
        $line = "swipe $X $Y $X2 $Y2"
        if ($Duration -gt 0) { $line += " $Duration" }
        Send-Control $line
    }
    'wake' {
        Send-Control 'wake'
    }
    'status' {
        Send-Control 'status'
    }
    'screenshot' {
        if (-not $Output) {
            $Output = Join-Path (Get-Location) 'sonix-screen.png'
        }
        $Output = [IO.Path]::GetFullPath($Output)
        $raw = [IO.Path]::ChangeExtension($Output, '.rgb565')
        Invoke-Adb @('pull', '/dev/fb0', $raw)

        $ffmpeg = Get-Command ffmpeg -ErrorAction SilentlyContinue
        if ($ffmpeg) {
            & $ffmpeg.Source -hide_banner -loglevel error -f rawvideo -pixel_format rgb565le `
                -video_size 480x800 -i $raw -vframes 1 -y $Output
        } elseif (Get-Command wsl.exe -ErrorAction SilentlyContinue) {
            $linuxRaw = (& wsl.exe -d Ubuntu -- wslpath -a ($raw -replace '\\', '/')).Trim()
            $linuxOutput = (& wsl.exe -d Ubuntu -- wslpath -a ($Output -replace '\\', '/')).Trim()
            & wsl.exe -d Ubuntu -- ffmpeg -hide_banner -loglevel error -f rawvideo `
                -pixel_format rgb565le -video_size 480x800 -i $linuxRaw -vframes 1 -y $linuxOutput
        } else {
            throw 'Screenshot captured, but ffmpeg or WSL is required to convert RGB565 to PNG.'
        }
        if ($LASTEXITCODE -ne 0) {
            throw "ffmpeg failed with exit code $LASTEXITCODE"
        }
        Remove-Item -LiteralPath $raw -Force
        Write-Output $Output
    }
}
