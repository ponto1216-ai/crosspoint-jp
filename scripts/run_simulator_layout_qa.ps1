param(
    [ValidateSet("Both", "X3", "X4")]
    [string]$Device = "Both"
)

$ErrorActionPreference = "Stop"
$Distro = "Ubuntu-24.04"
$LinuxProject = "/opt/yomuka-sim"
$RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$OutputRoot = Join-Path $RepoRoot "simulator-output\layout"
$Drive = $RepoRoot.Substring(0, 1).ToLowerInvariant()
$PathWithoutDrive = $RepoRoot.Substring(2).Replace('\', '/')
$WindowsProjectInWsl = "/mnt/$Drive$PathWithoutDrive"
$LinuxOutputRoot = "$WindowsProjectInWsl/simulator-output/layout"
$Devices = if ($Device -eq "Both") { @("X3", "X4") } else { @($Device) }

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

New-Item -ItemType Directory -Force -Path $OutputRoot | Out-Null

foreach ($CurrentDevice in $Devices) {
    & (Join-Path $PSScriptRoot "run_simulator_wsl.ps1") -Device $CurrentDevice -BuildOnly
    if ($LASTEXITCODE -ne 0) { throw "The $CurrentDevice simulator build failed." }

    $Environment = if ($CurrentDevice -eq "X3") { "simulator_x3" } else { "simulator" }
    $DeviceName = $CurrentDevice.ToLowerInvariant()
    foreach ($Language in @("ja", "en")) {
        foreach ($Orientation in 0..3) {
            foreach ($Category in 0..4) {
                $Name = "${DeviceName}_${Language}_orientation${Orientation}_settings${Category}.bmp"
                $LinuxScreenshot = "$LinuxOutputRoot/$Name"
                $LinuxSd = "/tmp/yomuka-layout-qa-${DeviceName}-${Language}-${Orientation}-${Category}"
                $SettingsJson = '{"uiOrientation":' + $Orientation + ',"uiTheme":1}'
                $WarmupScreenshot = "/tmp/yomuka-layout-warmup-${DeviceName}-${Language}-${Orientation}-${Category}.bmp"
                $Command = @"
set -e
rm -rf '$LinuxSd'
mkdir -p '$LinuxSd/.crosspoint' '$LinuxOutputRoot'
if [ -d '$LinuxProject/fs_/.fonts' ]; then cp -a '$LinuxProject/fs_/.fonts' '$LinuxSd/'; fi
printf '%s' '$SettingsJson' > '$LinuxSd/.crosspoint/settings.json'
cd '$LinuxProject'
DISPLAY='$Gateway`:0.0' SDL_VIDEODRIVER=x11 LIBGL_ALWAYS_SOFTWARE=1 CROSSPOINT_SIM_SD='$LinuxSd' CROSSPOINT_SIM_LANGUAGE='$Language' CROSSPOINT_SIM_SETTINGS_CATEGORY='$Category' CROSSPOINT_SIM_INPUT_SCRIPT='2400:QUIT' CROSSPOINT_SIM_SCREENSHOTS='1200:$WarmupScreenshot;1900:$LinuxScreenshot' .pio/build/$Environment/program
"@
                & wsl.exe -d $Distro -- bash -lc ($Command -replace "`r", "")
                if ($LASTEXITCODE -ne 0) { throw "Layout capture failed: $Name" }
            }
        }
    }
}

$CapturedFiles = foreach ($CurrentDevice in $Devices) {
    $DeviceName = $CurrentDevice.ToLowerInvariant()
    Get-ChildItem -Path $OutputRoot -Filter "${DeviceName}_*.bmp"
}
$CapturedFiles = @($CapturedFiles | Sort-Object Name)
$ExpectedCount = $Devices.Count * 2 * 4 * 5
if ($CapturedFiles.Count -ne $ExpectedCount) {
    throw "Expected $ExpectedCount screenshots, but found $($CapturedFiles.Count)."
}

$Cards = foreach ($File in $CapturedFiles) {
    $Name = [System.Net.WebUtility]::HtmlEncode($File.Name)
    '<figure><img src="./{0}" alt="{0}"><figcaption>{0}</figcaption></figure>' -f $Name
}
$Report = @"
<!doctype html>
<html lang="ja"><meta charset="utf-8"><title>Yomuka layout QA</title>
<style>
body{font-family:sans-serif;margin:20px}main{display:grid;grid-template-columns:repeat(5,minmax(180px,1fr));gap:16px}
figure{margin:0;border:1px solid #bbb;padding:8px}img{display:block;width:100%;height:260px;object-fit:contain}figcaption{font-size:12px;overflow-wrap:anywhere}
</style><h1>Yomuka layout QA</h1><p>$($CapturedFiles.Count) screenshots</p><main>
$($Cards -join "`n")
</main></html>
"@
$ReportPath = Join-Path $OutputRoot "index.html"
Set-Content -LiteralPath $ReportPath -Value $Report -Encoding utf8
Write-Host "Saved $($CapturedFiles.Count) layout screenshots and report to $OutputRoot"
