param(
    [ValidateSet("X3", "X4")]
    [string]$Device = "X3",
    [string]$Epub = "test/epubs/yomuka_vertical_punctuation_check.epub",
    [Parameter(Mandatory = $true)]
    [string]$FontDirectory,
    [Parameter(Mandatory = $true)]
    [ValidatePattern('^[A-Za-z0-9_-]+$')]
    [string]$FontFamily,
    [string]$Baseline
)

$ErrorActionPreference = "Stop"
$Distro = "Ubuntu-24.04"
$RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$EpubPath = (Resolve-Path $Epub).Path
$FontPath = (Resolve-Path $FontDirectory).Path
if (-not (Test-Path $EpubPath -PathType Leaf) -or [IO.Path]::GetExtension($EpubPath) -ne ".epub") {
    throw "An EPUB file is required: $Epub"
}
if (-not (Get-ChildItem -LiteralPath $FontPath -Filter '*.cpfont' -File | Select-Object -First 1)) {
    throw "The font directory has no .cpfont files: $FontPath"
}

function To-WslPath([string]$Path) {
    if ($Path -notmatch '^[A-Za-z]:\\') { throw "A local drive path is required: $Path" }
    $Drive = $Path.Substring(0, 1).ToLowerInvariant()
    return "/mnt/$Drive" + $Path.Substring(2).Replace('\', '/')
}
function BashQuote([string]$Value) { return "'" + $Value.Replace("'", "'\''") + "'" }

$OutputRoot = Join-Path $RepoRoot "simulator-output\reader-qa"
$RunName = "{0}_{1}_{2}_{3}" -f $Device.ToLowerInvariant(), [IO.Path]::GetFileNameWithoutExtension($EpubPath), $FontFamily, [guid]::NewGuid().ToString('N')
$RunDir = Join-Path $OutputRoot $RunName
New-Item -ItemType Directory -Force -Path $RunDir | Out-Null
$Capture = Join-Path $RunDir "page1.bmp"
$Log = Join-Path $RunDir "run.log"
$LinuxEpub = To-WslPath $EpubPath
$LinuxFont = To-WslPath $FontPath
$LinuxCapture = To-WslPath $Capture
$LinuxSd = "/tmp/yomuka-reader-qa-$([guid]::NewGuid().ToString('N'))"
$Settings = '{"writingMode":2,"horizontal":{},"vertical":{"sdFontFamilyName":"' + $FontFamily + '"}}'

& (Join-Path $PSScriptRoot "run_simulator_wsl.ps1") -Device $Device -BuildOnly
if ($LASTEXITCODE -ne 0) { throw "Simulator build failed" }
$VcXsrv = "C:\Program Files\VcXsrv\vcxsrv.exe"
if (-not (Test-Path $VcXsrv)) { throw "VcXsrv is not installed" }
if (-not (Get-Process vcxsrv -ErrorAction SilentlyContinue)) {
    Start-Process -FilePath $VcXsrv -ArgumentList ':0','-multiwindow','-clipboard','-nowgl','-ac' -WindowStyle Hidden
    Start-Sleep -Seconds 2
}
$Route = (& wsl.exe -d $Distro -- ip route show default | Select-Object -First 1)
$Gateway = ($Route -split '\s+')[2]
if (-not $Gateway) { throw "Could not determine the simulator display address" }
$Environment = if ($Device -eq "X3") { "simulator_x3" } else { "simulator" }
$InputEvents = "4500:ENTER;8500:ENTER;12500:ENTER;23000:QUIT"
$ScreenshotEvent = "20000:$LinuxCapture"
$Command = @"
set -e
mkdir -p '$LinuxSd/books' '$LinuxSd/.fonts/$FontFamily' '$LinuxSd/.crosspoint'
cp $(BashQuote $LinuxEpub) '$LinuxSd/books/'
cp -a $(BashQuote $LinuxFont)/. '$LinuxSd/.fonts/$FontFamily/'
printf '%s' $(BashQuote $Settings) > '$LinuxSd/.crosspoint/settings.json'
cd /opt/yomuka-sim
DISPLAY='$Gateway`:0.0' SDL_VIDEODRIVER=x11 LIBGL_ALWAYS_SOFTWARE=1 CROSSPOINT_SIM_SD='$LinuxSd' CROSSPOINT_SIM_LANGUAGE=ja CROSSPOINT_SIM_INPUT_SCRIPT=$(BashQuote $InputEvents) CROSSPOINT_SIM_SCREENSHOTS=$(BashQuote $ScreenshotEvent) .pio/build/$Environment/program
"@
& wsl.exe -d $Distro -- bash -lc ($Command -replace "`r", "") 2>&1 | Tee-Object -FilePath $Log
if ($LASTEXITCODE -ne 0) { throw "Simulator run failed; see $Log" }
if (-not (Test-Path $Capture -PathType Leaf)) { throw "Screenshot was not saved; see $Log" }
$RunLog = Get-Content -LiteralPath $Log -Raw
if ($RunLog -notmatch 'verticalMode=1' -or
    $RunLog -notmatch ('Loaded SD font family: ' + [regex]::Escape($FontFamily)) -or
    $RunLog -notmatch 'Rendered page in') {
    throw "Reader or requested font did not load; screenshot is not valid. See $Log"
}
Write-Host "Saved verified vertical reader screenshot: $Capture"
if ($Baseline) {
    py -3 (Join-Path $PSScriptRoot "compare_simulator_screenshots.py") $Baseline $RunDir (Join-Path $RunDir "comparison")
    if ($LASTEXITCODE -ne 0) { throw "Screenshot differs from baseline" }
}
