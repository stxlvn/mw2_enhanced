# QA helper: simulates a client that loads the level slower than the host. Waits until the client copy's log shows the launch (the loading screen), freezes the
# client's game process for -Seconds, then lets it go on. Run it next to tools/qa_pair.sh (the client copy is <Root>\mw2-<Bits>-b).
# Usage: powershell -File tools\stall_client.ps1 [-Root D:\coop-test] [-Bits 64] [-Seconds 25] [-Marker "menus: loading screen"] [-Timeout 180]
param(
	[string]$Root = "D:\coop-test",
	[int]$Bits = 64,
	[int]$Seconds = 25,
	[string]$Marker = "loading screen: the campaign",
	[int]$Timeout = 180
)
Add-Type -Namespace Native -Name Nt -MemberDefinition @'
[System.Runtime.InteropServices.DllImport("ntdll.dll")] public static extern int NtSuspendProcess(System.IntPtr h);
[System.Runtime.InteropServices.DllImport("ntdll.dll")] public static extern int NtResumeProcess(System.IntPtr h);
'@
$copy = Join-Path $Root "mw2-$Bits-b"
$log = Join-Path $copy "mw2coop\mw2coop.log"
$exe = Join-Path $copy "iw4sp.exe"
$t0 = Get-Date
while (((Get-Date) - $t0).TotalSeconds -lt $Timeout) {
	Start-Sleep -Milliseconds 100
	if (Test-Path $log) {
		$text = try { $fs = New-Object System.IO.FileStream($log, [System.IO.FileMode]::Open, [System.IO.FileAccess]::Read, [System.IO.FileShare]::ReadWrite); $sr = New-Object System.IO.StreamReader($fs); $t = $sr.ReadToEnd(); $sr.Close(); $t } catch { "" }
		if ($text.Contains($Marker)) { break }
	}
}
# the game writes its process id to <copy>\__iw4sp as a 4 byte little-endian number (run_test.ps1 relies on it too)
$pidFile = Join-Path $copy "__iw4sp"
$p = $null
if (Test-Path $pidFile) {
	try {
		$bytes = [System.IO.File]::ReadAllBytes($pidFile)
		if ($bytes.Length -ge 4) { $id = [System.BitConverter]::ToInt32($bytes, 0); if ($id -gt 0) { $p = Get-Process -Id $id -ErrorAction SilentlyContinue } }
	} catch { }
}
if (-not $p) { Write-Output "stall_client: no client process for $exe"; exit 1 }
Write-Output ("stall_client: freezing pid {0} for {1} s at {2:HH:mm:ss.fff}" -f $p.Id, $Seconds, (Get-Date))
[void][Native.Nt]::NtSuspendProcess($p.Handle)
Start-Sleep -Seconds $Seconds
[void][Native.Nt]::NtResumeProcess($p.Handle)
Write-Output ("stall_client: resumed at {0:HH:mm:ss.fff}" -f (Get-Date))
