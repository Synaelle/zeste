# Regression check for --headless: lists the zeste.exe windows Windows considers visible while a
# headless run is in progress. Expected output: only "exit=0".
param([string]$Exe = (Join-Path $PSScriptRoot "../../../build/bin/Release/zeste.exe"),
      [string]$Rom = (Join-Path $PSScriptRoot "../testrom/zeste-tests.3dsx"))
$Exe = (Resolve-Path $Exe).Path
$Rom = (Resolve-Path $Rom).Path
Add-Type @"
using System; using System.Text; using System.Runtime.InteropServices; using System.Collections.Generic;
public class W {
  public delegate bool EnumProc(IntPtr h, IntPtr p);
  [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr p);
  [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
  [DllImport("user32.dll")] public static extern int GetWindowText(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  public struct RECT { public int L,T,R,B; }
  public static List<string> List(uint target) {
    var res = new List<string>();
    EnumWindows((h,p)=>{ uint pid; GetWindowThreadProcessId(h, out pid);
      if (pid==target && IsWindowVisible(h)) { var sb=new StringBuilder(256); GetWindowText(h,sb,256); RECT r; GetWindowRect(h,out r);
        res.Add(sb.ToString()+" ["+r.L+","+r.T+" "+(r.R-r.L)+"x"+(r.B-r.T)+"]"); }
      return true; }, IntPtr.Zero);
    return res;
  }
}
"@
$p = Start-Process -FilePath $Exe -ArgumentList '--headless','--quit-after','400',$Rom -PassThru
for ($i=0; $i -lt 40; $i++) { Start-Sleep -Milliseconds 100; if ($p.HasExited) { break }; $l=[W]::List([uint32]$p.Id); if ($l.Count -gt 0) { "t=$($i*100)ms visible: $($l -join ' | ')" } }
if (!$p.HasExited) { $p.WaitForExit(20000) | Out-Null }; "exit=$($p.ExitCode)"
