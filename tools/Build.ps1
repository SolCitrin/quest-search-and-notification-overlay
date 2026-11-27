# Builds QuestSearchOverlay.dll and stages an MO2-ready mod folder under dist\.
# Dependencies go in deps\ (see README.md, "Building"). -Parts notify compiles core + capture only
# (HUDNotificationReplacer.dll) into build-notify-ng16\; it is not packaged.
param([string]$PackageName = 'Quest Search and Notification Overlay 1.0.0', [ValidateSet('all', 'notify')][string]$Parts = 'all')
# 'Continue', not 'Stop': in Windows PowerShell 5.1 a native tool's stderr line (CMake's status messages)
# becomes an error record, which 'Stop' turns into a failure. Failures are checked via $LASTEXITCODE / throw.
$ErrorActionPreference = 'Continue'
$project = Split-Path $PSScriptRoot -Parent
$deps = "$project\deps"
$common = "$deps\CommonLibSSE-NG-v16.12.0"                  # alandtse/CommonLibSSE-NG tag v16.12.0
$directxtk = "$deps\directxtk-install\share\directxtk"      # DirectXTK, built and installed here
$headers = "$deps\headers"                                  # contains rapidcsv-8.84\src
$spdlog = "$deps\spdlog-install\lib\cmake\spdlog"           # spdlog 1.14.1, built and installed here
$prisma = "$deps\prisma"                                    # PrismaUI_API.h from the Prisma UI SDK
foreach ($p in @("$common\CMakeLists.txt", "$directxtk\directxtk-config.cmake", "$headers\rapidcsv-8.84", $spdlog, "$prisma\PrismaUI_API.h")) { if (!(Test-Path $p)) { throw "Missing dependency: $p (see README.md)" } }

$vs = & 'C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe' -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (!$vs) { throw 'MSVC not found' }
$cmake = "$vs\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
$build = if ($Parts -eq 'all') { "$project\build-ng16" } else { "$project\build-notify-ng16" }
$target = if ($Parts -eq 'all') { 'QuestSearchOverlay' } else { 'HUDNotificationReplacer' }

& $cmake -S $project -B $build -G 'Visual Studio 17 2022' -A x64 "-DQSO_COMMONLIB_DIR=$common" "-DQSO_HEADERS_DIR=$headers" "-Dspdlog_DIR=$spdlog" "-Ddirectxtk_DIR=$directxtk" -DSKSE_SUPPORT_PATCH_SAFETY=OFF "-DQSO_PRISMA_DIR=$prisma" "-DQSO_PARTS=$Parts"
if ($LASTEXITCODE) { throw "configure failed ($LASTEXITCODE)" }
& $cmake --build $build --config Release --parallel 2
if ($LASTEXITCODE) { throw "build failed ($LASTEXITCODE)" }

$dll = "$build\Release\$target.dll"
$dump = Get-ChildItem "$vs\VC\Tools\MSVC" -Recurse -Filter dumpbin.exe | Where-Object FullName -Match 'Hostx64\\x64' | Select-Object -First 1
$exports = (& $dump.FullName /exports $dll) -join "`n"
foreach ($s in 'SKSEPlugin_Load', 'SKSEPlugin_Query', 'SKSEPlugin_Version') { if ($exports -notmatch "\b$s\b") { throw "Missing export $s" } }

if ($Parts -ne 'all') {
    Write-Host "PASS $Parts build (not packaged): $dll"
    Write-Host ("SHA256 " + (Get-FileHash $dll).Hash)
    return
}
$package = "$project\dist\$PackageName"
if (Test-Path $package) { throw "Package exists, not overwriting: $package" }
New-Item -ItemType Directory -Path "$package\SKSE\Plugins" | Out-Null
Copy-Item $dll "$package\SKSE\Plugins"
Copy-Item "$project\QuestSearchOverlay.ini" "$package\SKSE\Plugins"
# GPL-3.0 text and third-party notices travel with the DLL (they are kept as files in the project root).
foreach ($f in 'QuestSearchOverlay-LICENSE.txt', 'QuestSearchOverlay-THIRD-PARTY.txt') { Copy-Item "$project\$f" "$package\SKSE\Plugins" }
# PrismaUI loads views from Data\PrismaUI\views\<name>\ (CreateView("QuestSearchOverlay/index.html")).
$viewOut = "$package\PrismaUI\views\QuestSearchOverlay"
New-Item -ItemType Directory -Path $viewOut | Out-Null
# Only the page itself: demo.html / demo.js in view\ are for the browser preview and are not shipped.
# 0.16.0: the box layer has its own files (boxes.js / boxes.css), shared with a future notifications-only build.
$viewFiles = 'index.html', 'overlay.css', 'overlay.js', 'boxes.css', 'boxes.js'
foreach ($f in $viewFiles) { Copy-Item "$project\view\$f" $viewOut }
foreach ($f in $viewFiles) { if (!(Test-Path "$viewOut\$f")) { throw "View file missing: $f" } }
Write-Host "PASS $package"
Write-Host ("SHA256 " + (Get-FileHash "$package\SKSE\Plugins\QuestSearchOverlay.dll").Hash)
