# BG3 Memory Fix — Universal AOB `ls::GlobalAllocator` Patch (`00_BG3MemFix.dll`)

[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)](LICENSE)
[![Latest Release](https://img.shields.io/github/v/release/Alyciazsa/BG3-Memory-Fix?label=Release&color=brightgreen)](https://github.com/Alyciazsa/BG3-Memory-Fix/releases/latest)
[![Nexus Mods #26927](https://img.shields.io/badge/Nexus%20Mods-%2326927-DA8E35.svg)](https://www.nexusmods.com/baldursgate3/mods/26927)
[![Platform Windows x64](https://img.shields.io/badge/Platform-Windows%2010%2F11%20x64-0078D4.svg)](https://www.microsoft.com/windows)
[![Renderers Vulkan & DX11](https://img.shields.io/badge/Renderers-Vulkan%20%7C%20DirectX%2011-success.svg)](#-universal-support--compatibility)
[![Storefronts Steam & GOG](https://img.shields.io/badge/Editions-Steam%20%7C%20GOG-informational.svg)](#-universal-support--compatibility)
[![Dependencies KERNEL32 Only](https://img.shields.io/badge/Imports-KERNEL32.dll%20Only-orange.svg)](SECURITY.md)
[![Built with Google Antigravity](https://img.shields.io/badge/Built%20with-Google%20Antigravity-4285F4.svg)](https://antigravity.google)

A lightweight, open-source C++20 native engine plugin for **Baldur's Gate 3** that lifts Larian Engine's hardcoded **`5.375 GiB` (`5,504 MiB` / `16`-chunk)** `ls::GlobalAllocator` memory ceiling—stopping the infamous **75%–100% save-loading crashes** and **Act 3 / heavy combat out-of-memory CTDs** on heavily modded setups (300–700+ mods), even when your PC still has plenty of free RAM.

> 🔗 **Nexus Mods Page:** [BG3 Memory Fix — Universal AOB (#26927)](https://www.nexusmods.com/baldursgate3/mods/26927)  
> 📦 **Companion Tool for Multiplayer Groups:** [BG3ModSync — Zero-Copy Nginx HTTP, Steam P2P & VPN Edition](https://github.com/Alyciazsa/BG3-Mod-Sync-Utility) ([Nexus #25251](https://www.nexusmods.com/baldursgate3/mods/25251)) — synchronizes `.pak` mods, `modsettings.lsx`, Script Extender JSONs, and `NativeMods` DLLs (including `00_BG3MemFix.dll`) directly between Host and Clients.

---

## 📋 Overview & Capabilities

| Feature | Stock `bg3.exe` / `bg3_dx11.exe` | With **`00_BG3MemFix.dll`** (`v1.0.0`) |
| :--- | :--- | :--- |
| **`ls::GlobalAllocator` Ceiling** | **Fixed `5.375 GiB` (`5,504 MiB`)** (`chunk[0] = 1,664 MiB` + `15 × 256 MiB`) | **`16 GiB` Virtual `chunk[0]`** + **`Pool_Alloc` -> `Large_Alloc` spill net** |
| **Physical RAM Behavior** | Eagerly allocates new `256 MiB` OS chunks until `chunkCount == 16` aborts | **Zero extra physical RAM used upfront** — commits lazily in `4 MiB` steps on demand |
| **Editions & Renderers** | — | **Universal AOB:** Steam & GOG, **Vulkan (`bg3.exe`)** & **DirectX 11 (`bg3_dx11.exe`)** |
| **Game Modes** | Crashes identically in all modes on heavy modlists | **All Modes:** Explorer, Balanced, Tactician, **Honour Mode**, Custom, SP & **Multiplayer** |
| **External DLL Dependencies** | — | **`KERNEL32.DLL` only** (zero VC++ Redist needed by DLL; zero network/telemetry) |
| **Fail-Safe Guard** | — | Verifies 6 unique AOBs + PE bounds + `+0xE8` struct layout + call graph before patching |

---

## 🔗 Prerequisites & Ecosystem Links

### Required (Must Be Installed)

1. **[Baldur's Gate 3 Native Mod Loader (Nexus #944)](https://www.nexusmods.com/baldursgate3/mods/944)** by *kassent*
   - Required to load plugins placed inside `Baldurs Gate 3\bin\NativeMods\*.dll`.
   - Follow its instructions to place `bink2w64.dll` and `bink2w64_original.dll` inside `<BG3 Install Folder>\bin\`.
2. **[Microsoft Visual C++ 2015–2022 Redistributable (x64)](https://aka.ms/vs/17/release/vc_redist.x64.exe)**
   - Required by **Native Mod Loader** (`bink2w64.dll`). (`00_BG3MemFix.dll` itself compiles with `/MT /GS-` and depends **only on Windows `KERNEL32.DLL`**.)
3. **Windows Pagefile Enabled (Default "System Managed")**
   - Keep Windows **Automatically manage paging file size for all drives** on an SSD/NVMe. See the [Pagefile FAQ](#-faq-game-modes-pagefile-16-gb-ram--what-it-fixes) below — **you do NOT need a 30+ GB manual pagefile**.

### Recommended Companion Tools (Optional, 100% Compatible)

- **[Norbyte's BG3 Script Extender (BG3SE)](https://github.com/Norbyte/bg3se)** — Lua/Osiris modding framework (`DWrite.dll`); works seamlessly alongside `00_BG3MemFix.dll`.
- **[LaughingLeader's BG3 Mod Manager (BG3MM)](https://github.com/LaughingLeader/BG3ModManager)** — Essential load-order manager for large modlists.
- **[BG3ModSync](https://github.com/Alyciazsa/BG3-Mod-Sync-Utility)** — Automatically syncs `bin\NativeMods\00_BG3MemFix.dll` and your entire modpack to co-op friends.

---

## 📌 Forensic Proof: Why This Crash Comes From the Game Engine Itself

Online discussions often assume PC BG3 has *"no hard engine limit"* and blame 75%–100% save-load crashes solely on bad mods, Script Extender mismatches, or running out of 16 GB/32 GB physical RAM.

Direct binary inspection of stock **`bg3.exe` (Vulkan, `TimeDateStamp=0x6AB1367E`)** and **`bg3_dx11.exe` (DirectX 11, `TimeDateStamp=0x6AB1357D`)** (Patch 8, `v4.76.31.656`) proves that Larian's `ls::GlobalAllocator` has a **hardcoded `5.375 GiB` (`5,504 MiB`) arena ceiling compiled directly into the executable**.

### 1. Exact Engine Constants & Machine-Code Proof

When `GA_Get` (`RVA 0x04120630` Vulkan / `0x040EA160` DX11) initializes `gGlobalAllocatorStorage` (`0xB40` bytes):

1. **Initial `chunk[0]` size is hardcoded to `1,664 MiB` (`1.625 GiB`) in `.rdata`:**
   - At `GA_Get + 0x4C`: `movdqa xmm0, xmmword ptr [rip + disp]` loads the `16`-byte constant from `.rdata` (`RVA 0x05B9E660` in Vulkan `bg3.exe`):
     ```text
     0x0000000068000000  =  1,744,830,464 bytes  =  1,664 MiB  (13,312 superpages of 128 KiB)
     ```
2. **Additional chunks (`chunk[1]`..`chunk[15]`) are hardcoded to `256 MiB` each:**
   - Inside `GA_AllocSuperpage` (`RVA 0x041A6890` Vulkan / `0x04170480` DX11):
     ```asm
     +0x5B: 83 BE C8 00 00 00 10     cmp dword ptr [rsi+0C8h], 10h   ; if (chunkCount >= 16)
     +0x6E: 0F 8D 8C 00 00 00        jge fatal_oom                   ; abort / crash!
     ...
     +0x97: BA 00 00 02 00           mov edx, 20000h                 ; 128 KiB superpage align
     +0x9C: 41 B8 00 00 00 10        mov r8d, 10000000h              ; 256 MiB chunk size
     +0xBB: 48 81 46 38 00 08 00 00  add qword ptr [rsi+38h], 800h   ; +2,048 superpages
     +0xC3: 48 89 7C CE 48           mov [rsi+rcx*8+48h], rdi        ; chunks[chunkCount] = ptr
     +0xC8: FF 86 C8 00 00 00        inc dword ptr [rsi+0C8h]        ; ++chunkCount
     ```
3. **Hard mathematical ceiling:**
   ```text
   1 chunk[0] (1,664 MiB) + 15 overflow chunks × 256 MiB (3,840 MiB)
   = 5,504 MiB
   = 5.375 GiB MAX
   ```

The moment a heavy modlist (especially 400–600+ `.pak`s with large stat/template/texture/UI pools, loaded saves in Act 2/3, or Script Extender tables) pushes `ls::GlobalAllocator` over **5,504 MiB**, **`chunkCount` hits `16`** (`cmp [rsi+0xC8], 0x10` branches to `fatal_oom`), and **the game crashes unconditionally**—even on a 32 GB or 64 GB RAM PC with >20 GB still completely free.

### 2. `ls::GlobalAllocator` Binary Layout (`0xB40` Bytes)

| Offset | Field | Meaning |
| :--- | :--- | :--- |
| `+0x028` | `curSuperpage` | Current `128 KiB` superpage pointer |
| `+0x030` | `altSuperpage` | Alternate superpage pointer |
| `+0x038` | `remSuperpages` | Remaining superpages in active arena (`size >> 17`) |
| `+0x040` | `altRemSuperpages` | Alternate remaining superpages |
| `+0x048` | `chunks[16]` | Fixed 16-slot chunk pointer table (`+0x48..+0xC7`) |
| `+0x0C8` | `chunkCount` | Active chunk count (**hard limit `16` = `0x10`**) |
| `+0x0D0` | `chunk0Size` | Size in bytes of initial chunk (`1,664 MiB` stock -> **`16 GiB` patched**) |
| `+0x0E8` | `PoolManager` | Embedded small-pool allocator subobject |

### 3. Verify It Yourself (30 Seconds in Any Disassembler)

Anyone with **Ghidra**, **IDA**, or **x64dbg** can verify this directly on a clean unmodded `bg3.exe`:
1. Search executable memory for the AOB signature of `GA_AllocSuperpage`:
   `48 89 5C 24 10 56 48 83 EC 20 48 8B F1 FF 15 ?? ?? ?? ?? 85 C0 75 ?? 48 8B CE`
2. Inspect offset `+0x5B`: you will see `83 BE C8 00 00 00 10` (`cmp dword ptr [rsi+0xC8], 16`) followed by `jge` into the fatal OOM path, and `+0x9C`: `41 B8 00 00 00 10` (`256 MiB` chunk size).
3. Trace `Pool_BindBackend` (`... 49 BA 20 00 00 11 AD DE CE FA`) -> caller `GA_Get`: inspect `+0x4C` `.rdata` load to see `0x0000000068000000` (`1,664 MiB`).

### 4. Live In-Game Proof

Testing on a **621-mod Baldur's Gate 3 playthrough** that reliably crashed at **100% save load**:
- **Before fix (stock `bg3.exe`):** Crashed every time during save load once `ls::GlobalAllocator` hit its `5.375 GiB` (`5,504 MiB`) 16-chunk ceiling.
- **With `00_BG3MemFix.dll` attached:**
  - `BG3MemFix.log` confirmed `Layer1=ACTIVE` (`ReservedMiB=16384 InitCommittedMiB=8`) and `Layer2=ACTIVE`.
  - Live save loaded immediately past 100%, peaking at **`5,976 MiB` (`5.84 GiB`)** committed in `chunk[0]` — cleanly crossing the stock **`5,504 MiB`** crash wall with **`CommitFailures=0`** and remaining completely stable across extended gameplay.

---

## 🛠️ How `00_BG3MemFix.dll` Works

### Two-Layer Allocator Architecture

1. **Layer 1 — Lazy `16 GiB` `chunk[0]` Pre-Initialization + On-Demand Commit (`GA_AllocSuperpage` hook):**
   - Runs at `DLL_PROCESS_ATTACH` **before** BG3 initializes `ls::GlobalAllocator` (`PreAttachGA=0x0` — why the **`00_` filename prefix** matters!).
   - Reserves `16 GiB` (`16,384 MiB`) of `128 KiB`-aligned **virtual address space** (`MEM_RESERVE`, `PAGE_NOACCESS`), which uses **0 MB of physical RAM**.
   - Initializes `chunk[0]` inside `gGlobalAllocatorStorage` with `chunk0Size = 16 GiB`, constructs `PoolManager`, binds the backend, and publishes `*gGlobalAllocator = gGA`.
   - Hooks `GA_AllocSuperpage`: whenever BG3 hands out a superpage inside `chunk[0]`, `EnsureSuperpageCommitted()` commits virtual memory on demand in **`4 MiB` increments** (`SRWLOCK` slow path + lock-free acquire/release high-water mark fast path). Zero CRT heap allocations or file I/O occur inside hooks.
2. **Layer 2 — `Pool_Alloc` Overflow Net:**
   - Hooks `Pool_Alloc`: if all `16` chunks ever approach exhaustion (`chunkCount >= 16`, `remSuperpages < 256` [`< 32 MiB`], `altRemSuperpages == 0`), small pool allocations (`size <= 0x800`, `align <= 0x40`) cleanly latch and spill over to `Large_Alloc` instead of faulting.

### Universal AOB Engine (Why It Survives Store Editions, Renderers & Hotfixes)

Instead of hardcoding version-locked RVAs (`TimeDateStamp` / `SizeOfImage`), `00_BG3MemFix.dll` enumerates all executable PE sections (`IMAGE_SCN_MEM_EXECUTE`) at attach time and runs 6 unique byte-signature scans plus structural verification:

<details>
<summary><b>▶ Click to expand: Universal AOB Signatures & Cross-Verification Chain</b></summary>

<br>

1. Scans `.text` / executable PE sections using `std::memchr` rare-byte anchoring (`FindPatternUniqueInExecSections`) for 6 signatures, rejecting `NotFound` OR `Ambiguous` (`>= 2` matches):
   - `GA_Get` (66 bytes)
   - `GA_AllocSuperpage` (60 bytes)
   - `PoolManager_Ctor` (67 bytes)
   - `Pool_BindBackend` (104 bytes, anchored on sentinel constant `0xFACEDEAD11000020`)
   - `Pool_Alloc` (79 bytes)
   - `Large_Alloc` (109 bytes)
2. Extracts RIP-relative globals inside `GA_Get`:
   - `addrGlobalAllocator` (`+0x04`) and verifies it falls within `[ImageBase, ImageBase + SizeOfImage)`.
   - `addrGlobalAllocatorStorage` (`+0x28`) and verifies `0xB40` bytes fit within `[ImageBase, ImageBase + SizeOfImage)`.
   - `PoolManager` (`+0x3B`) and asserts `PoolManager - addrGlobalAllocatorStorage == 0xE8`.
3. Confirms `cmp dword ptr [rsi+0xC8], 16` (`83 BE C8 00 00 00 10`) inside `GA_AllocSuperpage`.
4. Verifies all three internal `E8 rel32` call edges (`GA_Get -> PoolManager_Ctor`, `GA_Get -> Pool_BindBackend`, `Pool_BindBackend -> GA_AllocSuperpage`) and exact 5-byte hook prologues (`48 89 5C 24 10` / `48 89 5C 24 08`).
5. Allocates a near-image `PAGE_EXECUTE_READ` relay page within `±2 GiB` (`AllocateNearRelay`) before patching `5`-byte `E9` detours.

</details>

---

## 🚀 Step-by-Step Installation Guide

1. **Install Prerequisites:**
   - Install **[Native Mod Loader (Nexus #944)](https://www.nexusmods.com/baldursgate3/mods/944)** into `<BG3 Install Folder>\bin\` (`bink2w64.dll` + `bink2w64_original.dll`).
   - Make sure **[Visual C++ 2015–2022 Redistributable (x64)](https://aka.ms/vs/17/release/vc_redist.x64.exe)** is installed (required by Native Mod Loader).
2. **Download `BG3MemFix`:**
   - Grab `BG3MemFix_v1.0.0_Universal.zip` from **[GitHub Releases](https://github.com/Alyciazsa/BG3-Memory-Fix/releases/latest)** or **[Nexus Mods #26927](https://www.nexusmods.com/baldursgate3/mods/26927)**.
3. **Extract to Your BG3 `bin\NativeMods\` Folder:**
   - Make sure **Baldur's Gate 3 is closed**.
   - Place `00_BG3MemFix.dll` at:
     ```text
     <BG3 Install Folder>\bin\NativeMods\00_BG3MemFix.dll
     ```
     *(Common Steam path: `C:\Program Files (x86)\Steam\steamapps\common\Baldurs Gate 3\bin\NativeMods\00_BG3MemFix.dll`)*
   - **IMPORTANT — Do NOT rename the file:** Keep the **`00_`** prefix! On Windows NTFS folders, Native Mod Loader iterates `NativeMods\*.dll` alphabetically; `00_` ensures `00_BG3MemFix.dll` initializes `ls::GlobalAllocator` **before** any other native plugin triggers engine allocations (`PreAttachGA=0x0`).
4. **Launch Baldur's Gate 3:**
   - Play normally using either **Vulkan** (`bg3.exe`) or **DirectX 11** (`bg3_dx11.exe`).

### Verify It Is Working

Open `<BG3 Install Folder>\bin\NativeMods\BG3MemFix.log` after launching the game:

```text
[2026-11-24 23:30:00] ATTACH bg3.exe Mode=UniversalAOB Base=0x00007FF620000000 TS=0x6AB1367E Size=0x0683F000 ScanMs=22 Relay=0x00007FF61FF80000
[2026-11-24 23:30:00]   ResolvedRVAs GA_Get=0x04120630 Superpage=0x041A6890 PoolCtor=0x041BD160 PoolBind=0x041BD750 PoolAlloc=0x041BEB10 LargeAlloc=0x041ABD70 GlobalPtr=0x062D4930 GlobalBuf=0x063D88A0
[2026-11-24 23:30:00]   Layer1=ACTIVE PreAttachGA=0x0 PostAttachGA=0x00007FF6263D88A0 Chunk0=0x0000020000000000 ReservedMiB=16384 InitCommittedMiB=8
[2026-11-24 23:30:00]   Layer2=ACTIVE
```

*(What to look for: `Layer1=ACTIVE PreAttachGA=0x0` and `Layer2=ACTIVE`. Note that if another DLL initialized `ls::GlobalAllocator` first so `PreAttachGA != 0`, `Layer1` safely reports `SKIPPED` while `Layer2` stays `ACTIVE` — double-check your `00_` prefix.)*

### Uninstall

Close BG3 and delete `<BG3 Install Folder>\bin\NativeMods\00_BG3MemFix.dll` (and optionally `BG3MemFix.log` / `BG3MemFix-prev.log`). No save files or `.pak` mods are ever altered.

---

## ❓ FAQ: Game Modes, Pagefile, 16 GB RAM & What It Fixes

### 1. Is this only for Honour Mode, or all game modes?
**All modes and all difficulties:** Explorer, Balanced, Tactician, **Honour Mode**, Custom difficulty, Single-player, and Co-op **Multiplayer**.
- Does **not** break multiplayer checksums (`.pak` / `modsettings.lsx` are untouched). Friends on heavy modlists can also install `00_BG3MemFix.dll` locally so their game doesn't crash on save load either.

### 2. Do I still need a 30+ GB manual Windows Pagefile, or is System Default enough?
**Windows Default ("System Managed Size" on an SSD/NVMe) is all you need.**
- Because `Layer 1` reserves `16 GiB` virtually (`MEM_RESERVE`) and commits memory **only in `4 MiB` steps as BG3 actually asks for it**, it does **not** lock up 16 GB or 30 GB of RAM or pagefile immediately on startup.
- As long as **System Managed Pagefile** is enabled on an SSD with free disk space (`15–30 GB+` free space recommended), Windows automatically expands the commit limit whenever peak virtual commit + standby needs headroom—without needing a permanent `32 GB` static `pagefile.sys` wasting SSD space.
- **Only rule:** **Never disable (`No paging file`) the Windows pagefile.** Even with 32 GB+ physical RAM, disabling the pagefile prevents Windows from backing virtual commit peaks and causes sudden allocation failures under heavy load.

### 3. Is this safe on a 16 GB RAM PC?
Yes, with caveats:
- Physical RAM used = **exact memory BG3 + Windows actually touch**, nothing extra.
- With **16 GB RAM** + a heavy modlist where BG3 commits `6–8+ GiB` and Windows + GPU + Discord/browser are running, Windows will page inactive background pages to your **SSD pagefile** instead of crashing BG3 at `5.375 GiB`. Closing heavy browser tabs helps keep frame times smooth if physical RAM fills up.

### 4. Which editions & updates are supported?
- **Steam & GOG**, **Vulkan (`bg3.exe`)** and **DirectX 11 (`bg3_dx11.exe`)**, tested on Patch 8 (`v4.76.31.656`).
- Because it scans executable PE sections dynamically using 6 AOB signatures + structural cross-checks, it works automatically across stores and survives minor hotfixes that only shift addresses. If a major future patch alters Larian's allocator code, it **fails closed**, logs the reason in `BG3MemFix.log`, and refuses to patch rather than risking corruption.

### 5. What does this mod NOT fix?
- Does **not** fix incompatible/outdated `.pak` mods, missing dependencies, or Lua/Script Extender bugs.
- Does **not** fix BG3's **`6x` consecutive self-`using` stat inheritance crash** before the main menu when multiple overhaul mods overwrite the exact same stat entries too deeply.

---

## 📁 Repository Structure

```text
BG3-Memory-Fix/
├── src/
│   ├── CommonHelpers.h         # Header-only Win32 PE section parser, AOB scanner, RIP resolver & near relay
│   ├── MemFixHelpers.h         # 6 universal AOBs + cross-verification, on-demand 4 MiB commit & Layer 2 spill logic
│   └── BG3MemFix.cpp           # DllMain + Hook_GA_AllocSuperpage + Hook_Pool_Alloc + BG3MemFix.log writer
├── tests/
│   └── test_memfix.cpp         # Offline unit tests + synthetic PE AOB negative tests + live DLL & real PE scan tests
├── build.ps1                   # MSVC C++20 /W4 /WX build, dumpbin import verification, test runner & ZIP packager
├── README.md                   # Documentation, binary proof & FAQ
├── SECURITY.md                 # Security guarantees & private reporting policy
├── CHANGELOG.md                # Release changelog
├── NEXUS_DESCRIPTION.md        # Ready-to-use Nexus Mods description page
├── README_Package.txt          # Plain-text README bundled inside release ZIPs
├── LICENSE                     # MIT License (Copyright 2026 Alyciazsa)
└── .gitignore                  # Excludes build/, release archives & IDE artifacts
```

---

## 🛠️ Building from Source

### Prerequisites
- **OS:** Windows 10 / Windows 11 (`x64`)
- **Compiler:** Visual Studio 2022 / Build Tools (`MSVC x64 C++20` toolset + Windows 10/11 SDK) — zero external libraries or vcpkg packages required.

### Build, Verify & Package

```powershell
# Auto-discovers MSVC/SDK via vswhere.exe, compiles with /std:c++20 /O2 /MT /W4 /WX /GS-,
# verifies KERNEL32.DLL-only imports via dumpbin, runs offline tests (+ real BG3 binary scan if installed),
# and writes release\BG3MemFix_v1.0.0_Universal.zip
pwsh -NoProfile -File .\build.ps1
```

To explicitly point the real PE scan test at a custom BG3 `bin\` directory (Steam or GOG):

```powershell
pwsh -NoProfile -File .\build.ps1 -Bg3BinDir "D:\Games\Baldurs Gate 3\bin"
```

---

## 📚 Credits & Acknowledgements

1. **[AlyciaX / @Alyciazsa](https://github.com/Alyciazsa)** — Root-cause reverse engineering of `ls::GlobalAllocator`'s `5.375 GiB` ceiling & implementation ([Nexus Mods Profile](https://www.nexusmods.com/profile/AlyciaXY), [BG3ModSync](https://github.com/Alyciazsa/BG3-Mod-Sync-Utility)).
2. **[kassent](https://www.nexusmods.com/baldursgate3/mods/944)** — Creator of *Baldur's Gate 3 Native Mod Loader* (`bink2w64.dll`), enabling native C++ plugins inside `bin\NativeMods\`.
3. **[Norbyte](https://github.com/Norbyte/bg3se)** — *BG3 Script Extender* & open-source BG3 engine research ecosystem.
4. **[Google Antigravity](https://antigravity.google)** — Built with AI-assisted development using Google Antigravity (human-directed architecture, testing, and verification).

---

## ⚖️ License & Disclaimer

Distributed under the [MIT License](LICENSE).

*This project is an independent open-source community fix and is not affiliated with, endorsed by, or associated with Larian Studios. Although read-only fail-closed signature verification prevents patching unrecognized game versions, always keep backups of your save files.*
