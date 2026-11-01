# Installs the Windows CI toolchain on a fresh Windows 10/11 machine (APEX_WIN_TARGET=machine,
# docs/development.md section 6). Run as an administrator; safe to re-run, finished steps are skipped.
#   scp tools/winmachine_setup.ps1 <user>@<machine>:
#   ssh <user>@<machine> powershell -NoProfile -ExecutionPolicy Bypass -File winmachine_setup.ps1
# Silent installs accept the VS Build Tools and Vulkan SDK licences.
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'  # Invoke-WebRequest is many times slower with its progress bar
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12

$Python = '3.12.10'
$Qt = '6.10.2'
$Dl = 'C:\apex-setup'
$VsDir = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\2022\BuildTools"
New-Item -ItemType Directory -Force $Dl | Out-Null
$start = Get-Date

function Say($msg) { Write-Host ("[{0:hh\:mm\:ss}] {1}" -f ((Get-Date) - $start), $msg) }

function Refresh-Path {
    $env:Path = [Environment]::GetEnvironmentVariable('Path', 'Machine') + ';' +
                [Environment]::GetEnvironmentVariable('Path', 'User')
}

function Fetch($url, $name) {
    $out = Join-Path $Dl $name
    if (-not (Test-Path $out)) {
        Say "  downloading $url"
        Invoke-WebRequest -UseBasicParsing -Uri $url -OutFile "$out.part"
        Move-Item -Force "$out.part" $out
    }
    Say ("  {0} ({1:N0} MB)" -f $name, ((Get-Item $out).Length / 1MB))
    $out
}

function Size-GB($dir) {
    if (-not (Test-Path $dir)) { return 0 }
    (Get-ChildItem -Recurse -File -Force -ErrorAction SilentlyContinue $dir | Measure-Object Length -Sum).Sum / 1GB
}

# Run an installer, printing a heartbeat (elapsed time, size of what it has written) every 30 s.
function Install($exe, $installerArgs, $watchDir, $ok = @(0)) {
    $p = Start-Process -FilePath $exe -ArgumentList $installerArgs -PassThru -NoNewWindow
    $null = $p.Handle  # PowerShell 5.1 leaves ExitCode empty unless the handle is opened while it runs
    $t = Get-Date
    while (-not $p.WaitForExit(30000)) {
        Say ("  still installing: {0:mm\:ss} elapsed, {1:N2} GB in {2}" -f ((Get-Date) - $t), (Size-GB $watchDir), $watchDir)
    }
    if ($ok -notcontains $p.ExitCode) { throw "$exe exited with $($p.ExitCode)" }
    Say "  done (exit $($p.ExitCode))"
}

Say "[1/5] Git"
Refresh-Path
if (Get-Command git -ErrorAction SilentlyContinue) { Say "  already installed" } else {
    $rel = Invoke-RestMethod -UseBasicParsing https://api.github.com/repos/git-for-windows/git/releases/latest
    $asset = $rel.assets | Where-Object { $_.name -match '^Git-[\d.]+-64-bit\.exe$' } | Select-Object -First 1
    $exe = Fetch $asset.browser_download_url $asset.name
    Install $exe '/VERYSILENT /NORESTART /NOCANCEL /SP- /SUPPRESSMSGBOXES' "$env:ProgramFiles\Git"
}

Say "[2/5] Python $Python"
$py = "$env:ProgramFiles\Python312\python.exe"
if (Test-Path $py) { Say "  already installed" } else {
    $exe = Fetch "https://www.python.org/ftp/python/$Python/python-$Python-amd64.exe" "python-$Python-amd64.exe"
    Install $exe '/quiet InstallAllUsers=1 PrependPath=1 Include_test=0' "$env:ProgramFiles\Python312"
}

Say "[3/5] Vulkan SDK"
if (Test-Path 'C:\VulkanSDK\*\Bin\glslc.exe') { Say "  already installed" } else {
    $exe = Fetch 'https://sdk.lunarg.com/sdk/download/latest/windows/vulkan-sdk.exe' 'vulkan-sdk.exe'
    Install $exe '--accept-licenses --default-answer --confirm-command install' 'C:\VulkanSDK'
}

Say "[4/5] VS 2022 Build Tools (C++ workload, CMake, Ninja) - the long one, 20-40 min"
if (Test-Path "$VsDir\VC\Auxiliary\Build\vcvars64.bat") { Say "  already installed" } else {
    $exe = Fetch 'https://aka.ms/vs/17/release/vs_BuildTools.exe' 'vs_BuildTools.exe'
    Install $exe ('--quiet --wait --norestart --nocache ' +
                  '--add Microsoft.VisualStudio.Workload.VCTools --includeRecommended ' +
                  '--add Microsoft.VisualStudio.Component.VC.CMake.Project') $VsDir @(0, 3010)
}

Say "[5/5] Qt $Qt msvc2022_64 with Multimedia (aqtinstall)"
if (Test-Path "C:\Qt\$Qt\msvc2022_64\bin\Qt6Core.dll") { Say "  already installed" } else {
    & $py -m pip install --disable-pip-version-check -q --upgrade aqtinstall
    if ($LASTEXITCODE) { throw "pip install aqtinstall failed" }
    & $py -m aqt install-qt windows desktop $Qt win64_msvc2022_64 -m qtmultimedia -O C:\Qt
    if ($LASTEXITCODE) { throw "aqt install-qt failed" }
}

Say "Checking the toolchain the way tools/ci.py sees it"
Refresh-Path
cmd /c "call `"$VsDir\VC\Auxiliary\Build\vcvars64.bat`" >nul && where cl cmake ninja git python glslc && cmake --version | findstr version"
if ($LASTEXITCODE) { throw "toolchain check failed" }
# sshd hands every session the machine environment it read when the service started, so new SSH
# sessions would not see the PATH entries added above until it restarts. A SYSTEM task does it after
# this script has exited, which also ends this connection.
$restart = 'Start-Sleep 5; Restart-Service sshd -Force; Unregister-ScheduledTask apex-restart-sshd -Confirm:$false'
Register-ScheduledTask -TaskName apex-restart-sshd -User SYSTEM -RunLevel Highest -Force `
    -Action (New-ScheduledTaskAction -Execute powershell.exe -Argument "-NoProfile -Command $restart") | Out-Null
Start-ScheduledTask apex-restart-sshd
Say "sshd restarts in 5 s so new SSH sessions see the new PATH"
Say ("All done. {0:N0} GB free on C:" -f ((Get-PSDrive C).Free / 1GB))
