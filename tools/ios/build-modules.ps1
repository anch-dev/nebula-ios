# SPDX-License-Identifier: GPL-3.0-only
<#
.SYNOPSIS
Builds Nebula's game modules for iOS from your own copy of Super Mario Galaxy
and adds them to the Nebula IPA, ready for SideStore/AltStore.

.DESCRIPTION
iOS runs only code that is signed and inside the app, so the game modules
(compiled from your game) have to be in the IPA before SideStore/AltStore
signs it. This script does what Nebula-Setup.exe does on Windows, for iOS:

  1. nebula-recomp extracts the game, packages its content and generates the
     C++ for the game, the Home Menu and the audio DSP (skipped with
     -Generated).
  2. clang compiles that C++ for arm64 iOS, against the module SDK (runtime
     headers, runtime libraries and the iOS SDK headers they need), and
     ld64.lld links RMGE01_game, RMGE01_home_button and RMGE01_dsp.
  3. Each module becomes a framework (Frameworks/<name>.framework).
  4. With -Ipa, the frameworks and the boot image are added to a copy of the
     IPA (and with -BundleContent, the game content too).

Needs LLVM 19 or newer for Windows (clang++.exe and ld64.lld.exe; the
llvm.org installer has both). Nothing leaves your PC.

.EXAMPLE
.\build-modules.ps1 -Game D:\Games\SMG.rvz -Sdk .\nebula-ios-module-sdk -Ipa .\Nebula.ipa -Out D:\nebula-ios
#>
[CmdletBinding()]
param(
    # The game: ISO, RVZ or an extracted folder (with DATA\sys\main.dol).
    [string]$Game,
    # An existing `nebula-recomp generate` output folder, instead of -Game.
    [string]$Generated,
    # The module SDK folder (or its .zip) from the iOS CI build.
    [Parameter(Mandatory = $true)][string]$Sdk,
    # nebula-recomp.exe; default: next to this script, then PATH.
    [string]$Recomp,
    # Folder with clang++.exe and ld64.lld.exe; default: PATH, then
    # C:\Program Files\LLVM\bin.
    [string]$Llvm,
    # The Nebula IPA to add the modules to.
    [string]$Ipa,
    # Also copy the game content into the IPA (large). Otherwise copy the
    # content folder to Nebula's folder on the device with the Files app.
    [switch]$BundleContent,
    # Output folder.
    [Parameter(Mandatory = $true)][string]$Out,
    [int]$Jobs = [Environment]::ProcessorCount
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 3.0
Add-Type -AssemblyName System.IO.Compression
Add-Type -AssemblyName System.IO.Compression.FileSystem

$Target = 'arm64-apple-ios16.0'
$MinimumOs = '16.0'

function Step([string]$text) { Write-Host "==> $text" -ForegroundColor Cyan }
# Forward slashes: Windows accepts them, and the Darwin linker reads its
# response file with Unix quoting, where a backslash escapes.
function Quote([string]$path) { '"' + $path.Replace('\', '/') + '"' }

function Find-Tool([string]$name, [string[]]$folders) {
    foreach ($folder in $folders) {
        if (-not $folder) { continue }
        $candidate = [System.IO.Path]::Combine($folder, $name)
        if ([System.IO.File]::Exists($candidate)) { return $candidate }
    }
    $command = Get-Command $name -ErrorAction SilentlyContinue
    if ($command) { return $command.Source }
    return $null
}

# Runs commands ($_.Exe, $_.Args, $_.Name) with at most $jobs at a time.
function Invoke-Parallel([object[]]$commands, [int]$jobs) {
    $queue = New-Object System.Collections.Generic.Queue[object]
    foreach ($command in $commands) { $queue.Enqueue($command) }
    $running = New-Object System.Collections.Generic.List[object]
    $total = $commands.Count
    $done = 0
    $failed = 0
    while ($queue.Count -gt 0 -or $running.Count -gt 0) {
        while ($running.Count -lt $jobs -and $queue.Count -gt 0) {
            $command = $queue.Dequeue()
            $info = New-Object System.Diagnostics.ProcessStartInfo
            $info.FileName = $command.Exe
            $info.Arguments = $command.Args
            $info.UseShellExecute = $false
            $info.CreateNoWindow = $true
            $info.RedirectStandardOutput = $true
            $info.RedirectStandardError = $true
            $process = [System.Diagnostics.Process]::Start($info)
            $running.Add([pscustomobject]@{
                Process = $process
                Command = $command
                Out = $process.StandardOutput.ReadToEndAsync()
                Err = $process.StandardError.ReadToEndAsync()
            })
        }
        Start-Sleep -Milliseconds 100
        foreach ($item in $running.ToArray()) {
            if (-not $item.Process.HasExited) { continue }
            $item.Process.WaitForExit()
            $null = $running.Remove($item)
            $done++
            $text = ($item.Out.Result + $item.Err.Result).Trim()
            if ($item.Process.ExitCode -ne 0) {
                $failed++
                Write-Host "[$done/$total] FAILED $($item.Command.Name)" -ForegroundColor Red
                if ($text) { Write-Host $text }
            } else {
                Write-Host "[$done/$total] $($item.Command.Name)"
                if ($text) { Write-Host $text }
            }
        }
    }
    if ($failed -gt 0) { throw "$failed of $total commands failed" }
}

function Invoke-Checked([string]$exe, [string[]]$arguments) {
    & $exe @arguments
    if ($LASTEXITCODE -ne 0) { throw "$exe $($arguments -join ' ') failed ($LASTEXITCODE)" }
}

# --- Inputs ---------------------------------------------------------------
if (-not $Game -and -not $Generated) { throw 'Give -Game (your copy of the game) or -Generated.' }
New-Item -ItemType Directory -Force -Path $Out | Out-Null
$Out = (Resolve-Path $Out).Path

if ($Sdk.EndsWith('.zip')) {
    $expanded = Join-Path $Out 'module-sdk'
    if (Test-Path $expanded) { Remove-Item -Recurse -Force $expanded }
    [System.IO.Compression.ZipFile]::ExtractToDirectory((Resolve-Path $Sdk).Path, $expanded)
    $Sdk = $expanded
    # A zip of the SDK folder itself.
    if (-not (Test-Path (Join-Path $Sdk 'sysroot'))) {
        $inner = Get-ChildItem $Sdk -Directory | Select-Object -First 1
        if ($inner) { $Sdk = $inner.FullName }
    }
}
$Sdk = (Resolve-Path $Sdk).Path
foreach ($part in 'include', 'lib', 'sysroot') {
    if (-not (Test-Path (Join-Path $Sdk $part))) { throw "$Sdk is not a Nebula iOS module SDK (no $part)" }
}

$llvmFolders = @($Llvm, 'C:\Program Files\LLVM\bin')
$clang = Find-Tool 'clang++.exe' $llvmFolders
if (-not $clang) { $clang = Find-Tool 'clang++' $llvmFolders }
if (-not $clang) { throw 'clang++ not found: install LLVM for Windows or pass -Llvm <folder>' }
$llvmBin = Split-Path $clang
$linker = Find-Tool 'ld64.lld.exe' @($llvmBin)
$linkerFlavor = @()
if (-not $linker) { $linker = Find-Tool 'ld64.lld' @($llvmBin) }
if (-not $linker) {
    $linker = Find-Tool 'lld.exe' @($llvmBin)
    if (-not $linker) { $linker = Find-Tool 'lld' @($llvmBin) }
    if (-not $linker) { throw "ld64.lld not found next to $clang" }
    $linkerFlavor = @('-flavor', 'darwin')
}
Write-Host "clang:  $clang"
Write-Host "linker: $linker"
Write-Host "SDK:    $Sdk"

# --- 1. Generate ----------------------------------------------------------
$content = $null
if (-not $Generated) {
    if (-not $Recomp) {
        $Recomp = Find-Tool 'nebula-recomp.exe' @($PSScriptRoot)
        if (-not $Recomp) { $Recomp = Find-Tool 'nebula-recomp' @($PSScriptRoot) }
    }
    if (-not $Recomp) { throw 'nebula-recomp.exe not found: pass -Recomp' }
    $Game = (Resolve-Path $Game).Path
    Step 'Checking the game'
    Invoke-Checked $Recomp @('identify', $Game)
    $disc = $Game
    if (-not (Test-Path -PathType Container $Game)) {
        $disc = Join-Path $Out 'disc'
        if (-not (Test-Path $disc)) {
            Step 'Extracting the game'
            Invoke-Checked $Recomp @('extract', $Game, '--output', $disc)
        }
    }
    $content = Join-Path $Out 'content'
    if (-not (Test-Path $content)) {
        Step 'Packaging the game content'
        Invoke-Checked $Recomp @('package-content', $disc, '--output', $content)
    }
    $Generated = Join-Path $Out 'generated'
    if (-not (Test-Path $Generated)) {
        Step 'Recompiling the game code'
        Invoke-Checked $Recomp @('generate', $disc, '--output', $Generated)
    }
}
$Generated = (Resolve-Path $Generated).Path

# --- 2. Compile -----------------------------------------------------------
$objects = Join-Path $Out 'obj'
New-Item -ItemType Directory -Force -Path $objects | Out-Null
$sysroot = Join-Path $Sdk 'sysroot'
# The flags of the Windows build (clang-cl /EHsc /GR- -w -ffp-contract=off
# -fwrapv -fno-strict-aliasing), for arm64 iOS.
$common = @(
    "--target=$Target", '-isysroot', (Quote $sysroot),
    '-nostdinc++', '-isystem', (Quote (Join-Path $sysroot 'usr\include\c++\v1')),
    '-I', (Quote (Join-Path $Sdk 'include')),
    '-std=c++20', '-fno-rtti', '-w', '-ffp-contract=off', '-fwrapv', '-fno-strict-aliasing',
    '-DGALAXY_BUILDING_GAME_MODULE=1', '-DNDEBUG'
) -join ' '
$rsp = Join-Path $objects 'common.rsp'
[System.IO.File]::WriteAllText($rsp, $common)

$modules = @(
    @{ Name = 'RMGE01_game'; Dir = 'game'; Opt = '-O2'; Pch = 'functions.h'; Libs = @('libgalaxy_ppc_float.a', 'libgalaxy_softfloat.a') },
    # Upstream builds the Home Menu RSO at /Od: it is one function with a
    # resume label per instruction, which no optimizer finishes quickly.
    @{ Name = 'RMGE01_home_button'; Dir = 'home'; Opt = '-O0'; Pch = $null; Libs = @('libgalaxy_ppc_float.a', 'libgalaxy_softfloat.a') },
    @{ Name = 'RMGE01_dsp'; Dir = 'dsp'; Opt = '-O2'; Pch = $null; Libs = @('libgalaxy_dsp_alu.a') }
)

foreach ($module in $modules) {
    $source = Join-Path $Generated $module.Dir
    if (-not (Test-Path $source)) { throw "$source is missing" }
    $moduleObjects = Join-Path $objects $module.Dir
    New-Item -ItemType Directory -Force -Path $moduleObjects | Out-Null
    $flags = "@$(Quote $rsp) $($module.Opt)"
    if ($module.Pch -and (Test-Path (Join-Path $source $module.Pch))) {
        Step "Precompiling $($module.Dir)\$($module.Pch)"
        $pch = Join-Path $moduleObjects "$($module.Pch).pch"
        Invoke-Parallel @([pscustomobject]@{
            Name = $module.Pch; Exe = $clang
            Args = "$flags -x c++-header $(Quote (Join-Path $source $module.Pch)) -o $(Quote $pch)"
        }) 1
        $flags = "$flags -include-pch $(Quote $pch)"
    }
    Step "Compiling $($module.Name)"
    # Largest first, so the slowest compiles do not trail at the end.
    $files = Get-ChildItem -Path $source -Filter '*.cpp' | Sort-Object Length -Descending
    $commands = foreach ($file in $files) {
        $object = Join-Path $moduleObjects ($file.BaseName + '.o')
        if ((Test-Path $object) -and (Get-Item $object).LastWriteTime -gt $file.LastWriteTime) { continue }
        [pscustomobject]@{
            Name = "$($module.Dir)\$($file.Name)"; Exe = $clang
            Args = "$flags -c $(Quote $file.FullName) -o $(Quote $object)"
        }
    }
    if ($commands) { Invoke-Parallel @($commands) $Jobs }
}

# --- 3. Link into frameworks ---------------------------------------------
$frameworks = Join-Path $Out 'Frameworks'
New-Item -ItemType Directory -Force -Path $frameworks | Out-Null
foreach ($module in $modules) {
    Step "Linking $($module.Name)"
    $name = $module.Name
    $framework = Join-Path $frameworks "$name.framework"
    New-Item -ItemType Directory -Force -Path $framework | Out-Null
    $linkRsp = Join-Path $objects "$name.link.rsp"
    $lines = New-Object System.Collections.Generic.List[string]
    $lines.AddRange([string[]]@(
        '-arch', 'arm64', '-platform_version', 'ios', $MinimumOs, $MinimumOs,
        '-dylib', '-install_name', "@rpath/$name.framework/$name",
        '-syslibroot', (Quote $sysroot), '-o', (Quote (Join-Path $framework $name))))
    foreach ($object in Get-ChildItem -Path (Join-Path $objects $module.Dir) -Filter '*.o') {
        $lines.Add((Quote $object.FullName))
    }
    foreach ($lib in $module.Libs) { $lines.Add((Quote (Join-Path $Sdk "lib\$lib"))) }
    $lines.AddRange([string[]]@('-lc++', '-lSystem'))
    [System.IO.File]::WriteAllText($linkRsp, ($lines -join "`n"))
    Invoke-Checked $linker ($linkerFlavor + @("@$linkRsp"))
    $plist = @"
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>CFBundleDevelopmentRegion</key><string>en</string>
    <key>CFBundleExecutable</key><string>$name</string>
    <key>CFBundleIdentifier</key><string>io.github.nebula.module.$($name.Replace('_', '-'))</string>
    <key>CFBundleInfoDictionaryVersion</key><string>6.0</string>
    <key>CFBundleName</key><string>$name</string>
    <key>CFBundlePackageType</key><string>FMWK</string>
    <key>CFBundleShortVersionString</key><string>1.0</string>
    <key>CFBundleVersion</key><string>1</string>
    <key>CFBundleSupportedPlatforms</key><array><string>iPhoneOS</string></array>
    <key>MinimumOSVersion</key><string>$MinimumOs</string>
</dict>
</plist>
"@
    [System.IO.File]::WriteAllText((Join-Path $framework 'Info.plist'), $plist)
}
$bootImage = Join-Path $Generated 'RMGE01_boot_image.bin'

# --- 4. Add to the IPA ----------------------------------------------------
if ($Ipa) {
    Step 'Adding the modules to the IPA'
    $outputIpa = Join-Path $Out 'Nebula-SMG.ipa'
    Copy-Item -Force (Resolve-Path $Ipa).Path $outputIpa
    $zip = [System.IO.Compression.ZipFile]::Open($outputIpa, [System.IO.Compression.ZipArchiveMode]::Update)
    try {
        $app = $zip.Entries | Where-Object { $_.FullName -match '^Payload/[^/]+\.app/Info\.plist$' } | Select-Object -First 1
        if (-not $app) { throw "$Ipa has no Payload/<name>.app" }
        $appPrefix = $app.FullName.Substring(0, $app.FullName.Length - 'Info.plist'.Length)
        $add = {
            param([string]$file, [string]$entryName, [int]$mode)
            $existing = $zip.GetEntry($entryName)
            if ($existing) { $existing.Delete() }
            $entry = [System.IO.Compression.ZipFileExtensions]::CreateEntryFromFile(
                $zip, $file, $entryName, [System.IO.Compression.CompressionLevel]::Optimal)
            # Unix permissions, so the binaries stay executable on the device.
            $entry.ExternalAttributes = ((0x8000 -bor $mode) -shl 16)
        }
        foreach ($framework in Get-ChildItem $frameworks -Directory) {
            foreach ($file in Get-ChildItem $framework.FullName -File) {
                $mode = if ($file.Name -eq 'Info.plist') { 0x1A4 } else { 0x1ED }  # 0644 / 0755
                & $add $file.FullName "${appPrefix}Frameworks/$($framework.Name)/$($file.Name)" $mode
            }
        }
        if (Test-Path $bootImage) {
            & $add $bootImage "${appPrefix}RMGE01_boot_image.bin" 0x1A4
        } else {
            Write-Warning "No RMGE01_boot_image.bin in $Generated; the game will not boot without it."
        }
        if ($BundleContent) {
            if (-not $content) { throw '-BundleContent needs -Game (the content is packaged from it)' }
            Step 'Adding the game content to the IPA'
            foreach ($file in Get-ChildItem $content -Recurse -File) {
                $relative = $file.FullName.Substring($content.Length + 1).Replace('\', '/')
                & $add $file.FullName "${appPrefix}content/$relative" 0x1A4
            }
        }
    } finally {
        $zip.Dispose()
    }
    Write-Host "IPA: $outputIpa" -ForegroundColor Green
} else {
    Write-Host "Frameworks: $frameworks" -ForegroundColor Green
}
if ($content -and -not $BundleContent) {
    Write-Host "Game content: $content (copy it to Nebula's folder on the device as 'content' with the Files app)"
}
