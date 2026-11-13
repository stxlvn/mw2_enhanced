param([int]$Seconds = 120)
# QA: keeps the TEST game's window (iw4sp.exe under D:\coop-test) in front for $Seconds. The game fades its sound out while its window is not in front (snd_fade), so an audio check (loopback_rec.exe) needs it.
Add-Type @"
using System; using System.Runtime.InteropServices;
public class F {
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
  [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr h, IntPtr after, int x, int y, int cx, int cy, uint f);
  [DllImport("user32.dll")] public static extern void keybd_event(byte vk, byte scan, uint f, UIntPtr e);
}
"@
$end = (Get-Date).AddSeconds($Seconds)
$log = @()
while ((Get-Date) -lt $end) {
    $p = Get-Process iw4sp -ErrorAction SilentlyContinue | Where-Object { $_.Path -like "D:\coop-test\*" -and $_.MainWindowHandle -ne 0 } | Select-Object -First 1
    if ($p) {
        $h = $p.MainWindowHandle
        if ([F]::GetForegroundWindow() -ne $h) {
            # the foreground lock lets a process that just sent a key take the focus
            [F]::keybd_event(0x12, 0, 0, [UIntPtr]::Zero); [F]::keybd_event(0x12, 0, 2, [UIntPtr]::Zero)
            [F]::SetWindowPos($h, [IntPtr](-1), 0, 0, 0, 0, 0x43) | Out-Null   # TOPMOST, no move, no size, show
            [F]::SetForegroundWindow($h) | Out-Null
            [F]::SetWindowPos($h, [IntPtr](-2), 0, 0, 0, 0, 0x43) | Out-Null   # NOTOPMOST again
        }
    }
    Start-Sleep -Seconds 2
}
