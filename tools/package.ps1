param([string]$CMake = 'cmake', [string]$BuildDirectory = 'build')
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$version = (Get-Content -LiteralPath (Join-Path $root 'VERSION') -Raw).Trim()
if ($version -notmatch '^\d+\.\d+\.\d+$') { throw 'VERSION must be major.minor.patch' }
if (![IO.Path]::IsPathRooted($BuildDirectory)) { $BuildDirectory = Join-Path $root $BuildDirectory }
$deps = Join-Path $root '.local\dependencies'
New-Item -ItemType Directory -Force -Path $deps | Out-Null
$assets = @{
    'USBPcapSetup-1.5.4.0.exe' = '87a7edf9bbbcf07b5f4373d9a192a6770d2ff3add7aa1e276e82e38582ccb622'
    'usbpcap-src-incl-pdb-1.5.4.0.7z' = '3a66cf03425f5860ff8acd64d11cc0bb50669c7f449754f47788011259a5b15e'
}
foreach ($name in $assets.Keys) {
    $path = Join-Path $deps $name
    if (!(Test-Path -LiteralPath $path)) {
        Invoke-WebRequest -UseBasicParsing "https://github.com/desowin/usbpcap/releases/download/1.5.4.0/$name" -OutFile $path
    }
    if ((Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash -ne $assets[$name]) { throw "Hash mismatch: $name" }
}
$signature = Get-AuthenticodeSignature (Join-Path $deps 'USBPcapSetup-1.5.4.0.exe')
if ($signature.Status -ne 'Valid') { throw "USBPcap signature validation failed: $($signature.Status)" }
& $CMake -S $root -B $BuildDirectory -G 'Visual Studio 17 2022' -A x64 -DSUN_BUILD_SETUP=ON
if ($LASTEXITCODE) { throw 'CMake configure failed' }
& $CMake --build $BuildDirectory --config Release -- /nodeReuse:false
if ($LASTEXITCODE) { throw 'Build failed' }
$ctest = Join-Path (Split-Path (Get-Command $CMake).Source) 'ctest.exe'
& $ctest --test-dir $BuildDirectory -C Release --output-on-failure
if ($LASTEXITCODE) { throw 'Tests failed' }
$out = Join-Path $root 'out'
New-Item -ItemType Directory -Force -Path $out | Out-Null
$setup = Join-Path $out "SunKeyboardTools-Setup-$version-x64.exe"
Copy-Item -LiteralPath (Join-Path $BuildDirectory 'Release\SunKeyboardTools-Setup.exe') -Destination $setup -Force
$hash = (Get-FileHash -LiteralPath $setup -Algorithm SHA256).Hash.ToLowerInvariant()
[IO.File]::WriteAllText((Join-Path $out 'SHA256SUMS.txt'), "$hash  SunKeyboardTools-Setup-$version-x64.exe`n", [Text.Encoding]::ASCII)
$notes = Join-Path $root "docs\releases\$version.md"
if (!(Test-Path -LiteralPath $notes)) { throw "Missing release notes for $version" }
Copy-Item -LiteralPath $notes -Destination (Join-Path $out 'RELEASE_NOTES.md') -Force
Write-Output $setup
