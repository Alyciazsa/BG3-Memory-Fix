# Changelog

All notable changes to **BG3 Memory Fix (`00_BG3MemFix.dll`)** are documented in this file.

## [1.0.0] - 2026-11-24

### Added
- **Universal PE Executable-Section AOB Scanner (`FindExecutableSections`, `ResolveMemTargets`):**
  - Replaced hardcoded RVA tables and strict `TimeDateStamp` / `SizeOfImage` gates with 6 unique AOB signatures supporting both **Vulkan (`bg3.exe`)** and **DirectX 11 (`bg3_dx11.exe`)** across **Steam and GOG** installations and surviving minor game updates.
  - Enforces uniqueness (`ScanStatus::Found` with ambiguity rejection), PE image bounds checks, `+0xE8` `PoolManager` struct offset validation, `cmp dword ptr [rsi+0xC8], 16` check, and internal call-graph edge verification (`GA_Get -> PoolManager_Ctor`, `GA_Get -> Pool_BindBackend`, `Pool_BindBackend -> GA_AllocSuperpage`) before applying any patch.
- **Layer 1 (`16 GiB` Lazy Virtual `chunk[0]` + On-Demand Commit):**
  - Reserves a `16 GiB` `MEM_RESERVE` virtual arena aligned to `128 KiB` superpages and commits pages on-demand in `4 MiB` increments (`SRWLOCK` + acquire/release high-water mark), lifting the stock engine `5.375 GiB` (`5,504 MiB`) allocator ceiling without consuming physical RAM ahead of use.
- **Layer 2 (`Pool_Alloc` Overflow Net):**
  - Latches small pool allocations (`size <= 0x800`, `align <= 0x40`) to `Large_Alloc` if all 16 chunks ever near exhaustion (`remSuperpages < 256` and `altRemSuperpages == 0`).
- **Diagnostic Telemetry (`BG3MemFix.log` / `BG3MemFix-prev.log`):**
  - Logs executable name, `ScanMs`, resolved RVAs, `Layer1`/`Layer2` status, initial committed MiB, and clean unload metrics with automatic log rotation.
- **Standalone Offline Test Suite (`tests/test_memfix.cpp`):**
  - Multi-threaded 16-thread superpage commit race, boundary tail-clamping, `VirtualAlloc2` + fallback arena allocation, Layer 2 latch state machine, near-relay patching, synthetic PE AOB positive + 4 negative mutation guards, live `LoadLibraryW` safe rejection, and read-only real BG3 PE section verification.
