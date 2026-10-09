param([string]$Setup, [string]$BuildDirectory = 'build')
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$version = (Get-Content (Join-Path $root 'VERSION') -Raw).Trim()
if (!$Setup) { $Setup = Join-Path $root "out\SunKeyboardTools-Setup-$version-x64.exe" }
if (![IO.Path]::IsPathRooted($BuildDirectory)) { $BuildDirectory = Join-Path $root $BuildDirectory }
$destination = Join-Path $root ('.local\package-smoke-' + [Guid]::NewGuid().ToString('N'))
$process = Start-Process -FilePath $Setup -ArgumentList "--extract `"$destination`"" -PassThru -WindowStyle Hidden
if (!$process.WaitForExit(30000)) { $process.Kill(); throw 'Payload extraction timed out' }
if ($process.ExitCode -ne 0) { throw "Payload extraction failed: $($process.ExitCode)" }
$pairs = @(
    @((Join-Path $BuildDirectory 'Release\sun-keyboard-tools.exe'), (Join-Path $destination 'sun-keyboard-tools.exe')),
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
