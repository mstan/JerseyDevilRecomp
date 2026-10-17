param(
    # Empty => read packaging/release/VERSION, the single source of truth that
    # tools/package_appimage.sh already uses. Do NOT reintroduce a hardcoded
    # default: this used to say "v1.0.5", so bumping VERSION for a release
    # silently produced a Windows zip named after the PREVIOUS version while the
    # AppImage picked up the new one. Per-platform version drift is exactly the
    # release-packager defect class this repo has been bitten by before.
    [string]$Version = "",
    [string]$BuildDir = "build-release",
    [string]$RecompilerBuildDir = "recompiler/build",
    [int]$Jobs = 2,
    [string]$ToolchainBin = "C:/msys64/mingw64/bin",
    [string]$Python = "py",
    [string[]]$CMakeArguments = @(),
    [switch]$SkipRegen
)

$ErrorActionPreference = "Stop"

$Root = Resolve-Path (Join-Path $PSScriptRoot "..")


function Invoke-ReleasePython([string[]]$Arguments) {
    $pythonArgs = if ([IO.Path]::GetFileNameWithoutExtension($Python) -eq 'py') { @('-3') + $Arguments } else { $Arguments }
    $savedPreference = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    & $Python @pythonArgs 2>&1 | Out-Host
    $pythonExit = $LASTEXITCODE
    $ErrorActionPreference = $savedPreference
    if ($pythonExit -ne 0) { throw "Release Python command failed: $pythonExit" }
}

if ([string]::IsNullOrWhiteSpace($Version)) {
    $VersionFile = Join-Path $Root "packaging/release/VERSION"
    if (-not (Test-Path $VersionFile)) {
        throw "no -Version given and $VersionFile is missing"
    }
    $Version = (Get-Content -Raw $VersionFile).Trim()
    if ([string]::IsNullOrWhiteSpace($Version)) {
        throw "$VersionFile is empty"
    }
    Write-Host "Release version from packaging/release/VERSION: $Version"
}
$BuildPath = Join-Path $Root $BuildDir
$StageRoot = Join-Path $Root "release-stage"
$Stage = Join-Path $StageRoot "JerseyDevilRecomp-windows-x64"
$ZipPath = Join-Path $Root ("JerseyDevilRecomp-{0}-windows-x64.zip" -f $Version)
$MingwBin = $ToolchainBin

$env:PATH = "$MingwBin;$env:PATH"
$CMake = Join-Path $MingwBin "cmake.exe"

# Regenerate the game's C BEFORE building. The recompiler emits the widescreen
# sites (2D true-FOV + background streamer) at regen time; the runtime build
# below just compiles generated/*.c. A stale generated/ would ship without those.
# cmake writes benign warnings (e.g. freetype's cmake_minimum_required
# deprecation) to STDERR. Under $ErrorActionPreference='Stop', PowerShell 5.1
# promotes a native command's stderr write to a TERMINATING error, aborting the
# release for a non-error. Run the native cmake invocations with the preference
# relaxed and gate on the real signal -- $LASTEXITCODE -- instead.
function Invoke-Native {
    param([scriptblock]$Cmd, [string]$What)
    $old = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    & $Cmd 2>&1 | Out-Host
    $code = $LASTEXITCODE
    $ErrorActionPreference = $old
    if ($code -ne 0) { throw "$What failed (exit $code)" }
}

function Get-TomlScalar {
    param(
        [Parameter(Mandatory)][string]$GameToml,
        [Parameter(Mandatory)][string]$Table,
        [Parameter(Mandatory)][string]$Key
    )
    $section = ""
    foreach ($raw in (Get-Content -LiteralPath $GameToml)) {
        $line = $raw.Trim()
        if (-not $line -or $line.StartsWith("#")) { continue }
        if ($line -match '^\[\[?([^\]]+)\]\]?$') { $section = $Matches[1].Trim(); continue }
        if ($section -ne $Table) { continue }
        if ($line -match ('^' + [regex]::Escape($Key) + '\s*=\s*(.+?)\s*(?:#.*)?$')) {
            return $Matches[1].Trim().Trim('"').Trim("'")
        }
    }
    return $null
}

function Ensure-BiosBackends {
    param([Parameter(Mandatory)][string]$FrameworkRoot)
    $stems = @()
    if (Test-Path -LiteralPath (Join-Path $FrameworkRoot "bios\OpenBIOS.toml")) {
        $stems += ,@("OpenBIOS", "bios/OpenBIOS.toml")
    }
    if (Test-Path -LiteralPath (Join-Path $FrameworkRoot "bios\SCPH1001.BIN")) {
        $stems += ,@("SCPH1001", "bios/SCPH1001.toml")
    }
    if (-not $stems) { throw "No BIOS profile available under $FrameworkRoot\bios" }

    $missing = @($stems | Where-Object {
        -not (Test-Path -LiteralPath (Join-Path $FrameworkRoot ("generated\{0}_dispatch.c" -f $_[0])))
    })
    if (-not $missing) { return }

    $bash = $null
    foreach ($cand in @("C:\msys64\usr\bin\bash.exe", "C:\msys64\mingw64\bin\bash.exe")) {
        if (Test-Path -LiteralPath $cand) { $bash = $cand; break }
    }
    if (-not $bash) {
        throw ("Missing recompiled BIOS backend(s): {0}. Install MSYS2 or run " +
               "psxrecomp/tools/regen_bios.sh manually." -f (($missing | ForEach-Object { $_[0] }) -join ', '))
    }

    $cygpath = Join-Path (Split-Path -Parent $bash) "cygpath.exe"
    $posixRoot = (& $cygpath -u $FrameworkRoot).Trim()
    $posixMingw = (& $cygpath -u $MingwBin).Trim()
    foreach ($stem in $missing) {
        Write-Host "Generating recompiled BIOS backend: $($stem[0])"
        $biosShellCmd = "export PATH='$posixMingw':`$PATH; cd '$posixRoot' && " +
                        "PSXRECOMP_BIOS_BUILD=recompiler/build tools/regen_bios.sh --config $($stem[1])"
        Invoke-Native { & $bash -c $biosShellCmd } "regen_bios ($($stem[0]))"
    }
}
$FrameworkRoot = Join-Path $Root "psxrecomp"
if (-not (Test-Path $FrameworkRoot)) {
    throw 'Initialize the pinned psxrecomp submodule before packaging.'
}
$RecompSourceDir = Join-Path $FrameworkRoot "recompiler"
$RecompDir = if ([System.IO.Path]::IsPathRooted($RecompilerBuildDir)) {
    $RecompilerBuildDir
} else { Join-Path $FrameworkRoot $RecompilerBuildDir }
$RecompBin = Join-Path $RecompDir "psxrecomp-game.exe"
if (-not (Test-Path -LiteralPath $RecompBin)) {
    Invoke-Native { & $CMake -S $RecompSourceDir -B $RecompDir -G Ninja -DCMAKE_BUILD_TYPE=Release } 'emitter configure'
    Invoke-Native { & $CMake --build $RecompDir --target psxrecomp-game psxrecomp-bios -j $Jobs } 'emitter build'
}

Ensure-BiosBackends -FrameworkRoot $FrameworkRoot
if (-not $SkipRegen) {
    Invoke-Native { & $RecompBin --config (Join-Path $Root 'game.toml') } 'base game regeneration'
}

Invoke-Native { & $CMake -S $Root -B $BuildPath -G Ninja -DCMAKE_BUILD_TYPE=Release -DPSX_DEBUG_TOOLS=OFF -DPSX_PGXP_VARIANT=OFF -DPSX_SDL_BACKEND=SDL3 "-DPSX_GAME_VERSION=$Version" @CMakeArguments } "cmake configure"
Invoke-Native { & $CMake --build $BuildPath --target psx-runtime -j $Jobs } "cmake build"

if (Test-Path $StageRoot) {
    $resolvedRoot = (Resolve-Path $Root).Path.TrimEnd('\')
    $resolvedStage = (Resolve-Path $StageRoot).Path.TrimEnd('\')
    if (-not $resolvedStage.StartsWith($resolvedRoot + "\", [System.StringComparison]::OrdinalIgnoreCase)) {
        throw "Refusing to delete stage path outside repo root: $resolvedStage"
    }
    Remove-Item -LiteralPath $StageRoot -Recurse -Force
}
New-Item -ItemType Directory -Force $Stage | Out-Null
New-Item -ItemType Directory -Force (Join-Path $Stage "saves") | Out-Null

# Dev exe is JerseyDevil_Recompiled.exe (per-game EXE_NAME); accept the pre-rename name too.
$DevExe = Join-Path $BuildPath "JerseyDevil_Recompiled.exe"
if (-not (Test-Path $DevExe)) { $DevExe = Join-Path $BuildPath "psx-runtime.exe" }
Copy-Item $DevExe (Join-Path $Stage "JerseyDevil_Recompiled.exe")
Copy-Item (Join-Path $Root "README.md") $Stage
Copy-Item (Join-Path $Root "LICENSE") $Stage
New-Item -ItemType Directory -Force (Join-Path $Stage "docs") | Out-Null
Copy-Item (Join-Path $Root "docs/AOT_OVERLAYS.md") (Join-Path $Stage "docs")
# Stage the mod catalog from the BUILD OUTPUT, not from mods/preloaded. The
# build output is the authoritative catalog: it is this repo's own packages
# PLUS the ones the framework stages for every game (loading speed). Copying
# the source tree instead silently drops the framework's mods from the
# release, so players get a Mods page missing entries the dev build shows.
#
# Routed through the framework's shared Add-ModCatalog. The hand-written block
# that used to live here was wrong twice over:
#
#   * it globbed mods/packages, the PRE-SPLIT layout. Framework 4cc04be3 moved
#     staged build output to mods/bundled and nothing in this repo followed, so
#     at framework master this threw "No mod catalog staged ... build the
#     runtime first" -- a message that blames the build for a layout rename
#     (bead beads-eio.3.101);
#   * it asserted "at least 16 package families". A count describes only one
#     side of a catalog two repositories contribute to, so it goes stale the
#     moment either side gains a mod. The identical assertion made Tomba 2
#     unreleasable on 2026-09-01 when the framework gained a fifth builtin.
#
# Add-ModCatalog asserts the invariant instead: every package the SOURCES
# define -- this repo's mods/preloaded/packages and the framework's
# mods/builtin/packages -- must survive into the staged catalog. It also strips
# the two things under mods/ that belong to this machine (installed/ and
# state.toml) rather than leaving them to be noticed later.
. (Join-Path $FrameworkRoot "tools\release_overlay_stage.ps1")
Add-ModCatalog -BuildPath $BuildPath -Stage $Stage `
               -GameModSource (Join-Path $Root "mods\preloaded") `
               -FrameworkModSource (Join-Path $FrameworkRoot "mods\builtin") `
               -RuntimeTarget psx-runtime | Out-Null
$BundledBiosSrc = Join-Path $BuildPath "bios"
if (!(Test-Path (Join-Path $BundledBiosSrc "openbios.bin")) -or
    (Get-Item (Join-Path $BundledBiosSrc "openbios.bin")).Length -ne 524288 -or
    !(Test-Path (Join-Path $BundledBiosSrc "OpenBIOS.LICENSE"))) {
    throw "Runtime build did not stage OpenBIOS and its MIT notice"
}
$BundledBiosDst = Join-Path $Stage "bios"
New-Item -ItemType Directory -Force $BundledBiosDst | Out-Null
Copy-Item (Join-Path $BundledBiosSrc "openbios.bin") $BundledBiosDst
Copy-Item (Join-Path $BundledBiosSrc "OpenBIOS.LICENSE") $BundledBiosDst
if (Test-Path (Join-Path $Root "RELEASE_NOTES.md")) {
    Copy-Item (Join-Path $Root "RELEASE_NOTES.md") $Stage
}

# Launcher assets: this build ships the shared recomp-ui Dear ImGui launcher
# (RECOMP_LAUNCHER; see main.cpp + recomp-ui/recomp_ui.cmake), which loads from
# <exe>/assets/ (fonts + img TGAs, including this repo's boxart baked in by
# recomp_target_launcher_ui's POST_BUILD).
$AssetsSrc = Join-Path $BuildPath "assets"
if (-not (Test-Path (Join-Path $AssetsSrc "img"))) {
    throw "recomp-ui launcher assets missing at $AssetsSrc -- was the recomp-ui launcher built (recomp-ui junction present)?"
}
Copy-Item -Recurse -Force $AssetsSrc (Join-Path $Stage "assets")
$fontCount = (Get-ChildItem (Join-Path $Stage "assets/fonts") -Filter *.ttf -ErrorAction SilentlyContinue).Count
$imgCount  = (Get-ChildItem (Join-Path $Stage "assets/img")   -Filter *.tga -ErrorAction SilentlyContinue).Count
Write-Host "Bundled recomp-ui launcher assets: $fontCount font(s) + $imgCount image(s)"

# Player-facing game.toml: same effective runtime settings as the dev config,
# minus dev-only sections ([recompiler] inputs beyond the required block, the
# gcc overlay-autocompile command, and the [audit] block). overlay_backend is
# left at the default "auto": with no gcc toolchain on a player box it resolves
# to tcc, which fills overlay gaps via the bundled overlay_toolchain/ (no system
# python or gcc needed). Players can edit [runtime]/[video] post-install.
# Both platforms ship the same game.toml used for code generation.
Copy-Item -Force (Join-Path $Root "game.toml") (Join-Path $Stage "game.toml")

# Fresh original-disc AOT is mandatory, including when base regeneration is skipped.
$RecompTools = (Resolve-Path -LiteralPath (Join-Path $FrameworkRoot "tools")).Path
$RecompInc = (Resolve-Path -LiteralPath (Join-Path $FrameworkRoot "runtime/include")).Path
$StagedGameToml = Join-Path $Stage 'game.toml'
Invoke-ReleasePython @(
    (Join-Path $RecompTools 'aot_overlay_pipeline.py'), 'release',
    '--profile', (Join-Path $Root 'aot/overlays.json'),
    '--game-toml', (Join-Path $Root 'game.toml'), '--runtime-config', $StagedGameToml,
    '--runtime-build-dir', $BuildPath, '--runtime-target', 'psx-runtime',
    '--recompiler', $RecompBin, '--work-dir', (Join-Path $Root 'build-aot'),
    '--stage', $Stage, '--gcc', (Join-Path $MingwBin 'gcc.exe'), '--workers', [string]$Jobs
)
Add-OverlayToolchain -Stage $Stage -RecompDir $RecompDir -RecompTools $RecompTools `
                     -RecompInc $RecompInc -MingwBin $MingwBin `
                     -DlCache (Join-Path $Root "tools\_toolchain_cache") | Out-Null
# The Release build is statically linked (PSX_STATIC_RUNTIME defaults ON for
# MinGW Release), so the exe imports ONLY Windows system DLLs -- nothing to
# bundle. Assert self-containment rather than trust it (mismatched side-by-side
# DLLs were the cause of the 0xc000007b launch crash on other projects).
$objdump = Join-Path $MingwBin 'objdump.exe'
if (-not (Test-Path -LiteralPath $objdump)) { $objdump = Join-Path $MingwBin 'llvm-objdump.exe' }
if (-not (Test-Path -LiteralPath $objdump)) { throw "No native objdump in $MingwBin" }
$imports = & $objdump -p (Join-Path $Stage "JerseyDevil_Recompiled.exe") |
    Select-String "DLL Name: (.+)" | ForEach-Object { $_.Matches[0].Groups[1].Value.Trim() }
$systemDlls = @("kernel32.dll","user32.dll","gdi32.dll","shell32.dll","msvcrt.dll",
                "advapi32.dll","ws2_32.dll","comdlg32.dll","dbghelp.dll","ole32.dll",
                "oleaut32.dll","winmm.dll","imm32.dll","version.dll","setupapi.dll",
                "dinput8.dll","rpcrt4.dll","hid.dll","cfgmgr32.dll","opengl32.dll",
                "d2d1.dll","dwrite.dll","ntdll.dll","bcrypt.dll","dwmapi.dll","shlwapi.dll","ucrtbase.dll")
$nonSystem = $imports | Where-Object { $systemDlls -notcontains $_.ToLower() -and $_ -notmatch '^api-ms-win-crt-[a-z0-9-]+\.dll$' }
if ($nonSystem) {
    throw "Release exe is NOT self-contained -- imports non-system DLL(s): $($nonSystem -join ', ')"
}
Write-Host "Verified self-contained: imports only system DLLs ($($imports.Count) total)"

@"
; PSXRecomp input mapping. PSX buttons are active when any listed source is pressed.
; Sources use SDL/Xbox names: a,b,x,y,back,start,leftshoulder,rightshoulder,
; lefttrigger,righttrigger,dpup,dpdown,dpleft,dpright,leftx-/leftx+/lefty-/lefty+.

[controller]
enabled = true
device = 0
deadzone = 12000

[mapping]
up = dpup,lefty-
down = dpdown,lefty+
left = dpleft,leftx-
right = dpright,leftx+
cross = a
circle = b
square = x
triangle = y
l1 = leftshoulder
r1 = rightshoulder
l2 = lefttrigger
r2 = righttrigger
start = start
select = back
"@ | Set-Content -Encoding ASCII (Join-Path $Stage "input.ini")

Copy-Item (Join-Path $Root "packaging/release/START_HERE.txt") $Stage

if (Test-Path $ZipPath) {
    Remove-Item -Force $ZipPath
}
# Keep framework/UI notices and source pins with the playable package.
$Licenses = Join-Path $Stage 'licenses'
New-Item -ItemType Directory -Path $Licenses -Force | Out-Null
Copy-Item -Path (Join-Path $FrameworkRoot 'runtime/licenses/*') -Destination $Licenses -Force
foreach ($notice in @('LICENSE', 'THIRD_PARTY_ATTRIBUTION.md')) {
    $source = Join-Path $FrameworkRoot $notice
    if (Test-Path -LiteralPath $source) {
        Copy-Item -LiteralPath $source -Destination (Join-Path $Licenses "psxrecomp-$notice") -Force
    }
}
$UiLicense = Join-Path $Root 'recomp-ui/LICENSE'
if (Test-Path -LiteralPath $UiLicense) {
    Copy-Item -LiteralPath $UiLicense -Destination (Join-Path $Licenses 'recomp-ui-LICENSE') -Force
}
foreach ($doc in @('VERSION','RELEASE_NOTES.md','framework_pins.txt','BUILD_PROVENANCE.json')) {
    $source = Join-Path $Root $doc
    if (Test-Path -LiteralPath $source) { Copy-Item -LiteralPath $source -Destination $Stage -Force }
}
$ReplayHelper = Join-Path $BuildPath 'replay-diagnostics.py'
if (-not (Test-Path -LiteralPath $ReplayHelper)) { throw "Build is missing $ReplayHelper" }
Copy-Item -LiteralPath $ReplayHelper -Destination (Join-Path $Stage 'replay-diagnostics.py') -Force
New-Item -ItemType Directory -Path (Join-Path $Stage 'docs') -Force | Out-Null
Copy-Item -LiteralPath (Join-Path $Root 'docs/PLAYTEST_DIAGNOSTICS.md') -Destination (Join-Path $Stage 'docs') -Force
Compress-Archive -Path (Join-Path $Stage "*") -DestinationPath $ZipPath -Force
$Digest = (Get-FileHash -LiteralPath $ZipPath -Algorithm SHA256).Hash.ToLowerInvariant()
[IO.File]::WriteAllText("$ZipPath.sha256", "$Digest  $([IO.Path]::GetFileName($ZipPath))`n", [Text.Encoding]::ASCII)

Write-Host "Wrote $ZipPath"

Invoke-ReleasePython @(
    (Join-Path $FrameworkRoot 'tools/ci/audit_playtest_diagnostics_package.py'),
    $ZipPath, '--game-id', 'SCUS-94907', '--diagnostics-id', 'jerseydevil.diagnostics'
)
