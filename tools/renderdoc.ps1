<#
.SYNOPSIS
    Inspect capture status, request a frame, pull its RDC, or verify a replay.
.EXAMPLE
    .\tools\renderdoc.ps1 status -ProcessId 12345
.EXAMPLE
    .\tools\renderdoc.ps1 capture-trigger -ProcessId 12345
.EXAMPLE
    .\tools\renderdoc.ps1 pull -ProcessId 12345
.EXAMPLE
    .\tools\renderdoc.ps1 verify .\captures\frame.rdc
.EXAMPLE
    .\tools\renderdoc.ps1 server-start
    .\tools\renderdoc.ps1 verify .\captures\frame.rdc -RemoteHost 127.0.0.1
#>
param(
    [Parameter(Position=0)][string]$Action = "status",
    [Parameter(Position=1)][string]$Arg1 = "",
    [string]$Device = "",
    [int]$ProcessId = 0,
    [string]$Package = "com.movtery.zalithlauncher.v2",
    [string]$StatusPath = "",
    [string]$CaptureRoot = "",
    [string]$OutputDirectory = "captures",
    [string]$RenderDocDirectory = "",
    [string]$RemoteHost = "",
    [switch]$ExpectedClear
)
$ErrorActionPreference = "Stop"
$Workspace = Split-Path -Parent $PSScriptRoot
$AdbCommand = Get-Command adb -ErrorAction SilentlyContinue
$ADB = if ($AdbCommand) { $AdbCommand.Source } elseif (Test-Path "D:/SDK/platform-tools/adb.exe") { "D:/SDK/platform-tools/adb.exe" } else { "" }
if (!$RenderDocDirectory) {
    foreach ($candidate in @("D:/Program Files/RenderDoc", "C:/Program Files/RenderDoc", "$env:ProgramFiles/RenderDoc")) {
        if (Test-Path "$candidate/renderdoccmd.exe") { $RenderDocDirectory = $candidate; break }
    }
}
function Require-RenderDoc {
    if (!$RenderDocDirectory -or !(Test-Path "$RenderDocDirectory/qrenderdoc.exe")) { throw "RenderDoc SDK not found; pass -RenderDocDirectory." }
}
function Quote-Remote([string]$Value) {
    $quote = [string][char]39
    $escaped = $quote + [char]34 + $quote + [char]34 + $quote
    return $quote + $Value.Replace($quote, $escaped) + $quote
}
function Get-Device {
    if (!$ADB) { throw "adb not found." }
    if ($Device) { return $Device }
    $connected = @(& $ADB devices | Where-Object { $_ -match "\sdevice$" } | ForEach-Object { ($_ -split '\s+')[0] })
    if ($connected.Count -ne 1) { throw "Pass -Device when exactly one connected device cannot be selected." }
    return $connected[0]
}
function Invoke-Root([string]$Command) {
    $rootCommand = "su -c " + (Quote-Remote $Command)
    $result = @(& $ADB -s $script:TargetDevice shell $rootCommand)
    if ($LASTEXITCODE -ne 0) { throw "Device command failed. Private application paths require root access." }
    return $result
}
function Read-Remote([string]$Path) {
    $command = "cat " + (Quote-Remote $Path)
    $readable = $false
    try {
        $result = @(& $ADB -s $script:TargetDevice shell $command 2>$null)
        $readable = $LASTEXITCODE -eq 0
    } catch { $readable = $false }
    if (!$readable) { $result = @(Invoke-Root $command) }
    return ($result -join [Environment]::NewLine)
}
function Get-CaptureStatus {
    $selectedPid = $ProcessId
    $path = $StatusPath
    if (!$path) {
        if (!$selectedPid) {
            $ids = @((& $ADB -s $script:TargetDevice shell ("pidof " + (Quote-Remote $Package))) -split '\s+' | Where-Object { $_ -match '^\d+$' })
            if ($ids.Count -ne 1) { throw "Select the game process with -ProcessId; no unique process for $Package." }
            $selectedPid = [int]$ids[0]
        }
        $environmentCommand = "tr " + (Quote-Remote '\000') + " " + (Quote-Remote '\n') +
            " < /proc/$selectedPid/environ | sed -n " + (Quote-Remote '/^HOME=/p;/^VULKAN_FIX_CAPTURE_STATUS=/p')
        $settings = @(Invoke-Root $environmentCommand)
        $homeSetting = @($settings | Where-Object { $_ -like 'HOME=*' } | Select-Object -First 1)
        $override = @($settings | Where-Object { $_ -like 'VULKAN_FIX_CAPTURE_STATUS=*' } | Select-Object -First 1)
        if ($override.Count) { $path = $override[0].Substring('VULKAN_FIX_CAPTURE_STATUS='.Length).Trim() }
        elseif ($homeSetting.Count) { $path = $homeSetting[0].Substring(5).TrimEnd('/') + '/vulkanfix-renderdoc.json' }
        else {
            # Zygote children can set HOME after startup; those changes are
            # absent from /proc/PID/environ. Use the manager's exact path log.
            $initialization = @(& $ADB -s $script:TargetDevice shell logcat -d "--pid=$selectedPid" -s 'VulkanFix:I' '*:S')
            foreach ($line in $initialization) {
                if ($line -match 'RenderDoc: control API available;.* status=(.+) trigger=') { $path = $Matches[1] }
            }
            if (!$path) { throw "Capture status path is unknown; pass -StatusPath." }
        }
    }
    try { $data = (Read-Remote $path) | ConvertFrom-Json }
    catch { throw "No readable RenderDoc status at $path. Select the RenderDoc renderer and the game process, or pass -StatusPath." }
    if ($selectedPid -and [int]$data.pid -ne $selectedPid) { throw "Status belongs to a previous process, not PID $selectedPid." }
    $root = if ($CaptureRoot) { $CaptureRoot.TrimEnd('/') } else { $path.Substring(0, $path.LastIndexOf('/')) }
    if (!$root -or $root -eq '/') { throw "Pass a specific -CaptureRoot." }
    return [pscustomobject]@{ Data = $data; Path = $path; Root = $root }
}
function Assert-CapturePath([string]$Path, $Context) {
    if (!$Path.StartsWith($Context.Root + '/', [StringComparison]::Ordinal) -or
        $Path -match '(^|/)\.\.?(/|$)|[\r\n]' -or $Path.Contains([string][char]0)) {
        throw "Capture path is outside the selected root. Pass -CaptureRoot for a configured output directory."
    }
}
function Pull-Capture($Context) {
    $path = [string]$Context.Data.last_capture
    if (!$path -or !$path.EndsWith('.rdc')) { throw "No successful RDC is recorded." }
    Assert-CapturePath $path $Context
    $destination = [IO.Path]::GetFullPath($OutputDirectory)
    New-Item -ItemType Directory -Force -Path $destination | Out-Null
    $temporary = "/data/local/tmp/vulkanfix-pull-" + [Guid]::NewGuid().ToString('N') + ".rdc"
    $local = Join-Path $destination ($path.Substring($path.LastIndexOf('/') + 1))
    try {
        Invoke-Root ("test ! -e " + (Quote-Remote $temporary) + " && cp " + (Quote-Remote $path) +
            " " + (Quote-Remote $temporary) + " && chmod 644 " + (Quote-Remote $temporary)) | Out-Null
        & $ADB -s $script:TargetDevice pull $temporary $local
        if ($LASTEXITCODE -ne 0) { throw "Could not pull the capture." }
    } finally {
        Invoke-Root ("rm -f " + (Quote-Remote $temporary)) | Out-Null
    }
    return $local
}
function Verify-Capture([string]$Capture, [bool]$ClearOracle) {
    Require-RenderDoc
    $capturePath = (Resolve-Path -LiteralPath $Capture).Path
    $directory = [IO.Path]::GetFullPath($OutputDirectory)
    New-Item -ItemType Directory -Force -Path $directory | Out-Null
    $report = Join-Path $directory ("replay-" + [Guid]::NewGuid().ToString('N') + ".json")
    $values = @{
        VULKAN_FIX_REPLAY_CAPTURE = $capturePath
        VULKAN_FIX_REPLAY_REPORT = $report
        VULKAN_FIX_REPLAY_EXPECT_CLEAR = $(if ($ClearOracle) { "1" } else { "0" })
        VULKAN_FIX_REPLAY_HOST = $RemoteHost
    }
    $oldValues = @{}
    try {
        foreach ($key in $values.Keys) {
            $oldValues[$key] = [Environment]::GetEnvironmentVariable($key, 'Process')
            [Environment]::SetEnvironmentVariable($key, $values[$key], 'Process')
        }
        $scriptPath = Join-Path $PSScriptRoot 'renderdoc_replay_check.py'
        $helper = Start-Process -FilePath "$RenderDocDirectory/qrenderdoc.exe" -ArgumentList @('--python', ('"' + $scriptPath + '"')) -WindowStyle Hidden -PassThru
    } finally {
        foreach ($key in $oldValues.Keys) { [Environment]::SetEnvironmentVariable($key, $oldValues[$key], 'Process') }
    }
    if (!$helper.WaitForExit(60000)) { throw "Replay helper is still running (PID $($helper.Id)); report: $report" }
    if (!(Test-Path -LiteralPath $report)) { throw "Replay exited without a report (exit $($helper.ExitCode))." }
    $result = Get-Content -LiteralPath $report -Raw | ConvertFrom-Json
    $result | ConvertTo-Json -Depth 5 | Write-Host
    if ($helper.ExitCode -ne 0 -or !$result.ok) { throw "Capture replay failed; see $report" }
    Write-Host "Replay verified. Report: $report"
}
switch ($Action.ToLowerInvariant()) {
    "verify" {
        if (!$Arg1) { throw "Usage: renderdoc.ps1 verify <capture.rdc>" }
        Verify-Capture $Arg1 $ExpectedClear.IsPresent
    }
    "gui" {
        Require-RenderDoc
        if ($Arg1) { Start-Process "$RenderDocDirectory/qrenderdoc.exe" -ArgumentList ('"' + (Resolve-Path -LiteralPath $Arg1).Path + '"') }
        else { Start-Process "$RenderDocDirectory/qrenderdoc.exe" }
    }
    "status" {
        $script:TargetDevice = Get-Device
        if ($RenderDocDirectory) { & "$RenderDocDirectory/renderdoccmd.exe" version }
        $context = Get-CaptureStatus
        $context.Data | ConvertTo-Json -Depth 5
        Write-Host "Status path: $($context.Path)"
    }
    "capture-trigger" {
        $script:TargetDevice = Get-Device
        $context = Get-CaptureStatus
        if (!$context.Data.api_available -or [int]$context.Data.devices -lt 1) { throw "No Vulkan device is available for capture." }
        Invoke-Root ("test -d /proc/" + [int]$context.Data.pid) | Out-Null
        $trigger = [string]$context.Data.trigger
        Assert-CapturePath $trigger $context
        Invoke-Root ("touch " + (Quote-Remote $trigger) + " && chmod 644 " + (Quote-Remote $trigger)) | Out-Null
        Write-Host "Capture requested for PID $($context.Data.pid). Success requires completed to increase and a new RDC path."
    }
    "pull" {
        $script:TargetDevice = Get-Device
        $context = Get-CaptureStatus
        $file = Pull-Capture $context
        Write-Host "Saved: $file"
    }
    "test" {
        Require-RenderDoc
        $script:TargetDevice = Get-Device
        $nativeDirectory = Join-Path $Workspace 'layer/build/intermediates/cxx'
        $main = Get-ChildItem $nativeDirectory -Recurse -Filter libvulkanfix.so | Where-Object { $_.FullName -match 'arm64-v8a' } | Sort-Object LastWriteTime -Descending | Select-Object -First 1
        $test = Get-ChildItem $nativeDirectory -Recurse -Filter vulkan_fix_test | Where-Object { $_.FullName -match 'arm64-v8a' } | Sort-Object LastWriteTime -Descending | Select-Object -First 1
        $payload = Join-Path $Workspace 'pluginApp/build/renderdoc-jniLibs/arm64-v8a'
        if (!$main -or !$test -or !(Test-Path "$payload/libvulkan_fix_renderdoc_driver.so")) { throw "Build vulkan_fix_test and run tools/prepare_renderdoc_capture.py first." }
        $remote = '/data/local/tmp/vulkanfix-renderdoc'
        & $ADB -s $script:TargetDevice shell "mkdir -p $remote"
        foreach ($file in @($main.FullName, $test.FullName, "$payload/libvulkan_fix_renderdoc_driver.so", "$payload/libVkLayer_GLES_RenderDoc.so")) {
            & $ADB -s $script:TargetDevice push $file "$remote/"
            if ($LASTEXITCODE -ne 0) { throw "Could not stage $file" }
        }
        $command = "cd $remote && chmod 755 vulkan_fix_test && HOME=$remote VULKAN_FIX_RENDERDOC_DRIVER=1 VULKAN_FIX_TEST_DEBUG_UTILS=1 VULKAN_FIX_DISABLE_PIPELINE_CACHE=1 VULKAN_FIX_LOG_PATH=./capture-layer.log LD_LIBRARY_PATH=. timeout 45 ./vulkan_fix_test"
        & $ADB -s $script:TargetDevice shell $command
        if ($LASTEXITCODE -ne 0) { throw "Native capture test failed." }
        $context = [pscustomobject]@{ Data = ((Read-Remote "$remote/vulkanfix-renderdoc.json") | ConvertFrom-Json); Root = $remote }
        $file = Pull-Capture $context
        Verify-Capture $file $true
    }
    "server-start" {
        $script:TargetDevice = Get-Device
        & $ADB -s $script:TargetDevice shell am start -n org.renderdoc.renderdoccmd.arm64/.Loader -e renderdoccmd remoteserver
        if ($LASTEXITCODE -ne 0) { throw "Could not start the RenderDoc replay server." }
        & $ADB -s $script:TargetDevice forward tcp:39920 localabstract:renderdoc_39920
    }
    "server-stop" {
        $script:TargetDevice = Get-Device
        & $ADB -s $script:TargetDevice shell am force-stop org.renderdoc.renderdoccmd.arm64
        & $ADB -s $script:TargetDevice forward --remove tcp:39920
    }
    default { throw "Commands: status, capture-trigger, pull, verify <file.rdc>, test, gui [file.rdc], server-start, server-stop" }
}
