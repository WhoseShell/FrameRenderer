param(
    [string]$ExePath = (Join-Path (Split-Path $PSScriptRoot -Parent) "build_vs\Release\dx12_hello.exe"),
    [string]$LogPath = "",
    [switch]$KeepLog,
    [int]$LaunchCycles = 5
)

$ErrorActionPreference = "Stop"

if ([string]::IsNullOrWhiteSpace($LogPath)) {
    $LogPath = Join-Path ([System.IO.Path]::GetTempPath()) ("shellengine_imgui_ui_smoke_{0}_{1}.log" -f $PID, [Guid]::NewGuid().ToString("N"))
}

Add-Type @"
using System;
using System.Runtime.InteropServices;
using System.Text;

public static class ImGuiSmokeWin32 {
    public delegate bool EnumWindowsProc(IntPtr hWnd, IntPtr lParam);
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumWindowsProc cb, IntPtr lParam);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr hWnd, out uint pid);
    [DllImport("user32.dll")] public static extern int GetWindowTextW(IntPtr hWnd, StringBuilder text, int maxCount);
    [DllImport("user32.dll")] public static extern int GetClassNameW(IntPtr hWnd, StringBuilder text, int maxCount);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr hWnd, out RECT rect);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr hWnd);
    [DllImport("user32.dll")] public static extern bool IsHungAppWindow(IntPtr hWnd);
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr hWnd);
    [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr hWnd, IntPtr after, int x, int y, int cx, int cy, uint flags);
    [DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr hWnd, ref POINT pt);
    [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
    [DllImport("user32.dll")] public static extern void mouse_event(uint flags, uint dx, uint dy, uint data, UIntPtr extra);
    [DllImport("user32.dll")] public static extern bool PostMessageW(IntPtr hWnd, uint msg, IntPtr wParam, IntPtr lParam);
    [StructLayout(LayoutKind.Sequential)] public struct POINT { public int X; public int Y; }
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left; public int Top; public int Right; public int Bottom; }
}
"@

$WM_CLOSE = 0x0010
$MOUSEEVENTF_LEFTDOWN = 0x0002
$MOUSEEVENTF_LEFTUP = 0x0004
$SWP_NOZORDER = 0x0004
$SWP_SHOWWINDOW = 0x0040

$script:LastAction = "startup"
$script:HadFailure = $true

if (Test-Path -LiteralPath $LogPath) {
    Remove-Item -LiteralPath $LogPath -Force
}

function Log-Step([string]$message) {
    $line = ("{0:HH:mm:ss.fff} {1}" -f (Get-Date), $message)
    try {
        [System.IO.File]::AppendAllText($LogPath, $line + [Environment]::NewLine, [Text.Encoding]::UTF8)
    } catch {
        Write-Host ("log write skipped: " + $_.Exception.Message)
    }
    Write-Host $line
}

function Get-WindowText([IntPtr]$hwnd) {
    $sb = [Text.StringBuilder]::new(512)
    [void][ImGuiSmokeWin32]::GetWindowTextW($hwnd, $sb, $sb.Capacity)
    $sb.ToString()
}

function Get-ClassName([IntPtr]$hwnd) {
    $sb = [Text.StringBuilder]::new(128)
    [void][ImGuiSmokeWin32]::GetClassNameW($hwnd, $sb, $sb.Capacity)
    $sb.ToString()
}

function Get-WindowRectText([IntPtr]$hwnd) {
    [ImGuiSmokeWin32+RECT]$rect = New-Object ImGuiSmokeWin32+RECT
    if (-not [ImGuiSmokeWin32]::GetWindowRect($hwnd, [ref]$rect)) {
        return "rect=unavailable"
    }
    return ("rect={0},{1},{2},{3}" -f $rect.Left, $rect.Top, ($rect.Right - $rect.Left), ($rect.Bottom - $rect.Top))
}

function Get-TopWindowsForPid([int]$targetPid) {
    $result = New-Object System.Collections.Generic.List[IntPtr]
    $cb = [ImGuiSmokeWin32+EnumWindowsProc]{
        param([IntPtr]$h, [IntPtr]$l)
        [uint32]$windowPid = 0
        [void][ImGuiSmokeWin32]::GetWindowThreadProcessId($h, [ref]$windowPid)
        if ($windowPid -eq $targetPid -and [ImGuiSmokeWin32]::IsWindowVisible($h)) {
            $script:enumResult.Add($h)
        }
        return $true
    }
    $script:enumResult = $result
    [void][ImGuiSmokeWin32]::EnumWindows($cb, [IntPtr]::Zero)
    $script:enumResult = $null
    return @($result)
}

function Find-MainWindow($proc) {
    for ($i = 0; $i -lt 100; ++$i) {
        $proc.Refresh()
        if ($proc.HasExited) { throw "process exited before main window appeared" }
        if ($proc.MainWindowHandle -ne [IntPtr]::Zero) { return $proc.MainWindowHandle }
        foreach ($h in Get-TopWindowsForPid $proc.Id) {
            if ((Get-WindowText $h) -like "ShellEngine*") { return $h }
        }
        Start-Sleep -Milliseconds 100
    }
    throw "main window was not found"
}

function Dump-WindowState($proc, [IntPtr]$main) {
    try {
        $proc.Refresh()
        Log-Step ("STATE lastAction={0} hasExited={1} responding={2}" -f $script:LastAction, $proc.HasExited, $proc.Responding)
        foreach ($h in Get-TopWindowsForPid $proc.Id) {
            Log-Step ("STATE hwnd={0} class='{1}' title='{2}' hung={3} {4}" -f $h, (Get-ClassName $h), (Get-WindowText $h), [ImGuiSmokeWin32]::IsHungAppWindow($h), (Get-WindowRectText $h))
        }
    } catch {
        Write-Host ("state dump failed: " + $_.Exception.Message)
    }
}

function Close-CommonDialogs($proc, [IntPtr]$main) {
    foreach ($h in Get-TopWindowsForPid $proc.Id) {
        if ($h -eq $main) { continue }
        $class = Get-ClassName $h
        $title = Get-WindowText $h
        if ($class -eq "#32770" -or $title -match "Open|Save|Import|Select|Choose|Browse") {
            [void][ImGuiSmokeWin32]::PostMessageW($h, $WM_CLOSE, [IntPtr]::Zero, [IntPtr]::Zero)
        }
    }
}

function Assert-Responsive($proc, [IntPtr]$main, [string]$label) {
    Start-Sleep -Milliseconds 250
    Close-CommonDialogs $proc $main
    Start-Sleep -Milliseconds 120
    $proc.Refresh()
    if ($proc.HasExited) {
        Dump-WindowState $proc $main
        throw "process exited after: $label"
    }
    if (-not $proc.Responding -or [ImGuiSmokeWin32]::IsHungAppWindow($main)) {
        Dump-WindowState $proc $main
        throw "window not responding after: $label"
    }
    Log-Step ("OK  " + $label)
}

function Click-Client([IntPtr]$main, [int]$x, [int]$y) {
    [ImGuiSmokeWin32+POINT]$pt = New-Object ImGuiSmokeWin32+POINT
    $pt.X = $x
    $pt.Y = $y
    [void][ImGuiSmokeWin32]::ClientToScreen($main, [ref]$pt)
    [void][ImGuiSmokeWin32]::SetForegroundWindow($main)
    [void][ImGuiSmokeWin32]::SetCursorPos($pt.X, $pt.Y)
    Start-Sleep -Milliseconds 35
    [ImGuiSmokeWin32]::mouse_event($MOUSEEVENTF_LEFTDOWN, 0, 0, 0, [UIntPtr]::Zero)
    Start-Sleep -Milliseconds 40
    [ImGuiSmokeWin32]::mouse_event($MOUSEEVENTF_LEFTUP, 0, 0, 0, [UIntPtr]::Zero)
}

function DoubleClick-Client([IntPtr]$main, [int]$x, [int]$y) {
    Click-Client $main $x $y
    Start-Sleep -Milliseconds 85
    Click-Client $main $x $y
}

function Do-Click($proc, [IntPtr]$main, [string]$label, [int]$x, [int]$y) {
    $script:LastAction = $label
    Log-Step ("DO  " + $label)
    Click-Client $main $x $y
    Assert-Responsive $proc $main $label
}

function Do-DoubleClick($proc, [IntPtr]$main, [string]$label, [int]$x, [int]$y) {
    $script:LastAction = $label
    Log-Step ("DO  " + $label)
    DoubleClick-Client $main $x $y
    Assert-Responsive $proc $main $label
}

function Do-Resize($proc, [IntPtr]$main, [int]$width, [int]$height, [string]$label) {
    $script:LastAction = $label
    Log-Step ("DO  " + $label)
    [void][ImGuiSmokeWin32]::SetWindowPos($main, [IntPtr]::Zero, 30, 30, $width, $height, $SWP_NOZORDER -bor $SWP_SHOWWINDOW)
    Assert-Responsive $proc $main $label
}

function Stop-TestProcess($proc, [IntPtr]$main) {
    if ($null -eq $proc) { return }
    try {
        if ($main -ne [IntPtr]::Zero) {
            Close-CommonDialogs $proc $main
            [void][ImGuiSmokeWin32]::PostMessageW($main, $WM_CLOSE, [IntPtr]::Zero, [IntPtr]::Zero)
            Start-Sleep -Milliseconds 250
        }
        $proc.Refresh()
        if (-not $proc.HasExited) {
            Stop-Process -Id $proc.Id -Force
        }
    } catch {
    }
}

function Start-TestApp([int]$width, [int]$height, [string]$label) {
    $repo = Split-Path $PSScriptRoot -Parent
    $proc = Start-Process -FilePath $ExePath -WorkingDirectory $repo -PassThru
    $main = Find-MainWindow $proc
    [void][ImGuiSmokeWin32]::SetWindowPos($main, [IntPtr]::Zero, 40, 40, $width, $height, $SWP_NOZORDER -bor $SWP_SHOWWINDOW)
    Assert-Responsive $proc $main $label
    return @{ Proc = $proc; Main = $main }
}

function Test-LaunchCycles() {
    for ($i = 1; $i -le $LaunchCycles; ++$i) {
        $state = $null
        try {
            $state = Start-TestApp 960 640 ("launch cycle {0}/{1}" -f $i, $LaunchCycles)
        } finally {
            if ($state) {
                Stop-TestProcess $state.Proc $state.Main
            }
        }
    }
    Log-Step ("PASS launch/close cycles x" + $LaunchCycles)
}

if (-not (Test-Path -LiteralPath $ExePath)) {
    throw "exe not found: $ExePath"
}

$proc = $null
$main = [IntPtr]::Zero
try {
    Test-LaunchCycles

    $state = Start-TestApp 1280 760 "launch and resize 1280x760"
    $proc = $state.Proc
    $main = $state.Main

    for ($i = 1; $i -le 3; ++$i) {
        Do-Click $proc $main ("open File menu repeat {0}" -f $i) 18 10
        Do-Click $proc $main ("close File menu repeat {0}" -f $i) 360 10
        Do-Click $proc $main ("open Window menu repeat {0}" -f $i) 67 10
        Do-Click $proc $main ("close Window menu repeat {0}" -f $i) 360 10
    }

    Do-Click $proc $main "open Window menu" 67 10
    Do-Click $proc $main "toggle Render Settings" 88 34
    Do-Click $proc $main "render settings checkbox" 285 106
    Do-Click $proc $main "render settings combo open" 306 82
    Do-Click $proc $main "render settings combo choose" 306 106

    Do-Click $proc $main "collapse Basic actors" 26 82
    Do-Click $proc $main "expand Basic actors" 26 82
    Do-Click $proc $main "place Sphere" 38 104
    Do-Click $proc $main "place Box" 30 122
    Do-Click $proc $main "place Cone" 35 140
    Do-Click $proc $main "collapse Environment actors" 26 156
    Do-Click $proc $main "expand Environment actors" 26 156
    Do-Click $proc $main "place Sun Light" 54 178
    Do-Click $proc $main "place Sky Atmosphere" 77 196
    Do-Click $proc $main "collapse RenderDoc actors" 26 214
    Do-Click $proc $main "expand RenderDoc actors" 26 214
    Do-Click $proc $main "place RenderDoc Rock" 76 236

    Do-Click $proc $main "select outliner actor" 1085 82
    Do-DoubleClick $proc $main "double-click outliner focus" 1108 100
    Do-Click $proc $main "activate Details tab" 1202 56
    Do-Click $proc $main "details name field area" 1094 96
    Do-Click $proc $main "details material combo open" 1118 164
    Do-Click $proc $main "details material combo choose first" 1118 188
    Do-Click $proc $main "details transform drag area" 1165 134

    Do-Click $proc $main "content Import OBJ dialog cancel" 288 548
    Do-Click $proc $main "content Open dialog cancel" 615 548
    Do-Click $proc $main "content Save dialog cancel" 660 548
    Do-Click $proc $main "content folder Textures" 283 598
    Do-DoubleClick $proc $main "double-click first texture preview" 430 580
    Do-Click $proc $main "content folder Materials" 288 616
    Do-DoubleClick $proc $main "double-click first material preview/editor" 430 580
    Do-Click $proc $main "open Window menu for material editor" 67 10
    Do-Click $proc $main "toggle Material Editor window" 96 56
    Do-Click $proc $main "material editor shading combo open" 145 142
    Do-Click $proc $main "material editor shading combo choose" 145 166
    Do-Click $proc $main "material editor texture slot combo open" 160 310
    Do-Click $proc $main "material editor texture slot combo choose" 160 334
    Do-Click $proc $main "content folder Levels" 279 634
    Do-Click $proc $main "content folder Models" 281 580
    Do-Click $proc $main "select first model asset" 420 580
    Do-DoubleClick $proc $main "double-click model preview" 430 580
    Do-Click $proc $main "content Place/Open" 372 548

    $sizes = @(
        @{ W = 1280; H = 720; Label = "resize 1280x720" },
        @{ W = 1280; H = 760; Label = "resize 1280x760" },
        @{ W = 1600; H = 900; Label = "resize 1600x900" },
        @{ W = 1920; H = 1080; Label = "resize 1920x1080" },
        @{ W = 1280; H = 720; Label = "resize back 1280x720" }
    )
    foreach ($size in $sizes) {
        Do-Resize $proc $main $size.W $size.H $size.Label
        Do-Click $proc $main ("viewport click after " + $size.Label) ([Math]::Max(420, [int]($size.W * 0.45))) ([Math]::Max(220, [int]($size.H * 0.38)))
        Do-Click $proc $main ("outliner click after " + $size.Label) ([Math]::Max(900, $size.W - 235)) 82
        Do-Click $proc $main ("details tab after " + $size.Label) ([Math]::Max(980, $size.W - 85)) 56
    }

    Log-Step "PASS imgui ui stability test"
    $script:HadFailure = $false
}
finally {
    Stop-TestProcess $proc $main
    if ($script:HadFailure -or $KeepLog) {
        Write-Host "UI smoke log kept at: $LogPath"
    } elseif (Test-Path -LiteralPath $LogPath) {
        Remove-Item -LiteralPath $LogPath -Force -ErrorAction SilentlyContinue
    }
}
