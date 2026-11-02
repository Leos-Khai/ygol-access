# Sends keys to the running game (started with gametest.ps1 -KeepOpen) and prints the log lines added meanwhile.
# Usage: powershell -File tools/gamekeys.ps1 -Keys RETURN,F5 [-KeyDelay 2] [-Settle 2] [-Filter "speak|duel"] [-Click]
# -Click: click the title bar even when the game is in front (it can stop taking keys until clicked).
param([string[]]$Keys = @(), [int]$KeyDelay = 2, [int]$Settle = 2, [string]$Filter = "", [switch]$Click)
$ErrorActionPreference = "Stop"
$log = "D:\SteamLibrary\steamapps\common\Yu-Gi-Oh! Legacy of the Duelist Link Evolution\ygol-access.log"
Add-Type -AssemblyName System.Windows.Forms
Add-Type @"
using System; using System.Runtime.InteropServices;
public static class K2 {
  [DllImport("user32.dll")] public static extern void keybd_event(byte vk, byte scan, uint flags, UIntPtr extra);
  [DllImport("user32.dll")] public static extern uint MapVirtualKey(uint code, uint type);
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
  [DllImport("user32.dll")] public static extern void mouse_event(uint f, int x, int y, uint d, UIntPtr e);
  [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
  public static void Focus(IntPtr h, bool always) { if (!always && GetForegroundWindow() == h) return; RECT r; GetWindowRect(h, out r); SetCursorPos(r.L + 150, r.T + 12); mouse_event(2, 0, 0, 0, UIntPtr.Zero); mouse_event(4, 0, 0, 0, UIntPtr.Zero); }
  public static void Press(byte vk) {
    uint ext = (vk >= 0x21 && vk <= 0x28) || vk == 0x2D || vk == 0x2E ? 1u : 0u;
    byte s = (byte)MapVirtualKey(vk, 0);
    keybd_event(vk, s, ext, UIntPtr.Zero); System.Threading.Thread.Sleep(80); keybd_event(vk, s, ext | 2, UIntPtr.Zero);
  }
}
"@
$p = Get-Process YuGiOh
$before = (Get-Content $log).Count
[K2]::Focus($p.MainWindowHandle, [bool]$Click)
foreach ($k in ($Keys -join ",").Split(",", [StringSplitOptions]::RemoveEmptyEntries)) {
  Start-Sleep $KeyDelay
  Write-Output "-- press $k"
  [K2]::Press([byte][System.Windows.Forms.Keys]::$k)
}
Start-Sleep $Settle
$lines = Get-Content $log | Select-Object -Skip $before
if ($Filter) { $lines = $lines | Where-Object { $_ -match $Filter } }
$lines | ForEach-Object { $_.Substring(0, [Math]::Min(260, $_.Length)) }
