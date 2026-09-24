<#
.SYNOPSIS
  Capture the Ace Combat 5 window as a clean PNG, whatever is in front of it.

.DESCRIPTION
  Screenshotting this game is awkward: it has to be on screen to be captured, and
  anything in front of it -- including the terminal this runs in -- lands in the
  picture.  Asking the window to paint itself instead (PrintWindow) avoids that
  entirely: it renders the window into a bitmap without needing it visible, so the
  capture is of the game and nothing else.

  It does not modify the game or its settings.  Run it while the game is on the
  screen you want recorded.

.PARAMETER Out
  Where to write.  Defaults to work\shots\cap_<timestamp>.png.

.PARAMETER MaxWidth
  Downscale if wider than this, to keep the file a sensible size.  0 disables.

.PARAMETER Screen
  Capture from the screen instead of asking the window to paint itself.  Use this
  if PrintWindow returns a black or stale picture; it hides other windows, brings
  the game forward, and tells you if something still covered it.

.EXAMPLE
  # In a mission, sky on screen:
  pwsh -File tools\capture.ps1 -Out sky-on.png

.EXAMPLE
  # Same view, native sky off, for a side-by-side:
  pwsh -File tools\capture.ps1 -Out sky-off.png
#>
[CmdletBinding()]
param(
    [string]$Out,
    [int]$MaxWidth = 1280,
    [switch]$Screen,
    [string]$ProcessName = 'ac5'
)

$ErrorActionPreference = 'Stop'

# Win32 only.  The bitmap is made in PowerShell, because System.Drawing.Bitmap is
# not available to Add-Type here (it lives in System.Drawing.Common, which is not
# in the default reference set) -- mixing the two is what makes the usual
# "capture a window" snippets fail on PowerShell 7.
Add-Type @"
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Text;

public class Cap {
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr p);
    public delegate bool EnumProc(IntPtr h, IntPtr p);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
    [DllImport("user32.dll")] public static extern bool IsIconic(IntPtr h);
    [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int c);
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
    [DllImport("user32.dll")] public static extern bool BringWindowToTop(IntPtr h);
    [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr h, ref POINT p);
    [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr dc, uint flags);
    [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
    [DllImport("user32.dll")] public static extern int GetWindowTextLength(IntPtr h);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);

    public struct RECT { public int L, T, R, B; }
    public struct POINT { public int X, Y; }
    public const int SW_HIDE = 0, SW_SHOW = 5, SW_RESTORE = 9;

    // Ask the window to paint itself.  PW_RENDERFULLCONTENT (2) is what makes this
    // work for a window that uses a compositor or GPU surface; without it the
    // result is usually empty.  The caller supplies the device context, so no
    // drawing type is needed here.
    public static bool Paint(IntPtr h, IntPtr dc) { return PrintWindow(h, dc, 2); }

    public static int[] Size(IntPtr h, bool clientOnly) {
        RECT r;
        if (clientOnly) { if (!GetClientRect(h, out r)) return null; }
        else            { if (!GetWindowRect(h, out r)) return null; }
        return new int[] { r.R - r.L, r.B - r.T };
    }

    public static List<IntPtr> Others(IntPtr keep) {
        var list = new List<IntPtr>();
        EnumWindows((h, p) => {
            if (h == keep) return true;
            if (!IsWindowVisible(h)) return true;
            if (GetWindowTextLength(h) == 0) return true;
            list.Add(h);
            return true;
        }, IntPtr.Zero);
        return list;
    }
}
"@

Add-Type -AssemblyName System.Drawing

$procs = Get-Process -Name $ProcessName -ErrorAction SilentlyContinue |
         Where-Object { $_.MainWindowHandle -ne 0 }
if (-not $procs) {
    Write-Host "No '$ProcessName' window found."
    Write-Host "Start the game, get it to the screen you want to record, then run this"
    Write-Host "again without closing the game."
    exit 1
}
$game = $procs | Select-Object -First 1
$hwnd = $game.MainWindowHandle
Write-Host "game: pid $($game.Id)  hwnd $hwnd"

if (-not $Out) {
    $dir = Join-Path (Split-Path $PSScriptRoot -Parent) 'work\shots'
    New-Item -ItemType Directory -Force -Path $dir | Out-Null
    $Out = Join-Path $dir ("cap_{0:yyyyMMdd_HHmmss}.png" -f (Get-Date))
}
$Out = [System.IO.Path]::GetFullPath($Out)
New-Item -ItemType Directory -Force -Path (Split-Path $Out -Parent) | Out-Null

$bmp = $null
$method = ''

if (-not $Screen) {
    $sz = [Cap]::Size($hwnd, $false)
    if ($sz -and $sz[0] -gt 0 -and $sz[1] -gt 0) {
        $bmp = New-Object System.Drawing.Bitmap($sz[0], $sz[1])
        $g = [System.Drawing.Graphics]::FromImage($bmp)
        $dc = $g.GetHdc()
        try { $ok = [Cap]::Paint($hwnd, $dc) } finally { $g.ReleaseHdc($dc) }
        $g.Dispose()
        if ($ok) { $method = 'PrintWindow' }
        else { $bmp.Dispose(); $bmp = $null }
    }
    if (-not $bmp) { Write-Host "PrintWindow failed; falling back to capturing the screen" }
}

if (-not $bmp) {
    # Screen capture: everything else has to get out of the way first.
    $hidden = @()
    foreach ($h in [Cap]::Others($hwnd)) {
        if ([Cap]::IsIconic($h)) { continue }
        if ([Cap]::ShowWindow($h, [Cap]::SW_HIDE)) { $hidden += $h }
    }
    try {
        [Cap]::ShowWindow($hwnd, [Cap]::SW_RESTORE) | Out-Null
        [Cap]::BringWindowToTop($hwnd) | Out-Null
        [Cap]::SetForegroundWindow($hwnd) | Out-Null
        Start-Sleep -Milliseconds 1200
        $r = New-Object Cap+RECT
        [Cap]::GetWindowRect($hwnd, [ref]$r) | Out-Null
        $w = $r.R - $r.L; $ht = $r.B - $r.T
        if ($w -lt 64 -or $ht -lt 64) { throw "window is ${w}x${ht}, nothing to capture" }
        $bmp = New-Object System.Drawing.Bitmap($w, $ht)
        $g = [System.Drawing.Graphics]::FromImage($bmp)
        $g.CopyFromScreen($r.L, $r.T, 0, 0, (New-Object System.Drawing.Size($w, $ht)))
        $g.Dispose()
        $method = 'screen'
        $fg = [Cap]::GetForegroundWindow()
        if ($fg -ne $hwnd) {
            Write-Host "WARNING: something was still in front; check the picture."
        }
    }
    finally {
        foreach ($h in $hidden) { [Cap]::ShowWindow($h, [Cap]::SW_SHOW) | Out-Null }
        if ($hidden.Count) { Write-Host "restored $($hidden.Count) window(s)" }
    }
}

if (-not $bmp) { throw "capture produced nothing" }

if ($MaxWidth -gt 0 -and $bmp.Width -gt $MaxWidth) {
    $nh = [int]($bmp.Height * $MaxWidth / $bmp.Width)
    $small = New-Object System.Drawing.Bitmap($MaxWidth, $nh)
    $g = [System.Drawing.Graphics]::FromImage($small)
    $g.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
    $g.DrawImage($bmp, 0, 0, $MaxWidth, $nh)
    $g.Dispose(); $bmp.Dispose()
    $bmp = $small
}

$bmp.Save($Out, [System.Drawing.Imaging.ImageFormat]::Png)
Write-Host "wrote $Out   $($bmp.Width)x$($bmp.Height)   via $method"
$bmp.Dispose()
