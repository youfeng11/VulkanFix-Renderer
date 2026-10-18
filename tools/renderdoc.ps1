<#
.SYNOPSIS
    VulkanFix RenderDoc Automation & Integration Tool
.DESCRIPTION
    Convenient CLI to invoke RenderDoc on Windows, manage Android remote replay server,
    trigger in-app Vulkan captures, and inspect .rdc captures.
#>

param(
    [Parameter(Position=0)]
    [string]$Action = "status",

    [Parameter(Position=1)]
    [string]$Arg1 = "",

    [string]$Device = ""
)

$ErrorActionPreference = "Stop"

# Auto-detect RenderDoc installation
$RDOC_PATHS = @(
    "D:\Program Files\RenderDoc",
    "C:\Program Files\RenderDoc",
    "${env:ProgramFiles}\RenderDoc"
)
$RDOC_DIR = ""
foreach ($p in $RDOC_PATHS) {
    if (Test-Path "$p\renderdoccmd.exe") {
        $RDOC_DIR = $p
        break
    }
}

if (-not $RDOC_DIR) {
    Write-Error "RenderDoc installation not found in standard paths."
    exit 1
}

$RENDERDOCCMD = "$RDOC_DIR\renderdoccmd.exe"
$QRENDERDOC   = "$RDOC_DIR\qrenderdoc.exe"

# Auto-detect ADB
$ADB = "adb"
if (Get-Command "adb" -ErrorAction SilentlyContinue) {
    $ADB = "adb"
} elseif (Test-Path "D:\SDK\platform-tools\adb.exe") {
    $ADB = "D:\SDK\platform-tools\adb.exe"
}

# Resolve target Android device
function Get-TargetDevice {
    if ($Device) { return $Device }
    $devLines = @(& $ADB devices | Where-Object { $_ -match "\tdevice$" -or $_ -match "\s+device$" })
    if ($devLines.Count -gt 0) {
        $line = $devLines[0].Trim()
        $parts = $line -split '\s+'
        return $parts[0].Trim()
    }
    return ""
}

$TARGET_DEVICE = Get-TargetDevice

switch ($Action.ToLower()) {
    "status" {
        Write-Host "=== RenderDoc Host Status ===" -ForegroundColor Cyan
        Write-Host "RenderDoc Path : $RDOC_DIR"
        & $RENDERDOCCMD version

        Write-Host "`n=== Target Android Device ===" -ForegroundColor Cyan
        if ($TARGET_DEVICE) {
            Write-Host "Device ID      : $TARGET_DEVICE"
            $model = & $ADB -s $TARGET_DEVICE shell getprop ro.product.model
            $mfr = & $ADB -s $TARGET_DEVICE shell getprop ro.product.manufacturer
            Write-Host "Model          : $mfr $model"

            $hasPkg = & $ADB -s $TARGET_DEVICE shell pm list packages | Select-String "org.renderdoc.renderdoccmd"
            Write-Host "RenderDoc App  : $(if ($hasPkg) { 'Installed (' + $hasPkg.ToString().Trim() + ')' } else { 'Not Installed' })"

            $procId = & $ADB -s $TARGET_DEVICE shell pidof org.renderdoc.renderdoccmd.arm64
            Write-Host "Server State   : $(if ($procId) { "Running (PID: $procId)" } else { "Not Running" })"
        } else {
            Write-Host "No connected Android devices detected." -ForegroundColor Yellow
        }
    }

    "server-start" {
        if (-not $TARGET_DEVICE) { Write-Error "No Android device connected"; exit 1 }
        Write-Host "Starting RenderDoc replay server on device $TARGET_DEVICE..." -ForegroundColor Cyan
        & $ADB -s $TARGET_DEVICE shell "am start -n org.renderdoc.renderdoccmd.arm64/.Loader -e renderdoccmd remoteserver"
        & $ADB -s $TARGET_DEVICE forward tcp:38920 localabstract:renderdoc_38920
        Write-Host "Forwarded tcp:38920 -> localabstract:renderdoc_38920" -ForegroundColor Green
    }

    "server-stop" {
        if (-not $TARGET_DEVICE) { Write-Error "No Android device connected"; exit 1 }
        Write-Host "Stopping RenderDoc replay server on device $TARGET_DEVICE..." -ForegroundColor Yellow
        & $ADB -s $TARGET_DEVICE shell "am force-stop org.renderdoc.renderdoccmd.arm64"
        & $ADB -s $TARGET_DEVICE forward --remove tcp:38920 2>$null
        Write-Host "Server stopped." -ForegroundColor Green
    }

    "gui" {
        Write-Host "Launching RenderDoc GUI ($QRENDERDOC)..." -ForegroundColor Cyan
        if ($Arg1) {
            Start-Process $QRENDERDOC -ArgumentList "`"$Arg1`""
        } else {
            Start-Process $QRENDERDOC
        }
    }

    "test" {
        if (-not $TARGET_DEVICE) { Write-Error "No Android device connected"; exit 1 }
        Write-Host "Syncing latest arm64-v8a binaries to device..." -ForegroundColor Cyan
        $soFile = Get-ChildItem -Path "app\build\intermediates\cxx" -Filter "libvulkan_fix.so" -Recurse | Where-Object { $_.FullName -match "arm64-v8a" } | Sort-Object LastWriteTime -Descending | Select-Object -First 1
        $exeFile = Get-ChildItem -Path "app\build\intermediates\cxx" -Filter "vulkan_fix_test" -Recurse | Where-Object { $_.FullName -match "arm64-v8a" } | Sort-Object LastWriteTime -Descending | Select-Object -First 1

        if ($soFile -and $exeFile) {
            Write-Host "Pushing $($soFile.FullName) -> /data/local/tmp/libvulkan_fix.so" -ForegroundColor DarkGray
            & $ADB -s $TARGET_DEVICE push $soFile.FullName /data/local/tmp/libvulkan_fix.so
            Write-Host "Pushing $($exeFile.FullName) -> /data/local/tmp/vulkan_fix_test" -ForegroundColor DarkGray
            & $ADB -s $TARGET_DEVICE push $exeFile.FullName /data/local/tmp/vulkan_fix_test
            & $ADB -s $TARGET_DEVICE shell "chmod 755 /data/local/tmp/vulkan_fix_test"
        }

        Write-Host "Running VulkanFix RenderDoc test on $TARGET_DEVICE..." -ForegroundColor Cyan
        & $ADB -s $TARGET_DEVICE shell "LD_LIBRARY_PATH=/data/local/tmp /data/local/tmp/vulkan_fix_test"
    }

    "capture-trigger" {
        if (-not $TARGET_DEVICE) { Write-Error "No Android device connected"; exit 1 }
        Write-Host "Sending frame capture trigger to device $TARGET_DEVICE..." -ForegroundColor Cyan
        & $ADB -s $TARGET_DEVICE shell "touch /sdcard/Download/vulkanfix_capture.trigger /sdcard/vulkanfix_capture.trigger 2>/dev/null || touch /data/local/tmp/vulkanfix_capture.trigger"
        Write-Host "Trigger sent! VulkanFix will capture the next frame." -ForegroundColor Green
    }

    "pull" {
        if (-not $TARGET_DEVICE) { Write-Error "No Android device connected"; exit 1 }
        $outDir = "captures"
        if (-not (Test-Path $outDir)) { New-Item -ItemType Directory -Path $outDir | Out-Null }
        Write-Host "Searching for .rdc capture files on device..." -ForegroundColor Cyan
        $files = & $ADB -s $TARGET_DEVICE shell "find /sdcard/ /data/local/tmp/ -name '*.rdc' 2>/dev/null"
        $lines = $files -split "`r?`n" | Where-Object { $_.Trim().EndsWith(".rdc") }
        if ($lines.Count -eq 0) {
            Write-Host "No .rdc capture files found on device." -ForegroundColor Yellow
        } else {
            foreach ($f in $lines) {
                $f = $f.Trim()
                Write-Host "Pulling $f -> $outDir/ ..." -ForegroundColor Green
                & $ADB -s $TARGET_DEVICE pull "$f" "$outDir/"
            }
            Write-Host "Done! Files saved in $outDir\" -ForegroundColor Green
        }
    }

    "inspect" {
        if (-not $Arg1) {
            Write-Host "Usage: .\tools\renderdoc.ps1 inspect <path_to_rdc>" -ForegroundColor Yellow
            exit 1
        }
        Write-Host "Inspecting capture: $Arg1" -ForegroundColor Cyan
        & $RENDERDOCCMD replay --preview "$Arg1"
    }

    default {
        Write-Host "Usage: .\tools\renderdoc.ps1 <command>" -ForegroundColor Cyan
        Write-Host "Commands:"
        Write-Host "  status             - Show RenderDoc PC & Android device connection status"
        Write-Host "  server-start       - Start RenderDoc replay server on Android & forward port"
        Write-Host "  server-stop        - Stop RenderDoc replay server on Android"
        Write-Host "  gui [file.rdc]     - Launch RenderDoc GUI on PC"
        Write-Host "  test               - Run native VulkanFix + RenderDoc pipeline test on device"
        Write-Host "  capture-trigger    - Trigger in-app capture of next frame on device"
        Write-Host "  pull               - Pull .rdc captures from device to local ./captures folder"
        Write-Host "  inspect <file.rdc> - Replay and preview capture using renderdoccmd"
    }
}
