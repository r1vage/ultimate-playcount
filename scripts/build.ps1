# Builds x64 Release, runs the core tests and packages dist\foo_ultimate_playcount-<version>.fb2k-component
param([switch]$SkipTests)

$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$msbuild = & $vswhere -latest -products * -requires Microsoft.Component.MSBuild -find 'MSBuild\**\Bin\MSBuild.exe' | Select-Object -First 1
if (-not $msbuild) { throw 'MSBuild not found (install Visual Studio 2022 or Build Tools with C++ desktop development).' }

& $msbuild "$root\foo_ultimate_playcount.vcxproj" -m -nologo -v:minimal -p:Configuration=Release -p:Platform=x64
if ($LASTEXITCODE) { throw 'Build failed' }

if (-not $SkipTests) {
    & $msbuild "$root\tests\core_tests.vcxproj" -nologo -v:minimal -p:Configuration=Release -p:Platform=x64
    if ($LASTEXITCODE) { throw 'Test build failed' }
    & "$root\build\tests\Release\core_tests.exe"
    if ($LASTEXITCODE) { throw 'Tests failed' }
}

$version = (Select-String -Path "$root\src\version.h" -Pattern 'UPC_VERSION_STRING "(.+)"').Matches[0].Groups[1].Value
$dist = Join-Path $root 'dist'
New-Item -ItemType Directory -Force $dist | Out-Null
$component = Join-Path $dist "foo_ultimate_playcount-$version.fb2k-component"
Remove-Item $component -Force -ErrorAction SilentlyContinue

# Compress-Archive on Windows PowerShell writes backslashes into entry names; build the zip by hand
# so the DLL is stored as "x64/foo_ultimate_playcount.dll" where foobar2000 x64 looks for it.
Add-Type -AssemblyName System.IO.Compression, System.IO.Compression.FileSystem
$archive = [IO.Compression.ZipFile]::Open($component, 'Create')
try {
    [void][IO.Compression.ZipFileExtensions]::CreateEntryFromFile($archive, "$root\build\x64\Release\foo_ultimate_playcount.dll", 'x64/foo_ultimate_playcount.dll')
} finally {
    $archive.Dispose()
}
Write-Host "Packaged $component"
