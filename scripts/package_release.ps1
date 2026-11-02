# Builds the mod, runs the tests, and packages it into dist\ygol-access-<version>.zip.
# The zip holds one ygol-access-<version> folder: the two DLLs that go in the game folder, the install
# steps, the changelog, this project's license and Prism's licenses.
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
Set-Location $root

cmake -S . -B build -A x64
if ($LASTEXITCODE -ne 0) { throw "cmake configure failed" }
cmake --build build --config Release
if ($LASTEXITCODE -ne 0) { throw "cmake build failed" }
ctest --test-dir build -C Release --output-on-failure
if ($LASTEXITCODE -ne 0) { throw "tests failed" }

$version = [regex]::Match((Get-Content CMakeLists.txt -Raw), '(?m)^project\(ygol_access VERSION (\d+\.\d+\.\d+)').Groups[1].Value
if (-not $version) { throw "No version in CMakeLists.txt's project() line" }
$name = "ygol-access-$version"
$stage = Join-Path $root "build\package\$name"
$zip = Join-Path $root "dist\$name.zip"

if (Test-Path $stage) { Remove-Item $stage -Recurse -Force }
New-Item -ItemType Directory -Force $stage, "$stage\third_party\prism", (Split-Path $zip) | Out-Null
Copy-Item build\Release\xinput9_1_0.dll, third_party\prism\dynamic\release\bin\prism.dll, LICENSE, CHANGELOG.md $stage
Copy-Item third_party\prism\LICENSES, third_party\prism\NOTICE "$stage\third_party\prism" -Recurse
@"
ygol-access ${version}: screen reader support for Yu-Gi-Oh! Legacy of the Duelist: Link Evolution (Steam, Windows).

Install
1. Close the game.
2. Copy xinput9_1_0.dll and prism.dll into the game folder, next to YuGiOh.exe. In Steam: right-click the game,
   Manage, Browse local files.
3. Start the game. Speech goes to your screen reader (NVDA, JAWS and others), or to Windows speech without one.

Uninstall: delete those two files from the game folder.
The mod writes a log, ygol-access.log, in the game folder; include it when reporting a problem.
"@ | Set-Content "$stage\INSTALL.txt" -Encoding utf8

if (Test-Path $zip) { Remove-Item $zip -Force }
Compress-Archive -Path $stage -DestinationPath $zip
Write-Host "Packaged $zip"
Get-ChildItem $stage | ForEach-Object { Write-Host "  $($_.Name)" }
