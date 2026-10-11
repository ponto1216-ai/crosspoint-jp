param(
    [ValidateSet("X3", "X4")]
    [string]$Device = "X4",
    [switch]$BuildOnly
)

$ErrorActionPreference = "Stop"
$Distro = "Ubuntu-24.04"
$LinuxProject = "/opt/yomuka-sim"
$RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$Drive = $RepoRoot.Substring(0, 1).ToLowerInvariant()
$PathWithoutDrive = $RepoRoot.Substring(2).Replace('\', '/')
$WindowsProjectInWsl = "/mnt/$Drive$PathWithoutDrive"

& wsl.exe -d $Distro -- true
if ($LASTEXITCODE -ne 0) {
    throw "Ubuntu-24.04 is not available. Check the WSL installation."
}

$Environment = if ($Device -eq "X3") { "simulator_x3" } else { "simulator" }
$SyncAndBuild = @"
set -e
mkdir -p '$LinuxProject/fs_/books'
rsync -a --delete --exclude=.git --exclude=.pio --exclude=.cache '$WindowsProjectInWsl/' '$LinuxProject/'
cd '$LinuxProject'
/opt/platformio/bin/pio run -e '$Environment'
"@

Write-Host "Building the Yomuka $Device simulator..."
& wsl.exe -d $Distro -- bash -lc ($SyncAndBuild -replace "`r", "")
if ($LASTEXITCODE -ne 0) {
    throw "The simulator build failed."
}

if ($BuildOnly) {
    Write-Host "The simulator build completed."
    exit 0
}

$VcXsrv = "C:\Program Files\VcXsrv\vcxsrv.exe"
if (-not (Test-Path $VcXsrv)) {
    throw "VcXsrv is not installed."
}
if (-not (Get-Process vcxsrv -ErrorAction SilentlyContinue)) {
    Start-Process -FilePath $VcXsrv -ArgumentList ':0','-multiwindow','-clipboard','-nowgl','-ac' -WindowStyle Hidden
    Start-Sleep -Seconds 2
}

$DefaultRoute = (& wsl.exe -d $Distro -- ip route show default | Select-Object -First 1)
$Gateway = ($DefaultRoute -split '\s+')[2]
if ([string]::IsNullOrWhiteSpace($Gateway)) {
    throw "The Windows display address could not be detected."
}

Write-Host "Starting the simulator. Close its window to stop it."
& wsl.exe -d $Distro -- bash -lc "cd '$LinuxProject' && DISPLAY='$Gateway`:0.0' SDL_VIDEODRIVER=x11 LIBGL_ALWAYS_SOFTWARE=1 .pio/build/$Environment/program"
Write-Host "The simulator has closed."
