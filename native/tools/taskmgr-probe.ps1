# Task Manager glass probe (parked experiment, not part of the app). Task Manager runs elevated (High integrity), so
# GlassForge can't touch it; this script must run elevated itself:
#   Start-Process powershell -Verb RunAs -ArgumentList '-NoProfile -ExecutionPolicy Bypass -File <this file>'
# It sets Acrylic + whole-window margins, turns the Details ListView background black, and answers every focus change
# with WM_NCACTIVATE(TRUE) from a WinEvent hook. Create the stop file to end it; it restores Mica, margins and colors.
$dir = Join-Path $env:LOCALAPPDATA 'GlassForge'
New-Item -ItemType Directory -Force $dir | Out-Null
$log = Join-Path $dir 'taskmgr-probe.log'
$stop = Join-Path $dir 'taskmgr-probe.stop'
Remove-Item $stop -ErrorAction SilentlyContinue
try {
Add-Type @'
using System;using System.IO;using System.Runtime.InteropServices;using System.Text;using System.Collections.Generic;
public static class Probe{
[StructLayout(LayoutKind.Sequential)]struct M{public int l,r,t,b;}
[StructLayout(LayoutKind.Sequential)]struct MSG{public IntPtr h;public uint m;public IntPtr w,l;public uint t;public int x,y;}
[DllImport("dwmapi")]static extern int DwmSetWindowAttribute(IntPtr h,int a,ref int v,int s);
[DllImport("dwmapi")]static extern int DwmGetWindowAttribute(IntPtr h,int a,out int v,int s);
[DllImport("dwmapi")]static extern int DwmExtendFrameIntoClientArea(IntPtr h,ref M m);
[DllImport("user32")]static extern bool IsWindow(IntPtr h);
[DllImport("user32")]static extern bool PostMessage(IntPtr h,uint m,IntPtr w,IntPtr l);
[DllImport("user32")]static extern IntPtr SendMessageTimeout(IntPtr h,uint m,IntPtr w,IntPtr l,uint f,uint t,out IntPtr r);
[DllImport("user32")]static extern bool InvalidateRect(IntPtr h,IntPtr r,bool e);
delegate bool EP(IntPtr h,IntPtr l);
[DllImport("user32")]static extern bool EnumChildWindows(IntPtr p,EP f,IntPtr l);
[DllImport("user32")]static extern int GetClassName(IntPtr h,StringBuilder s,int n);
delegate void WEP(IntPtr hook,uint ev,IntPtr hwnd,int obj,int child,uint thread,uint time);
[DllImport("user32")]static extern IntPtr SetWinEventHook(uint min,uint max,IntPtr mod,WEP p,uint pid,uint tid,uint flags);
[DllImport("user32")]static extern bool UnhookWinEvent(IntPtr h);
[DllImport("user32")]static extern IntPtr SetTimer(IntPtr h,IntPtr id,uint ms,IntPtr p);
[DllImport("user32")]static extern int GetMessage(out MSG m,IntPtr h,uint a,uint b);
[DllImport("user32")]static extern bool TranslateMessage(ref MSG m);
[DllImport("user32")]static extern IntPtr DispatchMessage(ref MSG m);
static IntPtr H; static string Log; static int resets;
static Dictionary<IntPtr,long[]> lvs=new Dictionary<IntPtr,long[]>();
static WEP keep;
static void L(string s){File.AppendAllText(Log,DateTime.Now.ToString("HH:mm:ss.fff ")+s+"\r\n");}
static long Send(IntPtr h,uint m,long w,long l){IntPtr r;SendMessageTimeout(h,m,new IntPtr(w),new IntPtr(l),2,1000,out r);return r.ToInt64();}
static void Glass(){int cur;DwmGetWindowAttribute(H,38,out cur,4);if(cur!=3){if(cur!=0)resets++;int v=3;DwmSetWindowAttribute(H,38,ref v,4);}M m=new M{l=-1,r=-1,t=-1,b=-1};DwmExtendFrameIntoClientArea(H,ref m);}
static void Lists(){var found=new List<IntPtr>();EnumChildWindows(H,(c,l)=>{var s=new StringBuilder(64);GetClassName(c,s,64);if(s.ToString()=="SysListView32")found.Add(c);return true;},IntPtr.Zero);
 foreach(var lv in found){long bk=Send(lv,0x1000,0,0),tbk=Send(lv,0x1025,0,0);if(!lvs.ContainsKey(lv))lvs[lv]=new long[]{bk,tbk};if(bk!=0){Send(lv,0x1001,0,0);InvalidateRect(lv,IntPtr.Zero,true);}}}
public static void Run(IntPtr h,string log,string stop){
 H=h;Log=log;int orig;DwmGetWindowAttribute(H,38,out orig,4);L("probe started, original backdrop "+orig);
 Glass();Lists();
 keep=(hook,ev,hwnd,obj,child,thread,time)=>{
  if(ev==3){ if(hwnd!=H){PostMessage(H,0x86,new IntPtr(1),IntPtr.Zero);} Glass(); }
 };
 IntPtr hk=SetWinEventHook(3,3,IntPtr.Zero,keep,0,0,0);
 SetTimer(IntPtr.Zero,IntPtr.Zero,500,IntPtr.Zero);
 DateTime end=DateTime.Now.AddMinutes(30);MSG msg;
 while(GetMessage(out msg,IntPtr.Zero,0,0)>0){
  if(msg.m==0x113){ if(File.Exists(stop)||!IsWindow(H)||DateTime.Now>end)break; Glass(); Lists(); }
  TranslateMessage(ref msg);DispatchMessage(ref msg);
 }
 UnhookWinEvent(hk);
 if(IsWindow(H)){foreach(var kv in lvs)if(IsWindow(kv.Key)){Send(kv.Key,0x1001,0,kv.Value[0]);Send(kv.Key,0x1026,0,kv.Value[1]);InvalidateRect(kv.Key,IntPtr.Zero,true);}
  M z=new M();DwmExtendFrameIntoClientArea(H,ref z);DwmSetWindowAttribute(H,38,ref orig,4);L("restored");}
 L("probe ended, backdrop resets: "+resets);
}}
'@
$h = [IntPtr](Get-Process Taskmgr | Select-Object -First 1).MainWindowHandle
[Probe]::Run($h, $log, $stop)
} catch { Add-Content $log ("ERROR " + $_) }
