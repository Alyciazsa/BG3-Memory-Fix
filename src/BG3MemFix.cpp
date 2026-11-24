#define WIN32_LEAN_AND_MEAN
#define _WIN32_WINNT 0x0A00
#include <windows.h>

#include <atomic>
#include <cstdint>
#include <cstring>

#include "CommonHelpers.h"
#include "MemFixHelpers.h"

namespace {

constexpr uint64_t kChunk0Size = 16ULL * 1024ULL * 1024ULL * 1024ULL; // 16 GiB virtual arena
constexpr size_t kRelaySize = 0x1000;                                 // 1 page: trampolines + detours (RX)
constexpr size_t kMaxExecSections = 16;

using PfnGAAllocSuperpage = void*(__fastcall*)(void* ga);
using PfnPoolManagerCtor = void(__fastcall*)(void* poolMgr);
using PfnPoolBindBackend = void(__fastcall*)(void* poolMgr, void* ga);
using PfnPoolAlloc = void*(__fastcall*)(void* poolMgr, size_t size, size_t align, size_t* outWaste);
using PfnLargeAlloc = void*(__fastcall*)(size_t size, size_t align);

// Shared state touched inside allocator hooks (zero heap alloc / zero I/O).
bg3fix::CommitState gCommitState{};
uint8_t* gGA = nullptr;
uint8_t* gRelay = nullptr;
PfnGAAllocSuperpage gOrigGAAllocSuperpage = nullptr;
PfnPoolAlloc gOrigPoolAlloc = nullptr;
PfnLargeAlloc gLargeAlloc = nullptr;
std::atomic<bool> gSpillToLarge{false};
std::atomic<uint64_t> gSpillCount{0};

bool gLayer1Active = false;
bool gLayer2Active = false;
wchar_t gLogPath[MAX_PATH]{};

void* __fastcall Hook_GA_AllocSuperpage(void* ga) {
    void* p = gOrigGAAllocSuperpage(ga);
    if (p) {
        bg3fix::EnsureSuperpageCommitted(gCommitState, reinterpret_cast<uint64_t>(p));
    }
    return p;
}

void* __fastcall Hook_Pool_Alloc(void* poolMgr, size_t size, size_t align, size_t* outWaste) {
    if (bg3fix::ShouldSpillPoolAlloc(size, align, gGA, gSpillToLarge)) {
        if (outWaste) {
            *outWaste = 0;
        }
        gSpillCount.fetch_add(1ULL, std::memory_order_relaxed);
        return gLargeAlloc(size, align);
    }

    return gOrigPoolAlloc(poolMgr, size, align, outWaste);
}

void InstallOnAttach(HMODULE hModule) noexcept {
    bg3fix::BuildLogPath(hModule, L"BG3MemFix.log", gLogPath);
    HANDLE hLog = bg3fix::OpenLogFile(gLogPath, true);

    const HMODULE hExe = GetModuleHandleW(nullptr);
    const uint64_t base = reinterpret_cast<uint64_t>(hExe);
    wchar_t exePathBuf[MAX_PATH]{};
    const wchar_t* exeName = bg3fix::GetModuleBaseName(hExe, exePathBuf);

    bg3fix::PeImageIdentity id{};
    if (!bg3fix::ReadPeImageIdentity(base, id)) {
        bg3fix::LogBuffer buf;
        buf.PutTimestamp();
        buf.PutStr("ERROR: Unsupported BG3 binary (invalid PE header). Exe=");
        buf.PutWideAsAscii(exeName);
        buf.PutStr(" ImageBase=");
        buf.PutHex(base);
        buf.PutStr("\r\n");
        buf.FlushToFile(hLog);
        if (hLog != INVALID_HANDLE_VALUE) {
            CloseHandle(hLog);
        }
        return;
    }

    bg3fix::PeSection execSecs[kMaxExecSections]{};
    const size_t secCount = bg3fix::FindExecutableSections(base, execSecs, kMaxExecSections);

    const ULONGLONG t0 = GetTickCount64();
    bg3fix::ResolvedMemTargets targets{};
    const char* failReason = nullptr;
    const bool resolved = bg3fix::ResolveMemTargets(base, id.sizeOfImage, execSecs, secCount, targets, failReason);
    const ULONGLONG scanMs = GetTickCount64() - t0;

    if (!resolved) {
        bg3fix::LogBuffer buf;
        buf.PutTimestamp();
        buf.PutStr("ERROR: Unsupported BG3 binary. Exe=");
        buf.PutWideAsAscii(exeName);
        buf.PutStr(" ImageBase=");
        buf.PutHex(base);
        buf.PutStr(" TimeDateStamp=");
        buf.PutHex(id.timeDateStamp, 8);
        buf.PutStr(" SizeOfImage=");
        buf.PutHex(id.sizeOfImage, 8);
        buf.PutStr(" Reason=");
        buf.PutStr(failReason ? failReason : "unknown");
        buf.PutStr("\r\n");
        buf.FlushToFile(hLog);
        if (hLog != INVALID_HANDLE_VALUE) {
            CloseHandle(hLog);
        }
        return;
    }

    const uint64_t fnSuperpage = targets.fnGAAllocSuperpage;
    const uint64_t fnPoolAlloc = targets.fnPoolAlloc;
    const uint64_t minHook = (fnSuperpage < fnPoolAlloc) ? fnSuperpage : fnPoolAlloc;
    const uint64_t maxHook = (fnSuperpage > fnPoolAlloc) ? fnSuperpage : fnPoolAlloc;

    gRelay = bg3fix::AllocateNearRelay(base, id.sizeOfImage, minHook, maxHook + 16ULL, kRelaySize);
    if (!gRelay) {
        bg3fix::LogLine(hLog, "ERROR: Failed to allocate near relay within +-2 GiB.");
        if (hLog != INVALID_HANDLE_VALUE) {
            CloseHandle(hLog);
        }
        return;
    }
    std::memset(gRelay, 0, kRelaySize);

    // Relay Page 0: trampolines and detours for GA_AllocSuperpage and Pool_Alloc.
    uint8_t* const trampSuperpage = gRelay + 0x000;
    uint8_t* const detourSuperpage = gRelay + 0x020;
    uint8_t* const trampPoolAlloc = gRelay + 0x040;
    uint8_t* const detourPoolAlloc = gRelay + 0x060;

    // Trampoline + detour for GA_AllocSuperpage (first 5 bytes: mov [rsp+10h], rbx).
    std::memcpy(trampSuperpage, reinterpret_cast<const void*>(fnSuperpage), 5);
    bg3fix::WriteAbsJump(trampSuperpage + 5, fnSuperpage + 5ULL);
    bg3fix::WriteAbsJump(detourSuperpage, reinterpret_cast<uint64_t>(&Hook_GA_AllocSuperpage));
    gOrigGAAllocSuperpage = reinterpret_cast<PfnGAAllocSuperpage>(trampSuperpage);

    // Trampoline + detour for Pool_Alloc (first 5 bytes: mov [rsp+8], rbx).
    std::memcpy(trampPoolAlloc, reinterpret_cast<const void*>(fnPoolAlloc), 5);
    bg3fix::WriteAbsJump(trampPoolAlloc + 5, fnPoolAlloc + 5ULL);
    bg3fix::WriteAbsJump(detourPoolAlloc, reinterpret_cast<uint64_t>(&Hook_Pool_Alloc));
    gOrigPoolAlloc = reinterpret_cast<PfnPoolAlloc>(trampPoolAlloc);
    gLargeAlloc = reinterpret_cast<PfnLargeAlloc>(targets.fnLargeAlloc);

    // Seal relay Page 0 as PAGE_EXECUTE_READ before patching target code.
    DWORD oldRelayProt = 0;
    if (!VirtualProtect(gRelay, kRelaySize, PAGE_EXECUTE_READ, &oldRelayProt)) {
        VirtualFree(gRelay, 0, MEM_RELEASE);
        gRelay = nullptr;
        bg3fix::LogLine(hLog, "ERROR: Failed to protect relay page as PAGE_EXECUTE_READ.");
        if (hLog != INVALID_HANDLE_VALUE) {
            CloseHandle(hLog);
        }
        return;
    }
    FlushInstructionCache(GetCurrentProcess(), gRelay, kRelaySize);

    auto* const globalAllocPtr = reinterpret_cast<PVOID volatile*>(targets.addrGlobalAllocator);
    const PVOID preAttachGA = *globalAllocPtr;
    gGA = preAttachGA
        ? static_cast<uint8_t*>(preAttachGA)
        : reinterpret_cast<uint8_t*>(targets.addrGlobalAllocatorStorage);

    // Install Layer 1 (pre-initialize 16 GiB on-demand chunk[0] + hook GA_AllocSuperpage)
    // ONLY when ls::GlobalAllocator has not already been initialized before attach.
    bool hookSuperpageInstalled = false;
    if (preAttachGA == nullptr) {
        bg3fix::ReservedArena arena = bg3fix::ReserveAlignedArena(kChunk0Size, bg3fix::kSuperpageSize);
        if (arena.alignedBase != 0 && (arena.alignedBase & (bg3fix::kSuperpageSize - 1ULL)) == 0) {
            hookSuperpageInstalled =
                bg3fix::WriteNearJmpPatch(fnSuperpage, reinterpret_cast<uint64_t>(detourSuperpage), 5);
            if (hookSuperpageInstalled) {
                const uint64_t chunk0 = arena.alignedBase;
                gCommitState.base = chunk0;
                gCommitState.end = chunk0 + kChunk0Size;
                InitializeSRWLock(&gCommitState.lock);
                gCommitState.hwm.store(chunk0, std::memory_order_relaxed);
                gCommitState.failures.store(0, std::memory_order_relaxed);

                std::memset(gGA, 0, bg3fix::kGlobalAllocatorSize);
                InitializeCriticalSectionAndSpinCount(reinterpret_cast<CRITICAL_SECTION*>(gGA), 0xFA0);
                *reinterpret_cast<uint64_t*>(gGA + bg3fix::kOffsetChunk0Size) = kChunk0Size;

                auto poolMgrCtor = reinterpret_cast<PfnPoolManagerCtor>(targets.fnPoolManagerCtor);
                auto poolBindBackend = reinterpret_cast<PfnPoolBindBackend>(targets.fnPoolBindBackend);
                poolMgrCtor(gGA + bg3fix::kOffsetPoolManager);

                *reinterpret_cast<uint64_t*>(gGA + bg3fix::kOffsetCurSuperpage) = chunk0;
                *reinterpret_cast<uint64_t*>(gGA + bg3fix::kOffsetAltSuperpage) = 0;
                *reinterpret_cast<uint64_t*>(gGA + bg3fix::kOffsetRemSuperpages) = kChunk0Size >> 17;
                *reinterpret_cast<uint64_t*>(gGA + bg3fix::kOffsetAltRemSuperpages) = 0;
                *reinterpret_cast<uint64_t*>(gGA + bg3fix::kOffsetChunksArray) = chunk0;
                *reinterpret_cast<int32_t*>(gGA + bg3fix::kOffsetChunkCount) = 1;

                poolBindBackend(gGA + bg3fix::kOffsetPoolManager, gGA);

                if (gCommitState.failures.load(std::memory_order_relaxed) == 0 &&
                    gCommitState.hwm.load(std::memory_order_relaxed) > chunk0) {
                    InterlockedExchangePointer(globalAllocPtr, gGA);
                    gLayer1Active = true;
                } else {
                    gCommitState.base = 0;
                    gCommitState.end = 0;
                    gCommitState.hwm.store(0, std::memory_order_relaxed);
                    DeleteCriticalSection(reinterpret_cast<CRITICAL_SECTION*>(gGA));
                    std::memset(gGA, 0, bg3fix::kGlobalAllocatorSize);
                    bg3fix::ReleaseReservedArena(arena);
                }
            } else {
                bg3fix::ReleaseReservedArena(arena);
            }
        } else {
            bg3fix::ReleaseReservedArena(arena);
        }
    }

    // Install Layer 2 (Pool_Alloc overflow safety net).
    if (bg3fix::WriteNearJmpPatch(fnPoolAlloc, reinterpret_cast<uint64_t>(detourPoolAlloc), 5)) {
        gLayer2Active = true;
    }

    // If zero hooks were patched into target code, release the unused relay page.
    if (!hookSuperpageInstalled && !gLayer2Active) {
        VirtualFree(gRelay, 0, MEM_RELEASE);
        gRelay = nullptr;
    }

    const uint64_t reservedMiB = gLayer1Active ? (kChunk0Size / (1024ULL * 1024ULL)) : 0ULL;
    const uint64_t initCommittedMiB = gLayer1Active
        ? ((gCommitState.hwm.load(std::memory_order_relaxed) - gCommitState.base) / (1024ULL * 1024ULL))
        : 0ULL;

    bg3fix::LogBuffer buf;
    buf.PutTimestamp();
    buf.PutStr("ATTACH ");
    buf.PutWideAsAscii(exeName);
    buf.PutStr(" Mode=UniversalAOB Base=");
    buf.PutHex(base);
    buf.PutStr(" TS=");
    buf.PutHex(id.timeDateStamp, 8);
    buf.PutStr(" Size=");
    buf.PutHex(id.sizeOfImage, 8);
    buf.PutStr(" ScanMs=");
    buf.PutDec(scanMs);
    buf.PutStr(" Relay=");
    buf.PutHex(reinterpret_cast<uint64_t>(gRelay));
    buf.PutStr("\r\n");

    buf.PutTimestamp();
    buf.PutStr("  ResolvedRVAs GA_Get=");
    buf.PutHex(targets.fnGAGet - base);
    buf.PutStr(" Superpage=");
    buf.PutHex(targets.fnGAAllocSuperpage - base);
    buf.PutStr(" PoolCtor=");
    buf.PutHex(targets.fnPoolManagerCtor - base);
    buf.PutStr(" PoolBind=");
    buf.PutHex(targets.fnPoolBindBackend - base);
    buf.PutStr(" PoolAlloc=");
    buf.PutHex(targets.fnPoolAlloc - base);
    buf.PutStr(" LargeAlloc=");
    buf.PutHex(targets.fnLargeAlloc - base);
    buf.PutStr(" GlobalPtr=");
    buf.PutHex(targets.addrGlobalAllocator - base);
    buf.PutStr(" GlobalBuf=");
    buf.PutHex(targets.addrGlobalAllocatorStorage - base);
    buf.PutStr("\r\n");

    buf.PutTimestamp();
    buf.PutStr("  Layer1=");
    buf.PutStr(gLayer1Active ? "ACTIVE" : "SKIPPED");
    buf.PutStr(" PreAttachGA=");
    buf.PutHex(reinterpret_cast<uint64_t>(preAttachGA));
    buf.PutStr(" PostAttachGA=");
    buf.PutHex(reinterpret_cast<uint64_t>(*globalAllocPtr));
    buf.PutStr(" Chunk0=");
    buf.PutHex(gCommitState.base);
    buf.PutStr(" ReservedMiB=");
    buf.PutDec(reservedMiB);
    buf.PutStr(" InitCommittedMiB=");
    buf.PutDec(initCommittedMiB);
    buf.PutStr("\r\n");

    buf.PutTimestamp();
    buf.PutStr("  Layer2=");
    buf.PutStr(gLayer2Active ? "ACTIVE" : "FAILED");
    buf.PutStr("\r\n");

    buf.FlushToFile(hLog);
    if (hLog != INVALID_HANDLE_VALUE) {
        CloseHandle(hLog);
    }
}

void LogOnDetach() noexcept {
    if (!gLayer1Active && !gLayer2Active) {
        return;
    }
    HANDLE hLog = bg3fix::OpenLogFile(gLogPath, false);
    if (hLog == INVALID_HANDLE_VALUE) {
        return;
    }

    const uint64_t peakCommittedMiB = gLayer1Active
        ? ((gCommitState.hwm.load(std::memory_order_relaxed) - gCommitState.base) / (1024ULL * 1024ULL))
        : 0ULL;
    const int32_t finalChunks = gGA ? *reinterpret_cast<const volatile int32_t*>(gGA + bg3fix::kOffsetChunkCount) : 0;
    const uint32_t commitFailures = gCommitState.failures.load(std::memory_order_relaxed);
    const uint64_t spillCount = gSpillCount.load(std::memory_order_relaxed);

    bg3fix::LogBuffer buf;
    buf.PutTimestamp();
    buf.PutStr("DETACH PeakChunk0CommittedMiB=");
    buf.PutDec(peakCommittedMiB);
    buf.PutStr(" FinalChunks=");
    buf.PutDec(finalChunks >= 0 ? static_cast<uint64_t>(finalChunks) : 0ULL);
    buf.PutStr(" CommitFailures=");
    buf.PutDec(commitFailures);
    buf.PutStr(" Layer2Spills=");
    buf.PutDec(spillCount);
    buf.PutStr("\r\n");
    buf.FlushToFile(hLog);
    CloseHandle(hLog);
}

} // namespace

BOOL APIENTRY DllMain(HMODULE hModule, DWORD ulReasonForCall, LPVOID) {
    if (ulReasonForCall == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hModule);
        InstallOnAttach(hModule);
    } else if (ulReasonForCall == DLL_PROCESS_DETACH) {
        LogOnDetach();
    }
    return TRUE;
}
