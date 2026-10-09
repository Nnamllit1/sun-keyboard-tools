param([string]$Setup, [string]$BuildDirectory = 'build')
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$version = (Get-Content (Join-Path $root 'VERSION') -Raw).Trim()
if (!$Setup) { $Setup = Join-Path $root "out\SunKeyboardTools-Setup-$version-x64.exe" }
if (![IO.Path]::IsPathRooted($BuildDirectory)) { $BuildDirectory = Join-Path $root $BuildDirectory }
$destination = Join-Path $root ('.local\package-smoke-' + [Guid]::NewGuid().ToString('N'))
$admin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
$process = Start-Process -FilePath $Setup -ArgumentList "--extract `"$destination`"" -PassThru -WindowStyle Hidden
if (!$process.WaitForExit(30000)) { $process.Kill(); throw 'Payload extraction timed out' }
if ($admin) {
    if ($process.ExitCode -eq 0 -or (Test-Path -LiteralPath $destination)) { throw 'Elevated payload extraction was allowed' }
    # Hosted CI runs elevated without a linked UAC token. Inspect PE resources as
    # inert data there, and verify the elevated extraction rejection above.
    Add-Type @'
using System;
using System.Runtime.InteropServices;
public static class SunPayload {
 [DllImport("kernel32", CharSet=CharSet.Unicode, SetLastError=true)] public static extern IntPtr LoadLibraryEx(string path, IntPtr file, uint flags);
 [DllImport("kernel32", SetLastError=true)] static extern IntPtr FindResource(IntPtr module, IntPtr name, IntPtr type);
 [DllImport("kernel32")] static extern uint SizeofResource(IntPtr module, IntPtr resource);
 [DllImport("kernel32")] static extern IntPtr LoadResource(IntPtr module, IntPtr resource);
 [DllImport("kernel32")] static extern IntPtr LockResource(IntPtr resource);
 [DllImport("kernel32")] public static extern bool FreeLibrary(IntPtr module);
 public static byte[] Read(IntPtr module, int id) {
  var resource=FindResource(module,(IntPtr)id,(IntPtr)10); if(resource==IntPtr.Zero) throw new Exception("Missing payload resource");
  var bytes=new byte[SizeofResource(module,resource)]; Marshal.Copy(LockResource(LoadResource(module,resource)),bytes,0,bytes.Length); return bytes;
 }
}
'@
    $module = [SunPayload]::LoadLibraryEx($Setup, [IntPtr]::Zero, 0x22)
    if ($module -eq [IntPtr]::Zero) { throw 'Cannot inspect installer payload' }
    $resources = @{100='sun-keyboard-tools.exe';101='profiles\sun-type7.ini';102='README.html';103='COPYRIGHT.txt';104='third-party\NOTICE.txt';105='third-party\USBPcapSetup-1.5.4.0.exe';106='third-party\usbpcap-src-incl-pdb-1.5.4.0.7z';107='third-party\GPL-2.0.txt';108='third-party\BSD-2-Clause.txt';109='sun-keyboard-capture.exe';110='Uninstall.exe'}
    try {
        foreach ($id in $resources.Keys) {
            $file = Join-Path $destination $resources[$id]
            [IO.Directory]::CreateDirectory((Split-Path -Parent $file)) | Out-Null
            [IO.File]::WriteAllBytes($file, [SunPayload]::Read($module, $id))
        }
    } finally { [SunPayload]::FreeLibrary($module) | Out-Null }
} elseif ($process.ExitCode -ne 0) { throw "Payload extraction failed: $($process.ExitCode)" }
$pairs = @(
    @((Join-Path $BuildDirectory 'Release\sun-keyboard-tools.exe'), (Join-Path $destination 'sun-keyboard-tools.exe')),
    @((Join-Path $BuildDirectory 'Release\sun-keyboard-capture.exe'), (Join-Path $destination 'sun-keyboard-capture.exe')),
    @((Join-Path $BuildDirectory 'Release\SunKeyboardTools-Maintenance.exe'), (Join-Path $destination 'Uninstall.exe')),
    @((Join-Path $root 'docs\README.html'), (Join-Path $destination 'README.html')),
    @((Join-Path $root 'profiles\sun-type7.ini'), (Join-Path $destination 'profiles\sun-type7.ini')),
    @((Join-Path $root 'COPYRIGHT.txt'), (Join-Path $destination 'COPYRIGHT.txt'))
)
foreach ($pair in $pairs) {
    if ((Get-FileHash $pair[0]).Hash -ne (Get-FileHash $pair[1]).Hash) { throw "Payload mismatch: $($pair[1])" }
}
$assets = @{
    'USBPcapSetup-1.5.4.0.exe' = '87a7edf9bbbcf07b5f4373d9a192a6770d2ff3add7aa1e276e82e38582ccb622'
    'usbpcap-src-incl-pdb-1.5.4.0.7z' = '3a66cf03425f5860ff8acd64d11cc0bb50669c7f449754f47788011259a5b15e'
}
foreach ($name in $assets.Keys) {
    if ((Get-FileHash (Join-Path $destination "third-party\$name")).Hash -ne $assets[$name]) { throw "Dependency mismatch: $name" }
}
if ((Get-Item $Setup).VersionInfo.ProductVersion -ne $version) { throw 'Installer version does not match VERSION' }
if ((Get-Item (Join-Path $destination 'sun-keyboard-tools.exe')).VersionInfo.ProductVersion -ne $version) { throw 'App version does not match VERSION' }
# Regression: reusing an extraction destination must fail promptly without a dialog.
$again = Start-Process -FilePath $Setup -ArgumentList "--extract `"$destination`"" -PassThru -WindowStyle Hidden
if (!$again.WaitForExit(15000)) { $again.Kill(); throw 'Invalid extraction displayed a blocking dialog' }
if ($again.ExitCode -eq 0) { throw 'Existing extraction directory was overwritten' }
Write-Output 'Package smoke checks passed; no app or driver was installed.'
