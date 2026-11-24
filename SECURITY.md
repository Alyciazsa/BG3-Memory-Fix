# Security Policy

## 🔒 Transparency & Zero-Network Guarantee

**BG3 Memory Fix (`00_BG3MemFix.dll`)** is an auditable, single-purpose C++20 native plugin that runs strictly in-process inside `bg3.exe` / `bg3_dx11.exe`:

- **Strictly `KERNEL32.DLL`-Only Imports:** Compiled without CRT dynamic dependencies or network libraries (`WinHTTP`, `WinSock`, `USER32`, `ADVAPI32` are all absent). You can verify imports directly with Visual Studio's `dumpbin /DEPENDENTS 00_BG3MemFix.dll`.
- **Zero Telemetry or Network Calls:** Performs zero outbound/inbound network connections and reads/writes only its own local `BG3MemFix.log` / `BG3MemFix-prev.log` alongside the DLL in `bin\NativeMods\`.
- **Fail-Closed Binary Verification (Anti-Mispatch Guard):** Before writing a single hook byte, `ResolveMemTargets()` scans executable PE sections only (`IMAGE_SCN_MEM_EXECUTE`) for **6 distinct AOB signatures**, rejects any ambiguous (`>= 2` matches) or missing signature, and cross-verifies:
  1. Valid `IMAGE_DOS_HEADER` and 64-bit `IMAGE_NT_HEADERS64`.
  2. `gGlobalAllocator` pointer and `gGlobalAllocatorStorage` buffer bounds strictly inside `[ImageBase, ImageBase + SizeOfImage)`.
  3. Exact struct layout offset (`PoolManager == GlobalAllocator + 0xE8`).
  4. Exact `cmp dword ptr [rsi+0xC8], 16` chunk-cap instruction inside `GA_AllocSuperpage`.
  5. Exact `E8` RIP-relative call graph edges (`GA_Get -> PoolManager_Ctor`, `GA_Get -> Pool_BindBackend`, `Pool_BindBackend -> GA_AllocSuperpage`).
  6. Exact 5-byte hookable prologues at `GA_AllocSuperpage` and `Pool_Alloc`.
  If **any** check fails on an unsupported or future game binary, the DLL writes diagnostics to `BG3MemFix.log` and exits cleanly without touching a single byte of game memory.
- **Scoped Scope:** Never modifies save files, `.pak` files, registry keys, or Windows system settings.
- **No Included Secrets:** Repository source, build scripts, and tests are open-source under the MIT License.

---

## 📦 Supported Versions

| Version | Status | Notes |
| :--- | :--- | :--- |
| `1.0.x` (Universal AOB) | ✅ Supported | Steam & GOG, Vulkan (`bg3.exe`) & DX11 (`bg3_dx11.exe`) |

---

## 🛡️ Reporting a Vulnerability

If you discover a memory safety issue, false-positive AOB collision on an official binary, or crash regression:

- Open a **[Private Vulnerability Report](https://github.com/Alyciazsa/BG3-Memory-Fix/security/advisories/new)** via GitHub Security Advisories, or
- Open a GitHub Issue attaching your `bin\NativeMods\BG3MemFix.log` and exact BG3 executable version / store edition (Steam or GOG).
