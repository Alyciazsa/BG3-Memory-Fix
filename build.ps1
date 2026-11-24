param(
    [string]$Bg3BinDir = ''
)

$ErrorActionPreference = 'Stop'

$Root       = $PSScriptRoot
$BuildDir   = Join-Path $Root 'build'
$ReleaseDir = Join-Path $Root 'release'
$StageDir   = Join-Path $ReleaseDir 'stage'
$ShareZip   = Join-Path $ReleaseDir 'BG3MemFix_v1.0.0_Universal.zip'

New-Item -ItemType Directory -Path $BuildDir   -Force | Out-Null
New-Item -ItemType Directory -Path $ReleaseDir -Force | Out-Null

function Resolve-MsvcToolchain {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    $vsRoots = @()
    if (Test-Path -LiteralPath $vswhere) {
        $found = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath 2>$null
        if ($LASTEXITCODE -eq 0 -and $found) {
            $vsRoots += $found.Trim()
        }
    }
    $vsRoots += @(
        'C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools',
        'C:\Program Files\Microsoft Visual Studio\2022\BuildTools',
        'C:\Program Files\Microsoft Visual Studio\2022\Community',
        'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools'
    )

    $clExe = $null
    $msvcRoot = $null
    foreach ($vr in $vsRoots) {
        if (-not $vr -or -not (Test-Path -LiteralPath $vr)) { continue }
        $vcTools = Join-Path $vr 'VC\Tools\MSVC'
        if (-not (Test-Path -LiteralPath $vcTools)) { continue }
        $latestVer = Get-ChildItem -LiteralPath $vcTools -Directory | Sort-Object Name -Descending | Select-Object -First 1
        if ($latestVer) {
            $candCl = Join-Path $latestVer.FullName 'bin\Hostx64\x64\cl.exe'
            if (Test-Path -LiteralPath $candCl) {
                $msvcRoot = $latestVer.FullName
                $clExe = $candCl
                break
            }
        }
    }
    if (-not $clExe) {
        throw 'MSVC x64 compiler (cl.exe) not found. Install Visual Studio Build Tools with C++ desktop workload.'
    }

    $sdkBase = 'C:\Program Files (x86)\Windows Kits\10'
    $sdkVer = Get-ChildItem -LiteralPath (Join-Path $sdkBase 'Include') -Directory |
        Where-Object { Test-Path -LiteralPath (Join-Path $_.FullName 'um\windows.h') } |
        Sort-Object Name -Descending |
        Select-Object -First 1
    if (-not $sdkVer) {
        throw 'Windows 10/11 SDK headers not found.'
    }

    return @{
        MsvcRoot   = $msvcRoot
        ClExe      = $clExe
        DumpbinExe = Join-Path $msvcRoot 'bin\Hostx64\x64\dumpbin.exe'
        SdkInc     = $sdkVer.FullName
        SdkLib     = Join-Path $sdkBase ("Lib\" + $sdkVer.Name)
    }
}

$tc = Resolve-MsvcToolchain
if (-not (Test-Path -LiteralPath $tc.DumpbinExe)) {
    throw "dumpbin.exe not found at $($tc.DumpbinExe)"
}

$env:INCLUDE = @(
    (Join-Path $tc.MsvcRoot 'include'),
    (Join-Path $tc.SdkInc 'ucrt'),
    (Join-Path $tc.SdkInc 'um'),
    (Join-Path $tc.SdkInc 'shared')
) -join ';'

$env:LIB = @(
    (Join-Path $tc.MsvcRoot 'lib\x64'),
    (Join-Path $tc.SdkLib 'ucrt\x64'),
    (Join-Path $tc.SdkLib 'um\x64')
) -join ';'

$env:PATH = (Join-Path $tc.MsvcRoot 'bin\Hostx64\x64') + ';' + $env:PATH

$MemDllOut = Join-Path $BuildDir '00_BG3MemFix.dll'
$MemObj    = Join-Path $BuildDir 'BG3MemFix.obj'
$TestOut   = Join-Path $BuildDir 'test_memfix.exe'
$TestObj   = Join-Path $BuildDir 'test_memfix.obj'

Write-Host '[1/5] Compiling 00_BG3MemFix.dll (Universal AOB Memory Allocator Fix)...'
& $tc.ClExe /nologo /std:c++20 /O2 /MT /W4 /WX /GS- /EHsc /LD `
    (Join-Path $Root 'src\BG3MemFix.cpp') `
    "/Fo$MemObj" `
    "/Fe$MemDllOut" `
    /link /NOLOGO kernel32.lib
if ($LASTEXITCODE -ne 0) {
    throw "Failed to compile 00_BG3MemFix.dll (exit $LASTEXITCODE)"
}

Write-Host '[2/5] Compiling test_memfix.exe...'
& $tc.ClExe /nologo /std:c++20 /O2 /MT /W4 /WX /EHsc `
    (Join-Path $Root 'tests\test_memfix.cpp') `
    "/Fo$TestObj" `
    "/Fe$TestOut" `
    /link /NOLOGO kernel32.lib
if ($LASTEXITCODE -ne 0) {
    throw "Failed to compile test_memfix.exe (exit $LASTEXITCODE)"
}

Write-Host '[3/5] Verifying x64 PE headers, zero external DLL deps, and string cleanliness...'
$hdrOut = (& $tc.DumpbinExe /NOLOGO /HEADERS $MemDllOut) -join "`n"
if ($LASTEXITCODE -ne 0 -or $hdrOut -notmatch '8664 machine \(x64\)' -or $hdrOut -notmatch 'File Type:\s+DLL') {
    throw "Unexpected PE header/architecture in $MemDllOut"
}
$depOut = & $tc.DumpbinExe /NOLOGO /DEPENDENTS $MemDllOut
if ($LASTEXITCODE -ne 0) {
    throw "dumpbin /DEPENDENTS failed"
}
if (($depOut -join "`n") -match 'delay load dependencies') {
    throw "Unexpected delay-load dependencies in $MemDllOut"
}
$deps = @($depOut | Where-Object { $_ -match '^\s+\S+\.dll\s*$' } | ForEach-Object { $_.Trim().ToUpperInvariant() })
Write-Host ("  00_BG3MemFix.dll: x64 DLL, deps=[{0}]" -f ($deps -join ', '))
if ($deps.Count -ne 1 -or $deps[0] -ne 'KERNEL32.DLL') {
    throw "Unexpected dependencies: $($deps -join ', ')"
}

$memText = [System.Text.Encoding]::ASCII.GetString([System.IO.File]::ReadAllBytes($MemDllOut))
foreach ($forbidden in @('Tooltip', 'Layer3', 'BG3TooltipFix')) {
    if ($memText.IndexOf($forbidden, [System.StringComparison]::OrdinalIgnoreCase) -ge 0) {
        throw "Unexpected forbidden string '$forbidden' in 00_BG3MemFix.dll"
    }
}

# Locate BG3 bin dir automatically for real PE scan if installed
if (-not $Bg3BinDir -and $env:BG3_BIN_DIR) {
    $Bg3BinDir = $env:BG3_BIN_DIR
}
if (-not $Bg3BinDir) {
    $candidates = @(
        'C:\Program Files (x86)\Steam\steamapps\common\Baldurs Gate 3\bin',
        'C:\Program Files (x86)\GOG Galaxy\Games\Baldurs Gate 3\bin',
        'C:\GOG Games\Baldurs Gate 3\bin',
        'D:\SteamLibrary\steamapps\common\Baldurs Gate 3\bin',
        'E:\SteamLibrary\steamapps\common\Baldurs Gate 3\bin'
    )
    $steamReg = (Get-ItemProperty 'HKCU:\Software\Valve\Steam' -ErrorAction SilentlyContinue).SteamPath
    if ($steamReg) {
        $steamNorm = $steamReg.Replace('/', '\')
        $candidates = @((Join-Path $steamNorm 'steamapps\common\Baldurs Gate 3\bin')) + $candidates
        $vdf = Join-Path $steamNorm 'steamapps\libraryfolders.vdf'
        if (Test-Path -LiteralPath $vdf) {
            foreach ($line in Get-Content -LiteralPath $vdf) {
                if ($line -match '"path"\s+"([^"]+)"') {
                    $lib = $Matches[1].Replace('\\', '\')
                    $candidates += (Join-Path $lib 'steamapps\common\Baldurs Gate 3\bin')
                }
            }
        }
    }
    foreach ($c in $candidates) {
        if (Test-Path -LiteralPath (Join-Path $c 'bg3.exe')) {
            $Bg3BinDir = $c
            break
        }
    }
}

Write-Host '[4/5] Running self-tests...'
if ($Bg3BinDir) {
    & $TestOut --bg3-bin $Bg3BinDir
} else {
    & $TestOut
}
if ($LASTEXITCODE -ne 0) {
    throw "test_memfix.exe failed (exit $LASTEXITCODE)"
}

Write-Host '[5/5] Packaging universal Nexus/GitHub release ZIP and verifying checksum...'
if (Test-Path -LiteralPath $StageDir) {
    Remove-Item -LiteralPath $StageDir -Recurse -Force
}
$nativeModsStage = Join-Path $StageDir 'bin\NativeMods'
New-Item -ItemType Directory -Path $nativeModsStage -Force | Out-Null

$memHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $MemDllOut).Hash
Copy-Item -LiteralPath $MemDllOut -Destination (Join-Path $nativeModsStage '00_BG3MemFix.dll') -Force
Copy-Item -LiteralPath (Join-Path $Root 'README_Package.txt') -Destination (Join-Path $StageDir 'README.txt') -Force
Copy-Item -LiteralPath (Join-Path $Root 'LICENSE') -Destination (Join-Path $StageDir 'LICENSE') -Force
"$memHash  bin/NativeMods/00_BG3MemFix.dll" | Set-Content -LiteralPath (Join-Path $StageDir 'SHA256.txt') -Encoding Ascii
"$memHash  00_BG3MemFix.dll" | Set-Content -LiteralPath (Join-Path $ReleaseDir 'SHA256.txt') -Encoding Ascii

if (Test-Path -LiteralPath $ShareZip) {
    Remove-Item -LiteralPath $ShareZip -Force
}
Compress-Archive -Path (Join-Path $StageDir '*') -DestinationPath $ShareZip -CompressionLevel Optimal
Remove-Item -LiteralPath $StageDir -Recurse -Force

Add-Type -AssemblyName System.IO.Compression.FileSystem
$zip = [System.IO.Compression.ZipFile]::OpenRead($ShareZip)
try {
    $entryNames = @($zip.Entries | Where-Object { -not $_.FullName.EndsWith('/') -and -not $_.FullName.EndsWith('\') } | ForEach-Object { $_.FullName.Replace('\', '/') } | Sort-Object)
    $expectedNames = @('bin/NativeMods/00_BG3MemFix.dll', 'LICENSE', 'README.txt', 'SHA256.txt')
    if (($entryNames -join ',') -ne ($expectedNames -join ',')) {
        throw "Unexpected ZIP entries: $($entryNames -join ', ')"
    }
    $dllEntry = $zip.GetEntry('bin/NativeMods/00_BG3MemFix.dll')
    if (-not $dllEntry) { $dllEntry = $zip.GetEntry('bin\NativeMods\00_BG3MemFix.dll') }
    $sha = [System.Security.Cryptography.SHA256]::Create()
    try {
        $stream = $dllEntry.Open()
        try {
            $zipDllHash = ([System.BitConverter]::ToString($sha.ComputeHash($stream))).Replace('-', '')
        } finally {
            $stream.Dispose()
        }
    } finally {
        $sha.Dispose()
    }
    if ($zipDllHash -ne $memHash) {
        throw "ZIP DLL hash mismatch: $zipDllHash != $memHash"
    }
} finally {
    $zip.Dispose()
}

Write-Host ''
Write-Host 'Build, PE verification, self-tests, and ZIP packaging succeeded:'
Write-Host "  DLL:    $MemDllOut (SHA256: $memHash)"
Write-Host "  Zip:    $ShareZip"
