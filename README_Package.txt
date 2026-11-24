BG3 Memory Fix (00_BG3MemFix.dll) — Universal AOB Edition
===========================================================
GitHub: https://github.com/Alyciazsa/BG3-Memory-Fix
License: MIT

What This Fixes
---------------
Baldur's Gate 3 has a hardcoded 5.375 GiB (5,504 MiB, 16 chunks max) ceiling
inside its engine allocator (`ls::GlobalAllocator`). Heavy modlists (300-600+
mods) or long sessions hit this wall during save loading (stuck/crashing around
75%-100%) or Act 3 / heavy combat—even when your PC still has plenty of free RAM.

`00_BG3MemFix.dll` applies two runtime allocator fixes (and nothing else):
  - Layer 1: Pre-initializes `ls::GlobalAllocator` chunk[0] with a 16 GiB virtual
             address reservation committed on demand in 4 MiB increments (zero
             extra physical RAM consumed until the game actually allocates it).
  - Layer 2: Adds a `Pool_Alloc` -> `Large_Alloc` spill safety net if all 16
             chunks ever approach exhaustion.

Compatibility
-------------
  - Editions:  Steam and GOG (any version whose allocator machine code matches
               all 6 AOB signatures + internal call-graph & struct checks).
  - Renderers: Vulkan (bg3.exe) and DirectX 11 (bg3_dx11.exe).
  - Modes:     All difficulties and modes (Explorer, Balanced, Tactician, Honour
               Mode, Custom, Single-player and Multiplayer).
  - Runtime:   Windows 10/11 x64. Depends strictly on `KERNEL32.DLL` only (no VC++
               Redistributable needed by this DLL itself, though Native Mod Loader
               requires VC++ 2015-2022 x64).

Prerequisites
-------------
  1. Native Mod Loader (https://www.nexusmods.com/baldursgate3/mods/944)
     installed in `<BG3 Install Folder>\bin\` (`bink2w64.dll` +
     `bink2w64_original.dll`).
  2. Windows Pagefile: Keep Windows "Automatically manage paging file size for
     all drives" (System Managed on SSD). Do NOT disable the pagefile; you do
     NOT need a manual 30+ GB fixed pagefile.

Installation
------------
  1. Close Baldur's Gate 3 completely.
  2. (Optional) Verify SHA-256 against `SHA256.txt`:
       PowerShell:  Get-FileHash .\bin\NativeMods\00_BG3MemFix.dll -Algorithm SHA256
  3. Extract `bin\NativeMods\00_BG3MemFix.dll` into:
       `<BG3 Install Folder>\bin\NativeMods\00_BG3MemFix.dll`
     IMPORTANT: Keep the `00_` filename prefix! Native Mod Loader loads
     `NativeMods\*.dll` alphabetically, so `00_` ensures Layer 1 initializes
     before any other native mod triggers BG3's allocator.
  4. Launch Baldur's Gate 3 normally (Vulkan or DX11).

Verify It Is Working
--------------------
Open `<BG3 Install Folder>\bin\NativeMods\BG3MemFix.log` after launching:
  [...] ATTACH bg3.exe Mode=UniversalAOB ... ScanMs=...
  [...]   ResolvedRVAs ...
  [...]   Layer1=ACTIVE PreAttachGA=0x0 ... ReservedMiB=16384 InitCommittedMiB=8
  [...]   Layer2=ACTIVE

Note: If BG3 updates in the future and breaks a signature, the DLL refuses to
patch anything, logs the exact reason to `BG3MemFix.log`, and exits cleanly.

Uninstall
---------
Delete `00_BG3MemFix.dll` (and optionally `BG3MemFix.log` / `BG3MemFix-prev.log`)
from `<BG3 Install Folder>\bin\NativeMods\`.
