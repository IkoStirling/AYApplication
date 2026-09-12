[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$VisualExe,

    [string]$BaselineDir = "",

    [string]$OutputDir = (Join-Path $env:TEMP "AYUI-production-vertical-golden"),

    [ValidateRange(0, 255)]
    [int]$PixelTolerance = 2,

    [ValidateRange(0.0, 100.0)]
    [double]$MaxChangedPixelPercent = 0.05,

    [switch]$UpdateBaselines
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"
$scenarioNames = @("vertical_boot", "vertical_gameplay", "vertical_parallel_modal")

if ([string]::IsNullOrWhiteSpace($BaselineDir)) {
    $scriptDirectory = Split-Path -Parent $PSCommandPath
    $BaselineDir = Join-Path $scriptDirectory "golden/windows-d3d11"
}

function Read-UncompressedTga([string]$Path) {
    $bytes = [System.IO.File]::ReadAllBytes($Path)
    if ($bytes.Length -lt 18 -or $bytes[1] -ne 0 -or $bytes[2] -ne 2) {
        throw "Unsupported or truncated TGA: $Path"
    }
    $width = [BitConverter]::ToUInt16($bytes, 12)
    $height = [BitConverter]::ToUInt16($bytes, 14)
    $bitsPerPixel = [int]$bytes[16]
    if ($width -le 0 -or $height -le 0 -or
        ($bitsPerPixel -ne 24 -and $bitsPerPixel -ne 32)) {
        throw "Unsupported TGA dimensions or pixel format: $Path"
    }
    $channels = [int]($bitsPerPixel / 8)
    $offset = 18 + [int]$bytes[0]
    $expected = $offset + ([int64]$width * [int64]$height * $channels)
    if ($bytes.Length -lt $expected) {
        throw "TGA pixel payload is truncated: $Path"
    }
    return [pscustomobject]@{
        Bytes = $bytes
        Width = [int]$width
        Height = [int]$height
        Channels = $channels
        Offset = $offset
    }
}

function Write-PngPreview([string]$TgaPath, [string]$PngPath) {
    Add-Type -AssemblyName System.Drawing
    $image = Read-UncompressedTga $TgaPath
    $bitmap = [System.Drawing.Bitmap]::new(
        $image.Width, $image.Height,
        [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $bounds = [System.Drawing.Rectangle]::new(
        0, 0, $image.Width, $image.Height)
    $data = $bitmap.LockBits(
        $bounds,
        [System.Drawing.Imaging.ImageLockMode]::WriteOnly,
        [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    try {
        $pixels = [byte[]]::new($data.Stride * $image.Height)
        $topOrigin = ($image.Bytes[17] -band 0x20) -ne 0
        for ($sourceY = 0; $sourceY -lt $image.Height; ++$sourceY) {
            $targetY = if ($topOrigin) {
                $sourceY
            } else {
                $image.Height - 1 - $sourceY
            }
            for ($x = 0; $x -lt $image.Width; ++$x) {
                $source = $image.Offset +
                    (($sourceY * $image.Width + $x) * $image.Channels)
                $target = ($targetY * $data.Stride) + ($x * 4)
                $pixels[$target] = $image.Bytes[$source]
                $pixels[$target + 1] = $image.Bytes[$source + 1]
                $pixels[$target + 2] = $image.Bytes[$source + 2]
                $pixels[$target + 3] = if ($image.Channels -eq 4) {
                    $image.Bytes[$source + 3]
                } else {
                    255
                }
            }
        }
        [Runtime.InteropServices.Marshal]::Copy(
            $pixels, 0, $data.Scan0, $pixels.Length)
    }
    finally {
        $bitmap.UnlockBits($data)
    }
    try {
        $bitmap.Save($PngPath, [System.Drawing.Imaging.ImageFormat]::Png)
    }
    finally {
        $bitmap.Dispose()
    }
}

function Compare-Tga(
    [string]$ActualPath,
    [string]$ExpectedPath,
    [int]$Tolerance,
    [double]$AllowedPercent
) {
    $actual = Read-UncompressedTga $ActualPath
    $expected = Read-UncompressedTga $ExpectedPath
    if ($actual.Width -ne $expected.Width -or
        $actual.Height -ne $expected.Height -or
        $actual.Channels -ne $expected.Channels) {
        throw "Golden dimensions differ for $ActualPath"
    }

    $pixelCount = [int64]$actual.Width * [int64]$actual.Height
    $changed = [int64]0
    $maxDelta = 0
    for ([int64]$pixel = 0; $pixel -lt $pixelCount; ++$pixel) {
        $pixelChanged = $false
        for ($channel = 0; $channel -lt $actual.Channels; ++$channel) {
            $actualIndex = $actual.Offset + ($pixel * $actual.Channels) + $channel
            $expectedIndex = $expected.Offset + ($pixel * $expected.Channels) + $channel
            $delta = [Math]::Abs(
                [int]$actual.Bytes[$actualIndex] - [int]$expected.Bytes[$expectedIndex])
            if ($delta -gt $maxDelta) { $maxDelta = $delta }
            if ($delta -gt $Tolerance) { $pixelChanged = $true }
        }
        if ($pixelChanged) { ++$changed }
    }
    $changedPercent = if ($pixelCount -eq 0) {
        0.0
    } else {
        100.0 * [double]$changed / [double]$pixelCount
    }
    if ($changedPercent -gt $AllowedPercent) {
        throw ("Vertical-slice golden mismatch: {0:N4}% pixels changed " +
               "(allowed {1:N4}%, max channel delta {2})" -f
               $changedPercent, $AllowedPercent, $maxDelta)
    }
    return [pscustomobject]@{
        ChangedPercent = $changedPercent
        MaxChannelDelta = $maxDelta
        Width = $actual.Width
        Height = $actual.Height
    }
}

$visualPath = (Resolve-Path -LiteralPath $VisualExe).Path
$visualDirectory = Split-Path -Parent $visualPath
$OutputDir = [System.IO.Path]::GetFullPath($OutputDir)
$BaselineDir = [System.IO.Path]::GetFullPath($BaselineDir)
New-Item -ItemType Directory -Force -Path $OutputDir | Out-Null
New-Item -ItemType Directory -Force -Path $BaselineDir | Out-Null

foreach ($name in $scenarioNames) {
    foreach ($extension in @(".tga", ".png", ".metrics.txt")) {
        Remove-Item -LiteralPath (Join-Path $OutputDir "$name$extension") `
            -ErrorAction SilentlyContinue
    }
}

try {
    $env:AY_UI_VERTICAL_CAPTURE_DIR = $OutputDir
    $process = Start-Process -FilePath $visualPath `
        -WorkingDirectory $visualDirectory `
        -Wait -PassThru -WindowStyle Hidden
    if ($process.ExitCode -ne 0) {
        throw "UIProductionVerticalSliceVisual exited with code $($process.ExitCode)"
    }
}
finally {
    Remove-Item Env:\AY_UI_VERTICAL_CAPTURE_DIR -ErrorAction SilentlyContinue
}

$results = @()
foreach ($name in $scenarioNames) {
    $actualTga = Join-Path $OutputDir "$name.tga"
    $actualPng = Join-Path $OutputDir "$name.png"
    $actualMetrics = Join-Path $OutputDir "$name.metrics.txt"
    $baselineTga = Join-Path $BaselineDir "$name.tga"
    $baselinePng = Join-Path $BaselineDir "$name.png"
    $baselineMetrics = Join-Path $BaselineDir "$name.metrics.txt"
    if (-not (Test-Path -LiteralPath $actualTga) -or
        -not (Test-Path -LiteralPath $actualMetrics)) {
        throw "Vertical-slice scenario '$name' did not produce capture artifacts."
    }
    if (-not (Test-Path -LiteralPath $actualPng)) {
        Write-PngPreview $actualTga $actualPng
    }
    $metrics = Get-Content -LiteralPath $actualMetrics -Raw
    if ($metrics -notmatch "queued=yes" -or
        $metrics -notmatch "scenario=$([regex]::Escape($name))") {
        throw "Vertical-slice scenario '$name' has invalid metrics."
    }

    if ($UpdateBaselines) {
        Copy-Item -LiteralPath $actualTga -Destination $baselineTga -Force
        Copy-Item -LiteralPath $actualMetrics -Destination $baselineMetrics -Force
        if (Test-Path -LiteralPath $actualPng) {
            Copy-Item -LiteralPath $actualPng -Destination $baselinePng -Force
        }
        Write-Host "UPDATED [$name]: $baselineTga"
        continue
    }
    if (-not (Test-Path -LiteralPath $baselineTga)) {
        throw "Golden baseline '$name' is missing. Review captures, then use -UpdateBaselines."
    }
    $comparison = Compare-Tga $actualTga $baselineTga `
        $PixelTolerance $MaxChangedPixelPercent
    $results += $comparison
    Write-Host ("PASS [{0}]: {1}x{2}; {3:N4}% pixels changed; max delta {4}." -f
        $name, $comparison.Width, $comparison.Height,
        $comparison.ChangedPercent, $comparison.MaxChannelDelta)
}

if (-not $UpdateBaselines) {
    Write-Host "PASS: $($results.Count) production vertical-slice golden scenarios."
}
