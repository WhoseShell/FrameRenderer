param(
    [string]$ExePath = (Join-Path (Split-Path $PSScriptRoot -Parent) "build_vs\Release\dx12_hello.exe"),
    [int]$TimeoutMs = 1800,
    [string]$LogPath = (Join-Path $PSScriptRoot "ui_smoke_test.log")
)

$ErrorActionPreference = "Stop"

Add-Type @"
using System;
using System.Runtime.InteropServices;
using System.Text;

public static class Win32Smoke {
    public delegate bool EnumWindowsProc(IntPtr hWnd, IntPtr lParam);

    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumWindowsProc lpEnumFunc, IntPtr lParam);
    [DllImport("user32.dll")] public static extern bool EnumChildWindows(IntPtr hWndParent, EnumWindowsProc lpEnumFunc, IntPtr lParam);
    [DllImport("user32.dll")] public static extern int GetWindowTextW(IntPtr hWnd, StringBuilder lpString, int nMaxCount);
    [DllImport("user32.dll")] public static extern int GetClassNameW(IntPtr hWnd, StringBuilder lpClassName, int nMaxCount);
    [DllImport("user32.dll")] public static extern int GetDlgCtrlID(IntPtr hwndCtl);
    [DllImport("user32.dll")] public static extern IntPtr GetParent(IntPtr hWnd);
    [DllImport("user32.dll")] public static extern bool IsWindow(IntPtr hWnd);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr hWnd);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr hWnd, out uint lpdwProcessId);
    [DllImport("user32.dll")] public static extern bool PostMessageW(IntPtr hWnd, uint Msg, IntPtr wParam, IntPtr lParam);
    [DllImport("user32.dll")] public static extern IntPtr SendMessageW(IntPtr hWnd, uint Msg, IntPtr wParam, IntPtr lParam);
    [DllImport("user32.dll")] public static extern IntPtr SendMessageTimeoutW(IntPtr hWnd, uint Msg, IntPtr wParam, IntPtr lParam, uint fuFlags, uint uTimeout, out IntPtr lpdwResult);
    [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr hWnd, IntPtr hWndInsertAfter, int X, int Y, int cx, int cy, uint uFlags);
}
"@

$WM_NULL = 0x0000
$WM_GETTEXTLENGTH = 0x000E
$WM_CLOSE = 0x0010
$WM_COMMAND = 0x0111
$WM_HSCROLL = 0x0114
$BM_CLICK = 0x00F5
$LB_SETCURSEL = 0x0186
$LB_GETCOUNT = 0x018B
$CB_SETCURSEL = 0x014E
$CB_GETCOUNT = 0x0146
$TBM_SETPOS = 0x0405
$BN_CLICKED = 0
$LBN_SELCHANGE = 1
$LBN_DBLCLK = 2
$CBN_SELCHANGE = 1
$SMTO_ABORTIFHUNG = 0x0002
$SWP_NOZORDER = 0x0004
$SWP_NOACTIVATE = 0x0010

if (Test-Path $LogPath) {
    Remove-Item -LiteralPath $LogPath -Force
}

function Log-Step([string]$message) {
    $line = ("{0:HH:mm:ss.fff} {1}" -f (Get-Date), $message)
    Add-Content -LiteralPath $LogPath -Value $line
    Write-Host $line
}

function Make-WParam([int]$id, [int]$code) {
    return [IntPtr]((($code -band 0xffff) -shl 16) -bor ($id -band 0xffff))
}

function Get-WindowText([IntPtr]$hwnd) {
    $sb = [Text.StringBuilder]::new(512)
    [void][Win32Smoke]::GetWindowTextW($hwnd, $sb, $sb.Capacity)
    $sb.ToString()
}

function Get-ClassName([IntPtr]$hwnd) {
    $sb = [Text.StringBuilder]::new(256)
    [void][Win32Smoke]::GetClassNameW($hwnd, $sb, $sb.Capacity)
    $sb.ToString()
}

function Get-TopWindowsForPid([int]$targetPid) {
    $result = New-Object System.Collections.Generic.List[IntPtr]
    $cb = [Win32Smoke+EnumWindowsProc]{
        param([IntPtr]$h, [IntPtr]$l)
        [uint32]$windowPid = 0
        [void][Win32Smoke]::GetWindowThreadProcessId($h, [ref]$windowPid)
        if ($windowPid -eq $targetPid -and [Win32Smoke]::IsWindowVisible($h)) {
            $script:topWindowResult.Add($h)
        }
        return $true
    }
    $script:topWindowResult = $result
    [void][Win32Smoke]::EnumWindows($cb, [IntPtr]::Zero)
    $script:topWindowResult = $null
    return @($result)
}

function Get-ChildWindows([IntPtr]$root) {
    $result = New-Object System.Collections.Generic.List[IntPtr]
    $cb = [Win32Smoke+EnumWindowsProc]{
        param([IntPtr]$h, [IntPtr]$l)
        $script:childWindowResult.Add($h)
        return $true
    }
    $script:childWindowResult = $result
    [void][Win32Smoke]::EnumChildWindows($root, $cb, [IntPtr]::Zero)
    $script:childWindowResult = $null
    return @($result)
}

function Find-ChildById([IntPtr]$root, [int]$id) {
    $scopes = New-Object System.Collections.Generic.List[IntPtr]
    $scopes.Add($root)
    if ($script:SmokePid) {
        foreach ($top in Get-TopWindowsForPid $script:SmokePid) {
            if ($top -ne $root) { $scopes.Add($top) }
        }
    }
    foreach ($scope in $scopes) {
        if ([Win32Smoke]::GetDlgCtrlID($scope) -eq $id) {
            return $scope
        }
        foreach ($h in Get-ChildWindows $scope) {
            if ([Win32Smoke]::GetDlgCtrlID($h) -eq $id) {
                return $h
            }
        }
    }
    return [IntPtr]::Zero
}

function Find-VisibleChildById([IntPtr]$root, [int]$id) {
    $scopes = New-Object System.Collections.Generic.List[IntPtr]
    $scopes.Add($root)
    if ($script:SmokePid) {
        foreach ($top in Get-TopWindowsForPid $script:SmokePid) {
            if ($top -ne $root) { $scopes.Add($top) }
        }
    }
    foreach ($scope in $scopes) {
        if ([Win32Smoke]::GetDlgCtrlID($scope) -eq $id -and [Win32Smoke]::IsWindowVisible($scope)) {
            return $scope
        }
        foreach ($h in Get-ChildWindows $scope) {
            if ([Win32Smoke]::GetDlgCtrlID($h) -eq $id -and [Win32Smoke]::IsWindowVisible($h)) {
                return $h
            }
        }
    }
    return [IntPtr]::Zero
}

function Find-ChildByText([IntPtr]$root, [string]$text) {
    $scopes = New-Object System.Collections.Generic.List[IntPtr]
    $scopes.Add($root)
    if ($script:SmokePid) {
        foreach ($top in Get-TopWindowsForPid $script:SmokePid) {
            if ($top -ne $root) { $scopes.Add($top) }
        }
    }
    foreach ($scope in $scopes) {
        foreach ($h in Get-ChildWindows $scope) {
            if ([Win32Smoke]::IsWindowVisible($h) -and (Get-WindowText $h) -eq $text) {
                return $h
            }
        }
    }
    return [IntPtr]::Zero
}

function Find-ChildByClassAndText([IntPtr]$root, [string]$class, [string]$text) {
    $scopes = New-Object System.Collections.Generic.List[IntPtr]
    $scopes.Add($root)
    if ($script:SmokePid) {
        foreach ($top in Get-TopWindowsForPid $script:SmokePid) {
            if ($top -ne $root) { $scopes.Add($top) }
        }
    }
    foreach ($scope in $scopes) {
        foreach ($h in Get-ChildWindows $scope) {
            if ([Win32Smoke]::IsWindowVisible($h) -and (Get-ClassName $h) -eq $class -and (Get-WindowText $h) -eq $text) {
                return $h
            }
        }
    }
    return [IntPtr]::Zero
}

function Find-ChildByIdInRootOnly([IntPtr]$root, [int]$id) {
    foreach ($h in Get-ChildWindows $root) {
        if ([Win32Smoke]::GetDlgCtrlID($h) -eq $id) {
            return $h
        }
    }
    return [IntPtr]::Zero
}

function Find-MainWindow($proc) {
    for ($i = 0; $i -lt 80; ++$i) {
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

function Assert-Responsive($proc, [IntPtr]$main, [string]$label) {
    $proc.Refresh()
    if ($proc.HasExited) { throw "process exited after: $label" }
    if (-not $proc.Responding) { throw "window not responding after: $label" }
    Log-Step ("OK  " + $label)
}

function Wait-Responsive($proc, [IntPtr]$main, [string]$label, [int]$maxWaitMs = 20000) {
    $deadline = [Environment]::TickCount64 + $maxWaitMs
    while ([Environment]::TickCount64 -lt $deadline) {
        $proc.Refresh()
        if ($proc.HasExited) { throw "process exited while waiting for: $label" }
        if ($proc.Responding) {
            Log-Step ("OK  " + $label)
            return
        }
        Start-Sleep -Milliseconds 250
    }
    throw "window not responding after waiting for: $label"
}

function Send-MessageChecked([IntPtr]$hwnd, [uint32]$msg, [IntPtr]$wParam, [IntPtr]$lParam, [string]$label, [int]$timeoutMs = 1000) {
    [IntPtr]$result = [IntPtr]::Zero
    $ok = [Win32Smoke]::SendMessageTimeoutW($hwnd, $msg, $wParam, $lParam, $SMTO_ABORTIFHUNG, [uint32]$timeoutMs, [ref]$result)
    if ($ok -eq [IntPtr]::Zero) {
        throw "timed out sending message for: $label"
    }
    return $result
}

function Close-CommonDialogs($proc, [IntPtr]$main) {
    foreach ($h in Get-TopWindowsForPid $proc.Id) {
        if ($h -eq $main) { continue }
        $class = Get-ClassName $h
        $title = Get-WindowText $h
        if ($class -eq "#32770" -or $title -match "Open|Save|Color|Import") {
            [void][Win32Smoke]::PostMessageW($h, $WM_CLOSE, [IntPtr]::Zero, [IntPtr]::Zero)
        }
    }
}

function After-Action($proc, [IntPtr]$main, [string]$label) {
    Start-Sleep -Milliseconds 450
    Close-CommonDialogs $proc $main
    Start-Sleep -Milliseconds 250
    Wait-Responsive $proc $main $label 8000
}

function Click-Button($proc, [IntPtr]$main, [int]$id, [string]$label) {
    Log-Step ("DO  " + $label)
    $h = Find-ChildById $main $id
    if ($h -eq [IntPtr]::Zero) { Log-Step ("SKIP missing " + $label); return }
    [void][Win32Smoke]::PostMessageW($h, $BM_CLICK, [IntPtr]::Zero, [IntPtr]::Zero)
    After-Action $proc $main $label
}

function Send-ListEvent($proc, [IntPtr]$main, [int]$id, [int]$index, [int]$eventCode, [string]$label) {
    Log-Step ("DO  " + $label)
    $h = Find-ChildById $main $id
    if ($h -eq [IntPtr]::Zero) { Log-Step ("SKIP missing " + $label); return }
    [void](Send-MessageChecked $h $LB_SETCURSEL ([IntPtr]$index) ([IntPtr]::Zero) $label)
    $parent = [Win32Smoke]::GetParent($h)
    if ($parent -eq [IntPtr]::Zero) { $parent = $main }
    [void][Win32Smoke]::PostMessageW($parent, $WM_COMMAND, (Make-WParam $id $eventCode), $h)
    After-Action $proc $main $label
}

function Select-Combo($proc, [IntPtr]$main, [int]$id, [int]$index, [string]$label) {
    Log-Step ("DO  " + $label)
    $h = Find-ChildById $main $id
    if ($h -eq [IntPtr]::Zero) { Log-Step ("SKIP missing " + $label); return }
    [void](Send-MessageChecked $h $CB_SETCURSEL ([IntPtr]$index) ([IntPtr]::Zero) $label)
    $parent = [Win32Smoke]::GetParent($h)
    if ($parent -eq [IntPtr]::Zero) { $parent = $main }
    [void][Win32Smoke]::PostMessageW($parent, $WM_COMMAND, (Make-WParam $id $CBN_SELCHANGE), $h)
    After-Action $proc $main $label
}

function Move-Trackbar($proc, [IntPtr]$main, [int]$id, [int]$position, [string]$label) {
    Log-Step ("DO  " + $label)
    $h = Find-ChildById $main $id
    if ($h -eq [IntPtr]::Zero) { Log-Step ("SKIP missing " + $label); return }
    [void](Send-MessageChecked $h $TBM_SETPOS ([IntPtr]1) ([IntPtr]$position) $label)
    $parent = [Win32Smoke]::GetParent($h)
    if ($parent -eq [IntPtr]::Zero) { $parent = $main }
    [void][Win32Smoke]::PostMessageW($parent, $WM_HSCROLL, [IntPtr]::Zero, $h)
    After-Action $proc $main $label
}

function Count-ListItems([IntPtr]$main, [int]$id) {
    $h = Find-ChildById $main $id
    if ($h -eq [IntPtr]::Zero) { return 0 }
    return [int](Send-MessageChecked $h $LB_GETCOUNT ([IntPtr]::Zero) ([IntPtr]::Zero) ("count list " + $id))
}

function Count-ComboItems([IntPtr]$main, [int]$id) {
    $h = Find-ChildById $main $id
    if ($h -eq [IntPtr]::Zero) { return 0 }
    return [int](Send-MessageChecked $h $CB_GETCOUNT ([IntPtr]::Zero) ([IntPtr]::Zero) ("count combo " + $id))
}

if (!(Test-Path $ExePath)) {
    throw "missing executable: $ExePath"
}

$proc = Start-Process -FilePath $ExePath -WorkingDirectory (Split-Path $ExePath -Parent) -PassThru
try {
    $script:SmokePid = $proc.Id
    $main = Find-MainWindow $proc
    Wait-Responsive $proc $main "startup"

    [void][Win32Smoke]::SetWindowPos($main, [IntPtr]::Zero, 80, 60, 1280, 720, $SWP_NOZORDER -bor $SWP_NOACTIVATE)
    After-Action $proc $main "resize 1280x720"
    [void][Win32Smoke]::SetWindowPos($main, [IntPtr]::Zero, 60, 40, 1760, 980, $SWP_NOZORDER -bor $SWP_NOACTIVATE)
    After-Action $proc $main "resize 1760x980"

    Click-Button $proc $main 5016 "Place Actors collapse"
    Click-Button $proc $main 5016 "Place Actors expand"
    Click-Button $proc $main 5015 "Content Drawer collapse"
    Click-Button $proc $main 5015 "Content Drawer expand"

    Click-Button $proc $main 5013 "open Settings"
    Select-Combo $proc $main 3000 1 "Render Path Deferred"
    Select-Combo $proc $main 3000 0 "Render Path Forward"
    Click-Button $proc $main 3009 "Lumen checkbox"
    Click-Button $proc $main 3011 "SWRT checkbox"
    Click-Button $proc $main 3010 "HWRT checkbox"
    Click-Button $proc $main 2005 "Tonemap checkbox"

    foreach ($folder in 0..3) {
        Send-ListEvent $proc $main 5012 $folder $LBN_SELCHANGE ("content folder " + $folder)
        $count = Count-ListItems $main 5001
        if ($count -gt 0) {
            Send-ListEvent $proc $main 5001 0 $LBN_DBLCLK ("content double-click folder " + $folder)
        }
    }

    Send-ListEvent $proc $main 5012 0 $LBN_SELCHANGE "select Models"
    if ((Count-ListItems $main 5001) -gt 0) {
        Send-ListEvent $proc $main 5001 0 $LBN_SELCHANGE "select first model"
        Click-Button $proc $main 5003 "bottom Place selected model"
        Click-Button $proc $main 5011 "toolbar Place selected model"
    }

    Send-ListEvent $proc $main 5012 1 $LBN_SELCHANGE "select Textures"
    if ((Count-ListItems $main 5001) -gt 0) {
        Send-ListEvent $proc $main 5001 0 $LBN_DBLCLK "double-click first texture"
    }
    Click-Button $proc $main 2004 "New Material"

    Send-ListEvent $proc $main 5012 2 $LBN_SELCHANGE "select Materials"
    if ((Count-ListItems $main 5001) -gt 0) {
        Send-ListEvent $proc $main 5001 0 $LBN_DBLCLK "double-click first material"
    }
    if ((Count-ListItems $main 2003) -gt 0) {
        Send-ListEvent $proc $main 2003 0 $LBN_DBLCLK "double-click loaded material"
    }

    $main = Find-MainWindow $proc
    $materialModes = Count-ComboItems $main 4100
    if ($materialModes -gt 0) {
        for ($i = 0; $i -lt [Math]::Min($materialModes, 3); ++$i) {
            Select-Combo $proc $main 4100 $i ("material shading mode " + $i)
        }
        Move-Trackbar $proc $main 4103 2500 "material slider A"
        Move-Trackbar $proc $main 4104 5000 "material slider B"
        foreach ($comboId in 4105,4106,4107,4108,4109) {
            $n = Count-ComboItems $main $comboId
            if ($n -gt 1) { Select-Combo $proc $main $comboId 1 ("material texture combo " + $comboId) }
        }
        Click-Button $proc $main 4102 "material Apply"
        Click-Button $proc $main 4101 "material Color dialog"
    }

    $outlinerCount = Count-ListItems $main 5101
    for ($i = 0; $i -lt [Math]::Min($outlinerCount, 6); ++$i) {
        Send-ListEvent $proc $main 5101 $i $LBN_SELCHANGE ("outliner select " + $i)
        Send-ListEvent $proc $main 5101 $i $LBN_DBLCLK ("outliner focus " + $i)
        Click-Button $proc $main 5201 ("apply details " + $i)
    }

    Click-Button $proc $main 5004 "bottom New Level"
    Click-Button $proc $main 5006 "bottom Save Level dialog"
    Click-Button $proc $main 5005 "bottom Open Level dialog"
    Click-Button $proc $main 5002 "bottom Import OBJ dialog"
    Click-Button $proc $main 5007 "toolbar New Level"
    Click-Button $proc $main 5009 "toolbar Save Level dialog"
    Click-Button $proc $main 5008 "toolbar Open Level dialog"
    Click-Button $proc $main 5010 "toolbar Import OBJ dialog"

    Assert-Responsive $proc $main "final"
    Log-Step "UI smoke test completed."
}
finally {
    if ($proc -and -not $proc.HasExited) {
        Stop-Process -Id $proc.Id -Force -ErrorAction SilentlyContinue
    }
}
