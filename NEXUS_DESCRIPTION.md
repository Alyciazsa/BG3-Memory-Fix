[size=5][b]BG3 Memory Fix — Universal `ls::GlobalAllocator` Patch (`00_BG3MemFix.dll`)[/b][/size]

[b]Fixes Baldur's Gate 3 crashing at 75%–100% save load or in Act 3 / heavy combat on large modlists (300–700+ mods) — even when your PC still has plenty of free RAM.[/b]

[list]
[*][b]Universal Support:[/b] Works on [b]Steam & GOG[/b], both [b]Vulkan (`bg3.exe`)[/b] and [b]DirectX 11 (`bg3_dx11.exe`)[/b], via executable-section AOB pattern scanning + structural cross-checks.
[*][b]All Game Modes:[/b] Explorer, Balanced, Tactician, [b]Honour Mode[/b], Custom, Single-player, and [b]Multiplayer[/b] (does not modify saves or `.pak` checksums).
[*][b]Zero Extra Physical RAM Wasted:[/b] Reserves a `16 GiB` virtual arena lazily committed on demand in `4 MiB` increments. Uses **default Windows System Managed pagefile on an SSD** (no 30 GB manual pagefile needed!).
[*][b]100% Open Source (MIT) & Verifiable:[/b] Full C++20 source code, automated tests, and pre-built releases on [url=https://github.com/Alyciazsa/BG3-Memory-Fix]GitHub (`Alyciazsa/BG3-Memory-Fix`)[/url]. Depends strictly on Windows `KERNEL32.DLL` only — zero network calls, zero telemetry, zero VC++ runtime DLLs required by the plugin itself.
[/list]

[line]

[size=4][b]Requirements (Prerequisites)[/b][/size]

[list=1]
[*][url=https://www.nexusmods.com/baldursgate3/mods/944][b]Baldur's Gate 3 Native Mod Loader (Nexus #944)[/b][/url] — Required to load DLL plugins from `Baldurs Gate 3\bin\NativeMods\`.
[*][url=https://aka.ms/vs/17/release/vc_redist.x64.exe][b]Visual C++ 2015–2022 Redistributable (x64)[/b][/url] — Required by Native Mod Loader (`bink2w64.dll`).
[*][b]Windows Pagefile Enabled (Default System Managed on SSD)[/b] — Do [b]not[/b] disable the Windows pagefile (`No paging file`). Leave Windows on [i]Automatically manage paging file size for all drives[/i].
[/list]

[line]

[size=4][b]Proof That This Crash Is Hardcoded Inside `bg3.exe`[/b][/size]

Community guides often blame 100% load crashes on corrupt saves or "too many mods", assuming PC BG3 has no allocator limit. In reality, Larian's `ls::GlobalAllocator` inside `bg3.exe` / `bg3_dx11.exe` (verified on Patch 8 `v4.76.31.656`) enforces a [b]hardcoded `5.375 GiB` (`5,504 MiB` / `16`-chunk) ceiling[/b]:

[code]
GA_Get (.rdata constant):       chunk[0]   = 0x0000000068000000 (1,664 MiB)
GA_AllocSuperpage (+0x5B):      83 BE C8 00 00 00 10   -> cmp dword ptr [rsi+0xC8], 16
                                0F 8D 8C 00 00 00      -> jge fatal_oom  (CRASH!)
GA_AllocSuperpage (+0x9C):      41 B8 00 00 00 10      -> chunk[1..15] = 256 MiB each

Maximum allocator capacity = 1,664 MiB + (15 * 256 MiB) = 5,504 MiB = 5.375 GiB
[/code]

Once a heavy modlist pushes `ls::GlobalAllocator` past `5.375 GiB`, `chunkCount` hits `16` and the engine aborts unconditionally—even on a 32 GB or 64 GB PC.
With `00_BG3MemFix.dll`, live `bg3.exe` cleanly commits past the `5,504 MiB` wall (verified crossing `5,976 MiB` / `5.84 GiB` with `0` commit failures on a 621-mod setup).

[line]

[size=4][b]How It Works[/b][/size]

[list]
[*][b]Layer 1 (`16 GiB` Virtual `chunk[0]` + Lazy `4 MiB` Commit):[/b] At attach time (`PreAttachGA=0x0`), reserves `16 GiB` of `128 KiB`-aligned virtual memory (`MEM_RESERVE`, zero physical RAM consumed until touched) and hooks `GA_AllocSuperpage` to commit `4 MiB` steps on demand.
[*][b]Layer 2 (`Pool_Alloc` Overflow Net):[/b] Hooks `Pool_Alloc` to safely spill small pool requests (`size <= 0x800`, `align <= 0x40`) to `Large_Alloc` if all 16 chunks ever near exhaustion.
[*][b]Fail-Closed Safety Guard:[/b] Scans executable PE sections dynamically for 6 unique signatures and verifies PE bounds, `+0xE8` `PoolManager` struct offset, `cmp [rsi+0xC8], 16`, and internal `E8` call edges. If a future breaking update doesn't match, it logs why in `BG3MemFix.log` and touches zero bytes.
[/list]

[line]

[size=4][b]Installation[/b][/size]

[list=1]
[*]Install [url=https://www.nexusmods.com/baldursgate3/mods/944]Native Mod Loader[/url] (`bink2w64.dll` + `bink2w64_original.dll` in `<BG3>\bin\`).
[*]Close Baldur's Gate 3 completely.
[*]Extract `bin\NativeMods\00_BG3MemFix.dll` into:
[code]<BG3 Install Folder>\bin\NativeMods\00_BG3MemFix.dll[/code]
[b]IMPORTANT: Keep the `00_` filename prefix![/b] Native Mod Loader loads DLLs alphabetically, and `00_` guarantees Layer 1 initializes [i]before[/i] other NativeMods trigger BG3's allocator.
[*]Launch Baldur's Gate 3 (Vulkan or DirectX 11).
[*]Check `<BG3 Install Folder>\bin\NativeMods\BG3MemFix.log` — you should see `Layer1=ACTIVE PreAttachGA=0x0` and `Layer2=ACTIVE`.
[/list]

[line]

[size=4][b]FAQ & Troubleshooting[/b][/size]

[b]Q: Does this work in Honour Mode or Multiplayer?[/b]
A: Yes — [b]all game modes[/b] (Explorer, Balanced, Tactician, Honour Mode, Custom) and both Single-player and Multiplayer.

[b]Q: Do I need to set a huge 30+ GB manual Pagefile?[/b]
A: No. Keep Windows on default [b]System Managed Size[/b] on an SSD with normal free space (`15–30 GB+`). Just never set your pagefile to [i]No paging file[/i].

[b]Q: Will this help on 16 GB RAM PCs?[/b]
A: Yes. It does not consume 16 GB of physical RAM — memory commits in `4 MiB` chunks strictly as BG3 needs it, and Windows pages inactive background allocations to your SSD pagefile cleanly instead of crashing at `5.375 GiB`. Close heavy browser tabs/apps on 16 GB systems for smoothest performance.

[b]Q: My `BG3MemFix.log` says `Layer1=SKIPPED` with `PreAttachGA!=0x0`![/b]
A: Make sure the file is still named [b]`00_BG3MemFix.dll`[/b] so it loads before other NativeMods, and make sure you deleted any older `00_BG3MemoryFix.dll` if you previously used an early test build. (`Layer2` remains active as a fallback.)

[b]Q: What does this NOT fix?[/b]
A: It only lifts BG3's `5.375 GiB` engine allocator limit. It cannot fix broken/outdated `.pak` mods, missing mod dependencies, or BG3's pre-main-menu `6x` recursive `using` stat override crash.

[line]

[b]Source Code & Checksums:[/b] [url=https://github.com/Alyciazsa/BG3-Memory-Fix]https://github.com/Alyciazsa/BG3-Memory-Fix[/url]
[b]Related Tool (Modpack Sync for Co-op):[/b] [url=https://www.nexusmods.com/baldursgate3/mods/25251]BG3 Mod Sync Utility (#25251)[/url]
