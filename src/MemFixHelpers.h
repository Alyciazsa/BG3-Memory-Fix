#pragma once

#include <atomic>
#include <cstdint>
#include <cstring>

#include "CommonHelpers.h"

namespace bg3fix {

inline constexpr uint64_t kSuperpageSize = 0x20000ULL;                 // 128 KiB
inline constexpr uint64_t kCommitStepBytes = 4ULL * 1024ULL * 1024ULL; // 4 MiB
inline constexpr uint64_t kLayer2ReserveSuperpages = 256;              // 32 MiB reserve threshold
inline constexpr size_t kOffsetCurSuperpage = 0x28;
inline constexpr size_t kOffsetAltSuperpage = 0x30;
inline constexpr size_t kOffsetRemSuperpages = 0x38;
inline constexpr size_t kOffsetAltRemSuperpages = 0x40;
inline constexpr size_t kOffsetChunksArray = 0x48;
inline constexpr size_t kOffsetChunkCount = 0xC8;
inline constexpr size_t kOffsetChunk0Size = 0xD0;
inline constexpr size_t kOffsetPoolManager = 0xE8;
inline constexpr size_t kGlobalAllocatorSize = 0xB40;

struct CommitState {
    uint64_t base = 0;
    uint64_t end = 0;
    SRWLOCK lock = SRWLOCK_INIT;
    std::atomic<uint64_t> hwm{0};
    std::atomic<uint32_t> failures{0};
};

struct ReservedArena {
    void* rawBase = nullptr;
    uint64_t alignedBase = 0;
};

struct ResolvedMemTargets {
    uint64_t addrGlobalAllocator = 0;
    uint64_t addrGlobalAllocatorStorage = 0;
    uint64_t fnGAGet = 0;
    uint64_t fnGAAllocSuperpage = 0;
    uint64_t fnPoolManagerCtor = 0;
    uint64_t fnPoolBindBackend = 0;
    uint64_t fnPoolAlloc = 0;
    uint64_t fnLargeAlloc = 0;
};

using PfnVirtualAlloc2 = PVOID(WINAPI*)(
    HANDLE Process,
    PVOID BaseAddress,
    SIZE_T Size,
    ULONG AllocationType,
    ULONG PageProtection,
    MEM_EXTENDED_PARAMETER* ExtendedParameters,
    ULONG ParameterCount);

// Reserves `sizeBytes` of MEM_RESERVE virtual address space aligned to `alignment` (power of 2).
// Preserves `rawBase` in the fallback path so ReleaseReservedArena() can always VirtualFree cleanly.
inline ReservedArena ReserveAlignedArena(
    uint64_t sizeBytes,
    uint64_t alignment,
    bool allowVirtualAlloc2 = true) noexcept {
    ReservedArena out{};
    if (sizeBytes == 0 || alignment == 0 || (alignment & (alignment - 1ULL)) != 0) {
        return out;
    }

    if (allowVirtualAlloc2) {
        HMODULE kb = GetModuleHandleW(L"KernelBase.dll");
        if (kb) {
            auto pVirtualAlloc2 = reinterpret_cast<PfnVirtualAlloc2>(
                reinterpret_cast<void*>(GetProcAddress(kb, "VirtualAlloc2")));
            if (pVirtualAlloc2) {
                MEM_ADDRESS_REQUIREMENTS req{};
                req.Alignment = static_cast<SIZE_T>(alignment);
                MEM_EXTENDED_PARAMETER param{};
                param.Type = MemExtendedParameterAddressRequirements;
                param.Pointer = &req;
                if (void* p = pVirtualAlloc2(
                        nullptr,
                        nullptr,
                        static_cast<SIZE_T>(sizeBytes),
                        MEM_RESERVE,
                        PAGE_NOACCESS,
                        &param,
                        1)) {
                    const uint64_t addr = reinterpret_cast<uint64_t>(p);
                    if ((addr & (alignment - 1ULL)) == 0) {
                        out.rawBase = p;
                        out.alignedBase = addr;
                        return out;
                    }
                    VirtualFree(p, 0, MEM_RELEASE);
                }
            }
        }
    }

    // Fallback: over-reserve by `alignment` and align up inside the reservation.
    if (sizeBytes > UINT64_MAX - alignment) {
        return out;
    }
    void* raw = VirtualAlloc(
        nullptr,
        static_cast<SIZE_T>(sizeBytes + alignment),
        MEM_RESERVE,
        PAGE_NOACCESS);
    if (!raw) {
        return out;
    }
    out.rawBase = raw;
    out.alignedBase = AlignUpU64(reinterpret_cast<uint64_t>(raw), alignment);
    return out;
}

inline void ReleaseReservedArena(ReservedArena& arena) noexcept {
    if (arena.rawBase) {
        VirtualFree(arena.rawBase, 0, MEM_RELEASE);
        arena.rawBase = nullptr;
        arena.alignedBase = 0;
    }
}

// Ensures [state.base, min(addr + kSuperpageSize, state.end)) is committed via VirtualAlloc(MEM_COMMIT).
// Fast path uses an acquire load; slow path serializes on SRWLOCK and publishes release
// only after VirtualAlloc succeeds. Zero heap allocations or file I/O.
inline bool EnsureSuperpageCommitted(CommitState& state, uint64_t addr) noexcept {
    if (addr < state.base || addr >= state.end) {
        return true;
    }

    const uint64_t remain = state.end - addr;
    const uint64_t need = (remain >= kSuperpageSize) ? (addr + kSuperpageSize) : state.end;
    if (need <= state.hwm.load(std::memory_order_acquire)) {
        return true;
    }

    AcquireSRWLockExclusive(&state.lock);
    const uint64_t hwm = state.hwm.load(std::memory_order_relaxed);
    if (need > hwm) {
        const uint64_t maxSpan = state.end - state.base;
        const uint64_t offset = need - state.base;
        const uint64_t alignedOffset = AlignUpU64(offset, kCommitStepBytes);
        uint64_t target = (alignedOffset >= offset && alignedOffset <= maxSpan)
            ? (state.base + alignedOffset)
            : state.end;

        void* committed = VirtualAlloc(
            reinterpret_cast<void*>(hwm),
            static_cast<SIZE_T>(target - hwm),
            MEM_COMMIT,
            PAGE_READWRITE);
        if (!committed && target != need) {
            target = need;
            committed = VirtualAlloc(
                reinterpret_cast<void*>(hwm),
                static_cast<SIZE_T>(target - hwm),
                MEM_COMMIT,
                PAGE_READWRITE);
        }

        if (!committed) {
            state.failures.fetch_add(1U, std::memory_order_relaxed);
            ReleaseSRWLockExclusive(&state.lock);
            return false;
        }

        state.hwm.store(target, std::memory_order_release);
    }
    ReleaseSRWLockExclusive(&state.lock);
    return true;
}

// Layer 2 decision predicate: once all 16 chunks are active and remaining superpages drop
// below kLayer2ReserveSuperpages (32 MiB) with altRem == 0, permanently latch small pool
// allocations (size <= 0x800, align <= 0x40) to spill to Large_Alloc.
inline bool ShouldSpillPoolAlloc(
    size_t size,
    size_t align,
    const uint8_t* ga,
    std::atomic<bool>& spillLatched) noexcept {
    if (size > 0x800ULL || align > 0x40ULL || ga == nullptr) {
        return false;
    }
    if (spillLatched.load(std::memory_order_acquire)) {
        return true;
    }
    const int32_t cc = *reinterpret_cast<const volatile int32_t*>(ga + kOffsetChunkCount);
    if (cc >= 16) {
        const uint64_t rem = *reinterpret_cast<const volatile uint64_t*>(ga + kOffsetRemSuperpages);
        const uint64_t altRem = *reinterpret_cast<const volatile uint64_t*>(ga + kOffsetAltRemSuperpages);
        if (rem < kLayer2ReserveSuperpages && altRem == 0) {
            spillLatched.store(true, std::memory_order_release);
            return true;
        }
    }
    return false;
}

// Universal PE executable-section AOB locator & structural verifier for Larian ls::GlobalAllocator.
// Requires every signature to match uniquely (count == 1) and cross-verifies RIP-relative data
// offsets, PoolManager offset (+0xE8), 16-chunk cap instruction, and internal call graph edges.
inline bool ResolveMemTargets(
    uint64_t imageBase,
    uint32_t sizeOfImage,
    const PeSection* execSecs,
    size_t secCount,
    ResolvedMemTargets& out,
    const char*& outFailReason) noexcept {
    out = ResolvedMemTargets{};
    outFailReason = nullptr;

    if (imageBase == 0 || sizeOfImage == 0 || !execSecs || secCount == 0) {
        outFailReason = "no executable PE sections";
        return false;
    }

    static constexpr uint8_t kPatGAGet[] = {
        0x48, 0x83, 0xEC, 0x28, 0x48, 0x8B, 0x05, 0x00, 0x00, 0x00, 0x00, 0x48, 0x85, 0xC0, 0x0F, 0x85,
        0x00, 0x00, 0x00, 0x00, 0x48, 0x89, 0x6C, 0x24, 0x38, 0xBA, 0xA0, 0x0F, 0x00, 0x00, 0x48, 0x89,
        0x74, 0x24, 0x40, 0x4C, 0x89, 0x74, 0x24, 0x20, 0x4C, 0x8D, 0x35, 0x00, 0x00, 0x00, 0x00, 0x49,
        0x8B, 0xCE, 0xFF, 0x15, 0x00, 0x00, 0x00, 0x00, 0x0F, 0x57, 0xC0, 0x48, 0x8D, 0x0D, 0x00, 0x00,
        0x00, 0x00};
    static constexpr char kMaskGAGet[] =
        "xxxxxxx????xxxxx????xxxxxxxxxxxxxxxxxxxxxxx????xxxxx????xxxxxx????";
    static_assert(sizeof(kPatGAGet) == sizeof(kMaskGAGet) - 1, "GA_Get mask mismatch");

    static constexpr uint8_t kPatSuperpage[] = {
        0x48, 0x89, 0x5C, 0x24, 0x10, 0x56, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0xF1, 0xFF, 0x15, 0x00,
        0x00, 0x00, 0x00, 0x85, 0xC0, 0x75, 0x00, 0x48, 0x8B, 0xCE, 0xFF, 0x15, 0x00, 0x00, 0x00, 0x00,
        0x48, 0x8B, 0x5E, 0x30, 0x48, 0x85, 0xDB, 0x74, 0x00, 0x48, 0x8B, 0x56, 0x40, 0x48, 0x83, 0xFA,
        0x01, 0x72, 0x00, 0x33, 0xC9, 0x48, 0x8D, 0x83, 0x00, 0x00, 0x02, 0x00};
    static constexpr char kMaskSuperpage[] =
        "xxxxxxxxxxxxxxx????xxx?xxxxx????xxxxxxxx?xxxxxxxxx?xxxxxxxxx";
    static_assert(sizeof(kPatSuperpage) == sizeof(kMaskSuperpage) - 1, "GA_AllocSuperpage mask mismatch");

    static constexpr uint8_t kPatPoolMgrCtor[] = {
        0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x33, 0xFF, 0x48, 0x8B, 0xD9, 0x48,
        0x89, 0x39, 0xBA, 0xA0, 0x0F, 0x00, 0x00, 0x48, 0x89, 0x79, 0x08, 0x48, 0x83, 0xC1, 0x10, 0xFF,
        0x15, 0x00, 0x00, 0x00, 0x00, 0x48, 0x8D, 0x4B, 0x48, 0x48, 0x89, 0x7B, 0x38, 0xBA, 0xA0, 0x0F,
        0x00, 0x00, 0x48, 0x89, 0x7B, 0x40, 0xFF, 0x15, 0x00, 0x00, 0x00, 0x00, 0x48, 0x8D, 0x8B, 0x80,
        0x00, 0x00, 0x00};
    static constexpr char kMaskPoolMgrCtor[] =
        "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx????xxxxxxxxxxxxxxxxxxx????xxxxxxx";
    static_assert(sizeof(kPatPoolMgrCtor) == sizeof(kMaskPoolMgrCtor) - 1, "PoolMgr_Ctor mask mismatch");

    static constexpr uint8_t kPatPoolBindBackend[] = {
        0x48, 0x89, 0x5C, 0x24, 0x18, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57,
        0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0xF9, 0x48, 0x89, 0x11, 0x48, 0x8B, 0xCA, 0x48, 0x8B, 0xF2,
        0xE8, 0x00, 0x00, 0x00, 0x00, 0x33, 0xED, 0x48, 0x89, 0x44, 0x24, 0x68, 0x4C, 0x8D, 0x44, 0x24,
        0x60, 0x48, 0x89, 0x6C, 0x24, 0x60, 0x48, 0x8D, 0x54, 0x24, 0x68, 0x48, 0x8B, 0xD8, 0xE8, 0x00,
        0x00, 0x00, 0x00, 0x48, 0x89, 0x5F, 0x08, 0x48, 0x8B, 0xCE, 0x48, 0x89, 0x77, 0x38, 0xE8, 0x00,
        0x00, 0x00, 0x00, 0xBB, 0x08, 0x00, 0x00, 0x00, 0x44, 0x8B, 0xC5, 0x44, 0x8B, 0xCB, 0x49, 0xBA,
        0x20, 0x00, 0x00, 0x11, 0xAD, 0xDE, 0xCE, 0xFA};
    static constexpr char kMaskPoolBindBackend[] =
        "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx????xxxxxxxxxxxxxxxxxxxxxxxxxx????xxxxxxxxxxxx????xxxxxxxxxxxxx"
        "xxxxxxxx";
    static_assert(sizeof(kPatPoolBindBackend) == sizeof(kMaskPoolBindBackend) - 1, "Pool_BindBackend mask mismatch");

    static constexpr uint8_t kPatPoolAlloc[] = {
        0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x57,
        0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57, 0x48, 0x83, 0xEC, 0x20, 0xBD, 0xE0, 0x00, 0x00,
        0x00, 0x44, 0x8D, 0x6D, 0x40, 0x44, 0x8D, 0x65, 0x30, 0x44, 0x8D, 0x7D, 0x20, 0x44, 0x8D, 0x75,
        0x10, 0x8D, 0x75, 0xF0, 0x8D, 0x7D, 0xE0, 0x8D, 0x5D, 0xD0, 0x8D, 0x45, 0xA0, 0x44, 0x8D, 0x5D,
        0xC0, 0x44, 0x8D, 0x55, 0xB0, 0x48, 0x83, 0xFA, 0x10, 0x77, 0x00, 0x49, 0x83, 0xF8, 0x40};
    static constexpr char kMaskPoolAlloc[] =
        "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx?xxxx";
    static_assert(sizeof(kPatPoolAlloc) == sizeof(kMaskPoolAlloc) - 1, "Pool_Alloc mask mismatch");

    static constexpr uint8_t kPatLargeAlloc[] = {
        0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x48,
        0x8B, 0xFA, 0x48, 0x8B, 0xF1, 0xE8, 0x00, 0x00, 0x00, 0x00, 0x48, 0x8B, 0xC8, 0x48, 0x8B, 0xD8,
        0xFF, 0x15, 0x00, 0x00, 0x00, 0x00, 0x85, 0xC0, 0x75, 0x00, 0x48, 0x8B, 0xCB, 0xFF, 0x15, 0x00,
        0x00, 0x00, 0x00, 0x48, 0x83, 0xFF, 0x10, 0x77, 0x00, 0x48, 0x8B, 0xCE, 0xE8, 0x00, 0x00, 0x00,
        0x00, 0xEB, 0x00, 0x4C, 0x8B, 0xC6, 0x48, 0x8B, 0xD7, 0xE8, 0x00, 0x00, 0x00, 0x00, 0x48, 0x8B,
        0xCB, 0x48, 0x8B, 0xF8, 0xFF, 0x15, 0x00, 0x00, 0x00, 0x00, 0x48, 0x8B, 0x5C, 0x24, 0x30, 0x48,
        0x8B, 0xC7, 0x48, 0x8B, 0x74, 0x24, 0x38, 0x48, 0x83, 0xC4, 0x20, 0x5F, 0xC3};
    static constexpr char kMaskLargeAlloc[] =
        "xxxxxxxxxxxxxxxxxxxxxx????xxxxxxxx????xxx?xxxxx????xxxxx?xxxx????x?xxxxxxx????xxxxxxxx????xxxxxxxxxxx"
        "xxxxxxxx";
    static_assert(sizeof(kPatLargeAlloc) == sizeof(kMaskLargeAlloc) - 1, "Large_Alloc mask mismatch");

    size_t secGAGet = 0;
    size_t secSuperpage = 0;
    size_t secPoolBind = 0;

    const auto scanOrFail = [&](
        const uint8_t* pat,
        const char* mask,
        size_t patLen,
        uint64_t& outAddr,
        size_t* outSec,
        const char* notFoundMsg,
        const char* ambigMsg) noexcept -> bool {
        const ScanStatus st =
            FindPatternUniqueInExecSections(execSecs, secCount, pat, mask, patLen, outAddr, outSec);
        if (st == ScanStatus::Found) {
            return true;
        }
        outFailReason = (st == ScanStatus::Ambiguous) ? ambigMsg : notFoundMsg;
        return false;
    };

    if (!scanOrFail(kPatGAGet, kMaskGAGet, sizeof(kPatGAGet), out.fnGAGet, &secGAGet,
                    "GA_Get AOB not found", "GA_Get AOB ambiguous")) {
        return false;
    }
    if (!scanOrFail(kPatSuperpage, kMaskSuperpage, sizeof(kPatSuperpage), out.fnGAAllocSuperpage, &secSuperpage,
                    "GA_AllocSuperpage AOB not found", "GA_AllocSuperpage AOB ambiguous")) {
        return false;
    }
    if (!scanOrFail(kPatPoolMgrCtor, kMaskPoolMgrCtor, sizeof(kPatPoolMgrCtor), out.fnPoolManagerCtor, nullptr,
                    "PoolManager_Ctor AOB not found", "PoolManager_Ctor AOB ambiguous")) {
        return false;
    }
    if (!scanOrFail(kPatPoolBindBackend, kMaskPoolBindBackend, sizeof(kPatPoolBindBackend), out.fnPoolBindBackend, &secPoolBind,
                    "Pool_BindBackend AOB not found", "Pool_BindBackend AOB ambiguous")) {
        return false;
    }
    if (!scanOrFail(kPatPoolAlloc, kMaskPoolAlloc, sizeof(kPatPoolAlloc), out.fnPoolAlloc, nullptr,
                    "Pool_Alloc AOB not found", "Pool_Alloc AOB ambiguous")) {
        return false;
    }
    if (!scanOrFail(kPatLargeAlloc, kMaskLargeAlloc, sizeof(kPatLargeAlloc), out.fnLargeAlloc, nullptr,
                    "Large_Alloc AOB not found", "Large_Alloc AOB ambiguous")) {
        return false;
    }

    // Extract RIP-relative globals inside GA_Get and cross-verify against PE image bounds.
    const uint64_t imageEnd = imageBase + static_cast<uint64_t>(sizeOfImage);
    out.addrGlobalAllocator = ResolveRipRel32(out.fnGAGet + 0x04ULL, 3, 7);
    out.addrGlobalAllocatorStorage = ResolveRipRel32(out.fnGAGet + 0x28ULL, 3, 7);
    const uint64_t poolMgrAddr = ResolveRipRel32(out.fnGAGet + 0x3BULL, 3, 7);

    if (out.addrGlobalAllocator < imageBase || out.addrGlobalAllocator + sizeof(PVOID) > imageEnd) {
        outFailReason = "gGlobalAllocator pointer out of PE image bounds";
        return false;
    }
    if (out.addrGlobalAllocatorStorage < imageBase ||
        out.addrGlobalAllocatorStorage + kGlobalAllocatorSize > imageEnd) {
        outFailReason = "gGlobalAllocatorStorage buffer out of PE image bounds";
        return false;
    }
    if (poolMgrAddr < out.addrGlobalAllocatorStorage ||
        (poolMgrAddr - out.addrGlobalAllocatorStorage) != kOffsetPoolManager) {
        outFailReason = "PoolManager offset != +0xE8 inside GlobalAllocator";
        return false;
    }

    // Verify 16-chunk cap instruction (`cmp dword ptr [rsi+0C8h], 16`) inside GA_AllocSuperpage.
    static constexpr uint8_t kChunkCapCmp[] = {0x83, 0xBE, 0xC8, 0x00, 0x00, 0x00, 0x10};
    const uint64_t spSecEnd = execSecs[secSuperpage].start + execSecs[secSuperpage].size;
    if (!ContainsBytes(out.fnGAAllocSuperpage + 0x45ULL, 0x3BULL, spSecEnd, kChunkCapCmp, sizeof(kChunkCapCmp))) {
        outFailReason = "GA_AllocSuperpage 16-chunk cap check not found";
        return false;
    }

    // Cross-verify internal call-graph edges between the resolved functions.
    const uint64_t gaGetSecEnd = execSecs[secGAGet].start + execSecs[secGAGet].size;
    if (!HasRel32CallToTarget(out.fnGAGet + 0x50ULL, 0x60ULL, gaGetSecEnd, out.fnPoolManagerCtor)) {
        outFailReason = "GA_Get -> PoolManager_Ctor call edge missing";
        return false;
    }
    if (!HasRel32CallToTarget(out.fnGAGet + 0xD0ULL, 0x80ULL, gaGetSecEnd, out.fnPoolBindBackend)) {
        outFailReason = "GA_Get -> Pool_BindBackend call edge missing";
        return false;
    }

    const uint64_t poolBindSecEnd = execSecs[secPoolBind].start + execSecs[secPoolBind].size;
    if (!HasRel32CallToTarget(out.fnPoolBindBackend + 0x15ULL, 0x50ULL, poolBindSecEnd, out.fnGAAllocSuperpage)) {
        outFailReason = "Pool_BindBackend -> GA_AllocSuperpage call edge missing";
        return false;
    }

    // Confirm movable 5-byte prologues at hook sites (mov [rsp+10h], rbx / mov [rsp+8], rbx).
    static constexpr uint8_t kPrologueSuperpage[] = {0x48, 0x89, 0x5C, 0x24, 0x10};
    static constexpr uint8_t kProloguePoolAlloc[] = {0x48, 0x89, 0x5C, 0x24, 0x08};
    if (std::memcmp(reinterpret_cast<const void*>(out.fnGAAllocSuperpage), kPrologueSuperpage, 5) != 0) {
        outFailReason = "GA_AllocSuperpage prologue mismatch";
        return false;
    }
    if (std::memcmp(reinterpret_cast<const void*>(out.fnPoolAlloc), kProloguePoolAlloc, 5) != 0) {
        outFailReason = "Pool_Alloc prologue mismatch";
        return false;
    }

    return true;
}

} // namespace bg3fix
