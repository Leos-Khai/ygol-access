# Launches the game, optionally presses keys, saves a screenshot, prints the mod log, and closes the game.
# Usage: powershell -File tools/gametest.ps1 [-Wait 40] [-WaitFor KAZUKI] [-Keys F5,RETURN] [-KeyDelay 3] [-Shot out.png] [-KeepOpen]
# -WaitFor KAZUKI waits for the title screen (its copyright line), so the first key is always the title's.
param([int]$Wait = 40, [string]$WaitFor = "", [string[]]$Keys = @(), [int]$KeyDelay = 3, [string]$Shot = "", [switch]$KeepOpen)
$ErrorActionPreference = "Stop"
$game = "D:\SteamLibrary\steamapps\common\Yu-Gi-Oh! Legacy of the Duelist Link Evolution"
Add-Type -AssemblyName System.Windows.Forms, System.Drawing
Add-Type @"
using System; using System.Runtime.InteropServices;
public static class K {
  [DllImport("user32.dll")] public static extern void keybd_event(byte vk, byte scan, uint flags, UIntPtr extra);
  [DllImport("user32.dll")] public static extern uint MapVirtualKey(uint code, uint type);
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
  [DllImport("user32.dll")] public static extern void mouse_event(uint f, int x, int y, uint d, UIntPtr e);
  // Clicks the window's title bar: a real click is the one reliable way to give it focus.
  public static void Focus(IntPtr h) { RECT r; GetWindowRect(h, out r); SetCursorPos(r.L + 150, r.T + 12); mouse_event(2, 0, 0, 0, UIntPtr.Zero); mouse_event(4, 0, 0, 0, UIntPtr.Zero); }
  // Arrows, Home/End, PageUp/Down, Insert/Delete are extended keys: without the flag they arrive as numpad keys.
  public static void Press(byte vk) {
    uint ext = (vk >= 0x21 && vk <= 0x28) || vk == 0x2D || vk == 0x2E ? 1u : 0u;
    byte s = (byte)MapVirtualKey(vk, 0);
    keybd_event(vk, s, ext, UIntPtr.Zero); System.Threading.Thread.Sleep(80); keybd_event(vk, s, ext | 2, UIntPtr.Zero);
  }
}
"@

$launched = Get-Date
if (-not (Get-Process YuGiOh -ErrorAction SilentlyContinue)) { Start-Process "steam://rungameid/1150640" }
if ($WaitFor) {
  # Wait until this launch's mod log shows this text (e.g. "KAZUKI" = title screen), up to $Wait seconds, then let
  # it settle. The log from the previous run has the same text, so only a log written after launch counts.
  $deadline = (Get-Date).AddSeconds($Wait)
  $log = "$game\ygol-access.log"
  while ((Get-Date) -lt $deadline) {
    $fresh = (Get-Process YuGiOh -ErrorAction SilentlyContinue) -and (Test-Path $log) -and ((Get-Item $log).LastWriteTime -gt $launched)
    if ($fresh -and (Select-String -Path $log -Pattern $WaitFor -Quiet)) { break }
    Start-Sleep 1
  }
  Start-Sleep 2
} else {
  Start-Sleep $Wait
}
$p = Get-Process YuGiOh
[K]::Focus($p.MainWindowHandle)
foreach ($k in ($Keys -join ",").Split(",", [StringSplitOptions]::RemoveEmptyEntries)) {  # -File passes "A,B" as one string
  Start-Sleep $KeyDelay
  Write-Output ("{0:HH:mm:ss.fff} press {1}" -f (Get-Date), $k)
  [K]::Press([byte][System.Windows.Forms.Keys]::$k)
}
Start-Sleep 2
if ($Shot) {
  $b = [System.Windows.Forms.Screen]::PrimaryScreen.Bounds
  $bmp = New-Object System.Drawing.Bitmap $b.Width, $b.Height
  [System.Drawing.Graphics]::FromImage($bmp).CopyFromScreen($b.Location, [System.Drawing.Point]::Empty, $b.Size)
  $bmp.Save($Shot)
}
if (-not $KeepOpen) { Stop-Process -Name YuGiOh -Confirm:$false }
Get-Content "$game\ygol-access.log"
