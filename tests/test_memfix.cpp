#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "../src/CommonHelpers.h"
#include "../src/MemFixHelpers.h"

namespace {

void WriteRel32At(uint8_t* codeBase, uint64_t patchAddr, size_t dispOffset, size_t instrLen, uint64_t target) {
    const int64_t diff = static_cast<int64_t>(target) - static_cast<int64_t>(patchAddr + instrLen);
    const int32_t disp = static_cast<int32_t>(diff);
    std::memcpy(codeBase + (patchAddr - reinterpret_cast<uint64_t>(codeBase)) + dispOffset, &disp, sizeof(disp));
}

bool TestMultiThreadCommitRace() {
    constexpr uint64_t kArenaSize = 64ULL * 1024ULL * 1024ULL; // 512 x 128 KiB superpages
    constexpr uint64_t kNumSuperpages = kArenaSize / bg3fix::kSuperpageSize;

    void* reserved = VirtualAlloc(nullptr, kArenaSize, MEM_RESERVE, PAGE_NOACCESS);
    if (!reserved) {
        std::printf("FAIL: VirtualAlloc MEM_RESERVE failed (%lu)\n", GetLastError());
        return false;
    }

    bg3fix::CommitState state{};
    state.base = reinterpret_cast<uint64_t>(reserved);
    state.end = state.base + kArenaSize;
    InitializeSRWLock(&state.lock);
    state.hwm.store(state.base, std::memory_order_relaxed);
    state.failures.store(0, std::memory_order_relaxed);

    constexpr int kThreads = 16;
    std::atomic<bool> start{false};
    std::atomic<bool> ok{true};
    std::vector<std::thread> workers;
    workers.reserve(kThreads);

    for (int t = 0; t < kThreads; ++t) {
        workers.emplace_back([&, t]() {
            while (!start.load(std::memory_order_acquire)) {
                YieldProcessor();
            }
            for (uint64_t step = 0; step < kNumSuperpages; ++step) {
                const uint64_t idx = (t % 2 == 0)
                    ? ((step * 17ULL + static_cast<uint64_t>(t) * 31ULL) % kNumSuperpages)
                    : (kNumSuperpages - 1ULL - ((step * 13ULL + static_cast<uint64_t>(t) * 7ULL) % kNumSuperpages));
                const uint64_t pageAddr = state.base + idx * bg3fix::kSuperpageSize;
                if (!bg3fix::EnsureSuperpageCommitted(state, pageAddr)) {
                    ok.store(false, std::memory_order_relaxed);
                    return;
                }
                auto* bytes = reinterpret_cast<volatile uint8_t*>(pageAddr);
                bytes[0] = static_cast<uint8_t>(idx & 0xFFU);
                bytes[bg3fix::kSuperpageSize / 2] = 0x5AU;
                bytes[bg3fix::kSuperpageSize - 1] = 0xA5U;
            }
        });
    }

    start.store(true, std::memory_order_release);
    for (auto& w : workers) {
        w.join();
    }

    const bool passed = ok.load() &&
                        state.failures.load() == 0 &&
                        state.hwm.load() == state.end;
    VirtualFree(reserved, 0, MEM_RELEASE);
    if (!passed) {
        std::printf("FAIL: CommitRace ok=%d failures=%u hwm=%llu end=%llu\n",
                    static_cast<int>(ok.load()),
                    state.failures.load(),
                    state.hwm.load(),
                    state.end);
        return false;
    }
    std::printf("PASS: Multi-thread 4 MiB on-demand commit race (16 threads, 64 MiB / 512 superpages)\n");
    return true;
}

bool TestCommitBoundaryEdgeCases() {
    constexpr uint64_t kArenaSize = 6ULL * 1024ULL * 1024ULL;
    void* reserved = VirtualAlloc(nullptr, kArenaSize, MEM_RESERVE, PAGE_NOACCESS);
    if (!reserved) {
        std::printf("FAIL: VirtualAlloc 6 MiB MEM_RESERVE failed\n");
        return false;
    }

    bg3fix::CommitState state{};
    state.base = reinterpret_cast<uint64_t>(reserved);
    state.end = state.base + kArenaSize;
    InitializeSRWLock(&state.lock);
    state.hwm.store(state.base, std::memory_order_relaxed);
    state.failures.store(0, std::memory_order_relaxed);

    if (!bg3fix::EnsureSuperpageCommitted(state, state.base - bg3fix::kSuperpageSize) ||
        state.hwm.load() != state.base ||
        !bg3fix::EnsureSuperpageCommitted(state, state.end) ||
        state.hwm.load() != state.base) {
        VirtualFree(reserved, 0, MEM_RELEASE);
        std::printf("FAIL: Out-of-range address modified hwm\n");
        return false;
    }

    if (!bg3fix::EnsureSuperpageCommitted(state, state.base) ||
        state.hwm.load() != state.base + bg3fix::kCommitStepBytes) {
        VirtualFree(reserved, 0, MEM_RELEASE);
        std::printf("FAIL: Initial 4 MiB commit step mismatch\n");
        return false;
    }

    if (!bg3fix::EnsureSuperpageCommitted(state, state.base + bg3fix::kSuperpageSize) ||
        state.hwm.load() != state.base + bg3fix::kCommitStepBytes) {
        VirtualFree(reserved, 0, MEM_RELEASE);
        std::printf("FAIL: Already-committed fast path modified hwm\n");
        return false;
    }

    const uint64_t unalignedTail = state.end - 0x10000ULL;
    if (!bg3fix::EnsureSuperpageCommitted(state, unalignedTail) ||
        !bg3fix::EnsureSuperpageCommitted(state, unalignedTail) ||
        state.hwm.load() != state.end ||
        state.failures.load() != 0) {
        VirtualFree(reserved, 0, MEM_RELEASE);
        std::printf("FAIL: Unaligned tail clamp commit failed\n");
        return false;
    }

    auto* tailByte = reinterpret_cast<volatile uint8_t*>(state.end - 1);
    *tailByte = 0x77;
    const uint8_t readback = *tailByte;
    VirtualFree(reserved, 0, MEM_RELEASE);
    if (readback != 0x77) {
        std::printf("FAIL: Tail byte readback mismatch\n");
        return false;
    }

    std::printf("PASS: CommitState boundary edge cases (out-of-range, fast path, 6 MiB unaligned tail clamp)\n");
    return true;
}

bool TestReservedArenaAndLayer2Spill() {
    constexpr uint64_t kTestArenaSize = 8ULL * 1024ULL * 1024ULL;
    for (int mode = 0; mode < 2; ++mode) {
        const bool allowVA2 = (mode == 0);
        bg3fix::ReservedArena arena =
            bg3fix::ReserveAlignedArena(kTestArenaSize, bg3fix::kSuperpageSize, allowVA2);
        if (!arena.rawBase || arena.alignedBase == 0 ||
            (arena.alignedBase & (bg3fix::kSuperpageSize - 1ULL)) != 0 ||
            arena.alignedBase < reinterpret_cast<uint64_t>(arena.rawBase)) {
            bg3fix::ReleaseReservedArena(arena);
            std::printf("FAIL: ReserveAlignedArena(allowVA2=%d) invalid\n", static_cast<int>(allowVA2));
            return false;
        }
        void* c = VirtualAlloc(
            reinterpret_cast<void*>(arena.alignedBase),
            bg3fix::kSuperpageSize,
            MEM_COMMIT,
            PAGE_READWRITE);
        if (!c) {
            bg3fix::ReleaseReservedArena(arena);
            std::printf("FAIL: VirtualAlloc MEM_COMMIT inside ReserveAlignedArena failed\n");
            return false;
        }
        *reinterpret_cast<volatile uint64_t*>(c) = 0xCAFEBABEULL;
        bg3fix::ReleaseReservedArena(arena);
        if (arena.rawBase != nullptr || arena.alignedBase != 0) {
            std::printf("FAIL: ReleaseReservedArena did not zero struct\n");
            return false;
        }
    }

    bg3fix::ReservedArena bad1 = bg3fix::ReserveAlignedArena(0, bg3fix::kSuperpageSize);
    bg3fix::ReservedArena bad2 = bg3fix::ReserveAlignedArena(kTestArenaSize, 3);
    if (bad1.rawBase != nullptr || bad2.rawBase != nullptr) {
        bg3fix::ReleaseReservedArena(bad1);
        bg3fix::ReleaseReservedArena(bad2);
        std::printf("FAIL: ReserveAlignedArena accepted invalid parameters\n");
        return false;
    }

    alignas(16) uint8_t fakeGA[0x200]{};
    std::atomic<bool> spillLatched{false};
    auto setGA = [&](int32_t cc, uint64_t rem, uint64_t altRem) {
        *reinterpret_cast<int32_t*>(fakeGA + bg3fix::kOffsetChunkCount) = cc;
        *reinterpret_cast<uint64_t*>(fakeGA + bg3fix::kOffsetRemSuperpages) = rem;
        *reinterpret_cast<uint64_t*>(fakeGA + bg3fix::kOffsetAltRemSuperpages) = altRem;
    };

    if (bg3fix::ShouldSpillPoolAlloc(0x100, 0x10, nullptr, spillLatched)) {
        std::printf("FAIL: ShouldSpillPoolAlloc triggered with nullptr ga\n");
        return false;
    }
    setGA(15, 0, 0);
    if (bg3fix::ShouldSpillPoolAlloc(0x800, 0x40, fakeGA, spillLatched) || spillLatched.load()) {
        std::printf("FAIL: ShouldSpillPoolAlloc triggered with cc=15\n");
        return false;
    }
    setGA(16, 0, 1);
    if (bg3fix::ShouldSpillPoolAlloc(0x800, 0x40, fakeGA, spillLatched) || spillLatched.load()) {
        std::printf("FAIL: ShouldSpillPoolAlloc triggered with altRem=1\n");
        return false;
    }
    setGA(16, bg3fix::kLayer2ReserveSuperpages, 0);
    if (bg3fix::ShouldSpillPoolAlloc(0x800, 0x40, fakeGA, spillLatched) || spillLatched.load()) {
        std::printf("FAIL: ShouldSpillPoolAlloc triggered at rem==threshold\n");
        return false;
    }
    setGA(16, bg3fix::kLayer2ReserveSuperpages - 1, 0);
    if (bg3fix::ShouldSpillPoolAlloc(0x801, 0x40, fakeGA, spillLatched) ||
        bg3fix::ShouldSpillPoolAlloc(0x800, 0x41, fakeGA, spillLatched) ||
        spillLatched.load()) {
        std::printf("FAIL: ShouldSpillPoolAlloc triggered for oversized size/align\n");
        return false;
    }
    if (!bg3fix::ShouldSpillPoolAlloc(0x800, 0x40, fakeGA, spillLatched) || !spillLatched.load()) {
        std::printf("FAIL: ShouldSpillPoolAlloc did not latch at boundary\n");
        return false;
    }
    setGA(16, 10000, 100);
    if (!bg3fix::ShouldSpillPoolAlloc(0x40, 0x10, fakeGA, spillLatched) ||
        bg3fix::ShouldSpillPoolAlloc(0x801, 0x10, fakeGA, spillLatched)) {
        std::printf("FAIL: ShouldSpillPoolAlloc post-latch behavior mismatch\n");
        return false;
    }

    std::printf("PASS: ReserveAlignedArena (VA2 + fallback) and Layer 2 ShouldSpillPoolAlloc state machine\n");
    return true;
}

bool TestNearRelayAndPatching() {
    constexpr uint64_t kAnchor = 0x00007FF622B10000ULL;
    if (!bg3fix::FitsRel32(kAnchor, kAnchor + 0x7F000000ULL) ||
        !bg3fix::FitsRel32(kAnchor, kAnchor - 0x7F000000ULL) ||
        bg3fix::FitsRel32(kAnchor, kAnchor + 0x7F000001ULL) ||
        bg3fix::FitsRel32(kAnchor, kAnchor - 0x7F000001ULL)) {
        std::printf("FAIL: FitsRel32 boundary check failed\n");
        return false;
    }

    const uint64_t base = reinterpret_cast<uint64_t>(GetModuleHandleW(nullptr));
    bg3fix::PeImageIdentity id{};
    if (!bg3fix::ReadPeImageIdentity(base, id) || id.sizeOfImage == 0) {
        std::printf("FAIL: ReadPeImageIdentity failed on test binary\n");
        return false;
    }

    if (bg3fix::AllocateNearRelay(0, 0x10000, 0x1000, 0x2000, 0x1000) != nullptr ||
        bg3fix::AllocateNearRelay(base, id.sizeOfImage, base, base + 0x100, 0) != nullptr ||
        bg3fix::AllocateNearRelay(base, id.sizeOfImage, base, base + 0x100, 0x20000) != nullptr) {
        std::printf("FAIL: AllocateNearRelay accepted invalid inputs\n");
        return false;
    }

    uint8_t* lowTest = bg3fix::AllocateNearRelay(0x50000ULL, 0x10000U, 0x50000ULL, 0x51000ULL, 0x1000);
    if (lowTest != nullptr) {
        const uint64_t lowAddr = reinterpret_cast<uint64_t>(lowTest);
        VirtualFree(lowTest, 0, MEM_RELEASE);
        if (lowAddr < 0x100000ULL) {
            std::printf("FAIL: AllocateNearRelay allocated below 0x100000\n");
            return false;
        }
    }

    uint8_t* relay1 = bg3fix::AllocateNearRelay(base, id.sizeOfImage, base + 0x1000, base + 0x2000, 0x1000);
    uint8_t* relay2 = bg3fix::AllocateNearRelay(base, id.sizeOfImage, base + 0x1000, base + 0x1800, 0x1000);
    if (!relay1 || !relay2) {
        if (relay1) VirtualFree(relay1, 0, MEM_RELEASE);
        if (relay2) VirtualFree(relay2, 0, MEM_RELEASE);
        std::printf("FAIL: AllocateNearRelay failed\n");
        return false;
    }

    static constexpr uint8_t kTargetCode[] = {0xB8, 0x42, 0x00, 0x00, 0x00, 0xC3};
    std::memcpy(relay2, kTargetCode, sizeof(kTargetCode));
    std::memset(relay1, 0xCC, 32);

    DWORD oldProt = 0;
    VirtualProtect(relay2, 0x1000, PAGE_EXECUTE_READ, &oldProt);
    VirtualProtect(relay1, 0x1000, PAGE_EXECUTE_READ, &oldProt);
    FlushInstructionCache(GetCurrentProcess(), relay2, 0x1000);

    const uint64_t r1 = reinterpret_cast<uint64_t>(relay1);
    const uint64_t r2 = reinterpret_cast<uint64_t>(relay2);
    const bool rejected = !bg3fix::WriteNearJmpPatch(0, r2, 5) && !bg3fix::WriteNearJmpPatch(r1, r2, 4);
    const bool patched = bg3fix::WriteNearJmpPatch(r1, r2, 5);
    const int retVal = patched ? reinterpret_cast<int (*)()>(relay1)() : -1;

    VirtualFree(relay1, 0, MEM_RELEASE);
    VirtualFree(relay2, 0, MEM_RELEASE);

    if (!rejected || !patched || retVal != 0x42) {
        std::printf("FAIL: NearRelay patch test rejected=%d patched=%d ret=%d\n", rejected, patched, retVal);
        return false;
    }
    std::printf("PASS: Near relay allocator, low-floor guard, and WriteNearJmpPatch\n");
    return true;
}

bool TestPeIdentityAndLogBuffer() {
    bg3fix::PeImageIdentity id{};
    if (bg3fix::ReadPeImageIdentity(0, id)) {
        std::printf("FAIL: ReadPeImageIdentity(0) succeeded\n");
        return false;
    }

    alignas(16) uint8_t badDos[256]{};
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(badDos);
    dos->e_magic = IMAGE_DOS_SIGNATURE;
    dos->e_lfanew = -16;
    if (bg3fix::ReadPeImageIdentity(reinterpret_cast<uint64_t>(badDos), id)) {
        std::printf("FAIL: ReadPeImageIdentity(negative e_lfanew) succeeded\n");
        return false;
    }

    bg3fix::LogBuffer buf;
    buf.PutDec(0);
    buf.PutChar(',');
    buf.PutDec(UINT64_MAX);
    buf.PutChar(',');
    buf.PutHex(0, 8);
    buf.PutChar(',');
    buf.PutHex(0xABCDEFULL, 1);
    const std::string formatted(buf.data, buf.len);
    if (formatted != "0,18446744073709551615,0x00000000,0xABCDEF") {
        std::printf("FAIL: LogBuffer format mismatch: '%s'\n", formatted.c_str());
        return false;
    }

    bg3fix::LogBuffer over;
    for (int i = 0; i < 4096; ++i) {
        over.PutChar('A');
    }
    if (over.len != sizeof(over.data)) {
        std::printf("FAIL: LogBuffer overflow clamp failed\n");
        return false;
    }

    std::printf("PASS: PE identity validation and LogBuffer guards\n");
    return true;
}

bool TestUniversalAobScannerUnit() {
    // 1. Unit test FindPatternUniqueInRange (Found / Ambiguous / NotFound) + ResolveRipRel32
    uint8_t hay[64]{};
    std::memset(hay, 0xCC, sizeof(hay));
    hay[10] = 0xAA; hay[11] = 0x11; hay[12] = 0x22; hay[13] = 0xBB;
    static constexpr uint8_t kUnitPat[] = {0xAA, 0x00, 0x00, 0xBB};
    static constexpr char kUnitMask[] = "x??x";
    uint64_t outAddr = 0;
    if (bg3fix::FindPatternUniqueInRange(hay, sizeof(hay), kUnitPat, kUnitMask, 4, outAddr) != bg3fix::ScanStatus::Found ||
        outAddr != reinterpret_cast<uint64_t>(hay + 10)) {
        std::printf("FAIL: Unit FindPatternUniqueInRange Found check failed\n");
        return false;
    }
    hay[40] = 0xAA; hay[41] = 0x99; hay[42] = 0x88; hay[43] = 0xBB;
    if (bg3fix::FindPatternUniqueInRange(hay, sizeof(hay), kUnitPat, kUnitMask, 4, outAddr) != bg3fix::ScanStatus::Ambiguous) {
        std::printf("FAIL: Unit FindPatternUniqueInRange Ambiguous check failed\n");
        return false;
    }
    static constexpr uint8_t kMissingPat[] = {0xDE, 0xAD, 0xBE, 0xEF};
    if (bg3fix::FindPatternUniqueInRange(hay, sizeof(hay), kMissingPat, "xxxx", 4, outAddr) != bg3fix::ScanStatus::NotFound) {
        std::printf("FAIL: Unit FindPatternUniqueInRange NotFound check failed\n");
        return false;
    }

    // 2. Synthetic executable buffer testing full ResolveMemTargets() positive + 4 negative mutations
    constexpr size_t kImgSize = 0x10000;
    std::vector<uint8_t> img(kImgSize, 0xCC);
    uint8_t* const raw = img.data();
    const uint64_t imgBase = reinterpret_cast<uint64_t>(raw);
    const bg3fix::PeSection execSec{imgBase + 0x1000ULL, 0x7000ULL};

    constexpr size_t kOffGAGet = 0x1100;
    constexpr size_t kOffSuperpage = 0x1400;
    constexpr size_t kOffPoolCtor = 0x1800;
    constexpr size_t kOffPoolBind = 0x1C00;
    constexpr size_t kOffPoolAlloc = 0x2000;
    constexpr size_t kOffLargeAlloc = 0x2400;
    constexpr size_t kOffGlobalPtr = 0x9000;
    constexpr size_t kOffGlobalBuf = 0x9100;

    static constexpr uint8_t kGAGet[] = {
        0x48, 0x83, 0xEC, 0x28, 0x48, 0x8B, 0x05, 0x00, 0x00, 0x00, 0x00, 0x48, 0x85, 0xC0, 0x0F, 0x85,
        0x00, 0x00, 0x00, 0x00, 0x48, 0x89, 0x6C, 0x24, 0x38, 0xBA, 0xA0, 0x0F, 0x00, 0x00, 0x48, 0x89,
        0x74, 0x24, 0x40, 0x4C, 0x89, 0x74, 0x24, 0x20, 0x4C, 0x8D, 0x35, 0x00, 0x00, 0x00, 0x00, 0x49,
        0x8B, 0xCE, 0xFF, 0x15, 0x00, 0x00, 0x00, 0x00, 0x0F, 0x57, 0xC0, 0x48, 0x8D, 0x0D, 0x00, 0x00,
        0x00, 0x00};
    static constexpr uint8_t kSuper[] = {
        0x48, 0x89, 0x5C, 0x24, 0x10, 0x56, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0xF1, 0xFF, 0x15, 0x00,
        0x00, 0x00, 0x00, 0x85, 0xC0, 0x75, 0x09, 0x48, 0x8B, 0xCE, 0xFF, 0x15, 0x00, 0x00, 0x00, 0x00,
        0x48, 0x8B, 0x5E, 0x30, 0x48, 0x85, 0xDB, 0x74, 0x28, 0x48, 0x8B, 0x56, 0x40, 0x48, 0x83, 0xFA,
        0x01, 0x72, 0x1E, 0x33, 0xC9, 0x48, 0x8D, 0x83, 0x00, 0x00, 0x02, 0x00};
    static constexpr uint8_t kCtor[] = {
        0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x33, 0xFF, 0x48, 0x8B, 0xD9, 0x48,
        0x89, 0x39, 0xBA, 0xA0, 0x0F, 0x00, 0x00, 0x48, 0x89, 0x79, 0x08, 0x48, 0x83, 0xC1, 0x10, 0xFF,
        0x15, 0x00, 0x00, 0x00, 0x00, 0x48, 0x8D, 0x4B, 0x48, 0x48, 0x89, 0x7B, 0x38, 0xBA, 0xA0, 0x0F,
        0x00, 0x00, 0x48, 0x89, 0x7B, 0x40, 0xFF, 0x15, 0x00, 0x00, 0x00, 0x00, 0x48, 0x8D, 0x8B, 0x80,
        0x00, 0x00, 0x00};
    static constexpr uint8_t kBind[] = {
        0x48, 0x89, 0x5C, 0x24, 0x18, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57,
        0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0xF9, 0x48, 0x89, 0x11, 0x48, 0x8B, 0xCA, 0x48, 0x8B, 0xF2,
        0xE8, 0x00, 0x00, 0x00, 0x00, 0x33, 0xED, 0x48, 0x89, 0x44, 0x24, 0x68, 0x4C, 0x8D, 0x44, 0x24,
        0x60, 0x48, 0x89, 0x6C, 0x24, 0x60, 0x48, 0x8D, 0x54, 0x24, 0x68, 0x48, 0x8B, 0xD8, 0xE8, 0x00,
        0x00, 0x00, 0x00, 0x48, 0x89, 0x5F, 0x08, 0x48, 0x8B, 0xCE, 0x48, 0x89, 0x77, 0x38, 0xE8, 0x00,
        0x00, 0x00, 0x00, 0xBB, 0x08, 0x00, 0x00, 0x00, 0x44, 0x8B, 0xC5, 0x44, 0x8B, 0xCB, 0x49, 0xBA,
        0x20, 0x00, 0x00, 0x11, 0xAD, 0xDE, 0xCE, 0xFA};
    static constexpr uint8_t kPoolAlloc[] = {
        0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x57,
        0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57, 0x48, 0x83, 0xEC, 0x20, 0xBD, 0xE0, 0x00, 0x00,
        0x00, 0x44, 0x8D, 0x6D, 0x40, 0x44, 0x8D, 0x65, 0x30, 0x44, 0x8D, 0x7D, 0x20, 0x44, 0x8D, 0x75,
        0x10, 0x8D, 0x75, 0xF0, 0x8D, 0x7D, 0xE0, 0x8D, 0x5D, 0xD0, 0x8D, 0x45, 0xA0, 0x44, 0x8D, 0x5D,
        0xC0, 0x44, 0x8D, 0x55, 0xB0, 0x48, 0x83, 0xFA, 0x10, 0x77, 0x1F, 0x49, 0x83, 0xF8, 0x40};
    static constexpr uint8_t kLargeAlloc[] = {
        0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x48,
        0x8B, 0xFA, 0x48, 0x8B, 0xF1, 0xE8, 0x56, 0x00, 0x00, 0x00, 0x48, 0x8B, 0xC8, 0x48, 0x8B, 0xD8,
        0xFF, 0x15, 0x00, 0x00, 0x00, 0x00, 0x85, 0xC0, 0x75, 0x09, 0x48, 0x8B, 0xCB, 0xFF, 0x15, 0x00,
        0x00, 0x00, 0x00, 0x48, 0x83, 0xFF, 0x10, 0x77, 0x0A, 0x48, 0x8B, 0xCE, 0xE8, 0x00, 0x00, 0x00,
        0x00, 0xEB, 0x0B, 0x4C, 0x8B, 0xC6, 0x48, 0x8B, 0xD7, 0xE8, 0x00, 0x00, 0x00, 0x00, 0x48, 0x8B,
        0xCB, 0x48, 0x8B, 0xF8, 0xFF, 0x15, 0x00, 0x00, 0x00, 0x00, 0x48, 0x8B, 0x5C, 0x24, 0x30, 0x48,
        0x8B, 0xC7, 0x48, 0x8B, 0x74, 0x24, 0x38, 0x48, 0x83, 0xC4, 0x20, 0x5F, 0xC3};

    std::memcpy(raw + kOffGAGet, kGAGet, sizeof(kGAGet));
    std::memcpy(raw + kOffSuperpage, kSuper, sizeof(kSuper));
    std::memcpy(raw + kOffPoolCtor, kCtor, sizeof(kCtor));
    std::memcpy(raw + kOffPoolBind, kBind, sizeof(kBind));
    std::memcpy(raw + kOffPoolAlloc, kPoolAlloc, sizeof(kPoolAlloc));
    std::memcpy(raw + kOffLargeAlloc, kLargeAlloc, sizeof(kLargeAlloc));

    // Patch RIP-rel globals + internal call edges inside mock functions.
    WriteRel32At(raw, imgBase + kOffGAGet + 0x04ULL, 3, 7, imgBase + kOffGlobalPtr);
    WriteRel32At(raw, imgBase + kOffGAGet + 0x28ULL, 3, 7, imgBase + kOffGlobalBuf);
    WriteRel32At(raw, imgBase + kOffGAGet + 0x3BULL, 3, 7, imgBase + kOffGlobalBuf + bg3fix::kOffsetPoolManager);

    raw[kOffGAGet + 0x74] = 0xE8;
    WriteRel32At(raw, imgBase + kOffGAGet + 0x74ULL, 1, 5, imgBase + kOffPoolCtor);
    raw[kOffGAGet + 0x107] = 0xE8;
    WriteRel32At(raw, imgBase + kOffGAGet + 0x107ULL, 1, 5, imgBase + kOffPoolBind);
    WriteRel32At(raw, imgBase + kOffPoolBind + 0x4EULL, 1, 5, imgBase + kOffSuperpage);

    static constexpr uint8_t kChunkCapCmp[] = {0x83, 0xBE, 0xC8, 0x00, 0x00, 0x00, 0x10};
    std::memcpy(raw + kOffSuperpage + 0x5B, kChunkCapCmp, sizeof(kChunkCapCmp));

    bg3fix::ResolvedMemTargets res{};
    const char* failReason = nullptr;
    if (!bg3fix::ResolveMemTargets(imgBase, static_cast<uint32_t>(kImgSize), &execSec, 1, res, failReason) ||
        res.fnGAGet != imgBase + kOffGAGet ||
        res.fnGAAllocSuperpage != imgBase + kOffSuperpage ||
        res.fnPoolManagerCtor != imgBase + kOffPoolCtor ||
        res.fnPoolBindBackend != imgBase + kOffPoolBind ||
        res.fnPoolAlloc != imgBase + kOffPoolAlloc ||
        res.fnLargeAlloc != imgBase + kOffLargeAlloc ||
        res.addrGlobalAllocator != imgBase + kOffGlobalPtr ||
        res.addrGlobalAllocatorStorage != imgBase + kOffGlobalBuf) {
        std::printf("FAIL: Synthetic ResolveMemTargets failed (%s)\n", failReason ? failReason : "addr mismatch");
        return false;
    }

    // Negative 1: Wrong PoolManager offset (+0xF0 instead of +0xE8)
    WriteRel32At(raw, imgBase + kOffGAGet + 0x3BULL, 3, 7, imgBase + kOffGlobalBuf + 0xF0);
    if (bg3fix::ResolveMemTargets(imgBase, static_cast<uint32_t>(kImgSize), &execSec, 1, res, failReason)) {
        std::printf("FAIL: ResolveMemTargets accepted bad PoolManager offset\n");
        return false;
    }
    WriteRel32At(raw, imgBase + kOffGAGet + 0x3BULL, 3, 7, imgBase + kOffGlobalBuf + bg3fix::kOffsetPoolManager);

    // Negative 2: Missing 16-chunk cap cmp
    raw[kOffSuperpage + 0x5B + 6] = 0x99;
    if (bg3fix::ResolveMemTargets(imgBase, static_cast<uint32_t>(kImgSize), &execSec, 1, res, failReason)) {
        std::printf("FAIL: ResolveMemTargets accepted missing 16-chunk cap check\n");
        return false;
    }
    raw[kOffSuperpage + 0x5B + 6] = 0x10;

    // Negative 3: Broken Pool_BindBackend -> GA_AllocSuperpage call edge
    WriteRel32At(raw, imgBase + kOffPoolBind + 0x4EULL, 1, 5, imgBase + kOffPoolAlloc);
    if (bg3fix::ResolveMemTargets(imgBase, static_cast<uint32_t>(kImgSize), &execSec, 1, res, failReason)) {
        std::printf("FAIL: ResolveMemTargets accepted broken Pool_BindBackend call edge\n");
        return false;
    }
    WriteRel32At(raw, imgBase + kOffPoolBind + 0x4EULL, 1, 5, imgBase + kOffSuperpage);

    // Negative 4: Ambiguous duplicate pattern
    std::memcpy(raw + 0x3000, kCtor, sizeof(kCtor));
    if (bg3fix::ResolveMemTargets(imgBase, static_cast<uint32_t>(kImgSize), &execSec, 1, res, failReason) ||
        !failReason || std::strstr(failReason, "ambiguous") == nullptr) {
        std::printf("FAIL: ResolveMemTargets did not reject duplicate ambiguous pattern (%s)\n",
                    failReason ? failReason : "succeeded");
        return false;
    }

    std::printf("PASS: Universal AOB scanner unit tests (positive synthetic image + 4 negative guards)\n");
    return true;
}

bool ReadFileToString(const std::wstring& path, std::string& out) {
    out.clear();
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        return false;
    }
    char chunk[1024]{};
    DWORD read = 0;
    while (ReadFile(h, chunk, sizeof(chunk), &read, nullptr) && read > 0) {
        out.append(chunk, read);
    }
    CloseHandle(h);
    return true;
}

bool TestLiveLoadUnloadRejection() {
    wchar_t exePath[MAX_PATH]{};
    const DWORD n = GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) {
        std::printf("FAIL: GetModuleFileNameW failed\n");
        return false;
    }
    std::wstring buildDir(exePath);
    const size_t slash = buildDir.find_last_of(L"\\/");
    if (slash == std::wstring::npos) {
        std::printf("FAIL: Unable to locate build dir\n");
        return false;
    }
    buildDir.resize(slash + 1);

    wchar_t tempRoot[MAX_PATH]{};
    if (GetTempPathW(MAX_PATH, tempRoot) == 0) {
        std::printf("FAIL: GetTempPathW failed\n");
        return false;
    }
    wchar_t subDir[64]{};
    std::swprintf(subDir, 64, L"bg3memfix_test_%lu\\", GetCurrentProcessId());
    const std::wstring tempDir = std::wstring(tempRoot) + subDir;
    if (!CreateDirectoryW(tempDir.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) {
        std::printf("FAIL: CreateDirectoryW failed\n");
        return false;
    }

    const std::wstring srcDll = buildDir + L"00_BG3MemFix.dll";
    const std::wstring dstDll = tempDir + L"00_BG3MemFix.dll";
    const std::wstring logCur = tempDir + L"BG3MemFix.log";
    const std::wstring logPrev = tempDir + L"BG3MemFix-prev.log";

    auto Cleanup = [&]() {
        DeleteFileW(logCur.c_str());
        DeleteFileW(logPrev.c_str());
        DeleteFileW(dstDll.c_str());
        RemoveDirectoryW(tempDir.c_str());
    };

    if (!CopyFileW(srcDll.c_str(), dstDll.c_str(), FALSE)) {
        Cleanup();
        std::printf("FAIL: CopyFileW failed for %ls\n", srcDll.c_str());
        return false;
    }

    for (int cycle = 0; cycle < 2; ++cycle) {
        HMODULE hMod = LoadLibraryW(dstDll.c_str());
        if (!hMod) {
            Cleanup();
            std::printf("FAIL: LoadLibraryW failed on cycle %d (%lu)\n", cycle, GetLastError());
            return false;
        }
        FreeLibrary(hMod);
    }

    std::string curText, prevText;
    const bool ok = ReadFileToString(logCur, curText) && ReadFileToString(logPrev, prevText);
    Cleanup();

    if (!ok ||
        curText.find("ERROR: Unsupported BG3 binary.") == std::string::npos ||
        prevText.find("ERROR: Unsupported BG3 binary.") == std::string::npos ||
        curText.find("DETACH") != std::string::npos) {
        std::printf("FAIL: Unexpected LoadLibrary rejection log behavior\n");
        return false;
    }

    std::printf("PASS: Live LoadLibraryW/FreeLibrary safe non-BG3 rejection and log rotation\n");
    return true;
}

bool TestRealPeImageScan(const std::wstring& exePath, const char* label) {
    HANDLE hFile = CreateFileW(exePath.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                               nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE) {
        std::printf("SKIP: %s not present (%ls)\n", label, exePath.c_str());
        return true;
    }

    LARGE_INTEGER liSize{};
    if (!GetFileSizeEx(hFile, &liSize) || liSize.QuadPart < 0x1000) {
        CloseHandle(hFile);
        std::printf("FAIL: Invalid file size for %s\n", label);
        return false;
    }

    std::vector<uint8_t> raw(static_cast<size_t>(liSize.QuadPart));
    size_t totalRead = 0;
    while (totalRead < raw.size()) {
        const DWORD chunk = static_cast<DWORD>(
            (raw.size() - totalRead > 0x1000000ULL) ? 0x1000000ULL : (raw.size() - totalRead));
        DWORD got = 0;
        if (!ReadFile(hFile, raw.data() + totalRead, chunk, &got, nullptr) || got == 0) {
            CloseHandle(hFile);
            std::printf("FAIL: ReadFile failed for %s\n", label);
            return false;
        }
        totalRead += got;
    }
    CloseHandle(hFile);

    const uint64_t rawBase = reinterpret_cast<uint64_t>(raw.data());
    bg3fix::PeImageIdentity id{};
    if (!bg3fix::ReadPeImageIdentity(rawBase, id) || id.sizeOfImage == 0) {
        std::printf("FAIL: Invalid PE identity for %s\n", label);
        return false;
    }

    auto* mapped = static_cast<uint8_t*>(
        VirtualAlloc(nullptr, id.sizeOfImage, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (!mapped) {
        std::printf("FAIL: VirtualAlloc(%u) failed for %s\n", id.sizeOfImage, label);
        return false;
    }

    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(raw.data());
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(raw.data() + dos->e_lfanew);
    const DWORD hdrSize = nt->OptionalHeader.SizeOfHeaders;
    if (hdrSize <= raw.size() && hdrSize <= id.sizeOfImage) {
        std::memcpy(mapped, raw.data(), hdrSize);
    }

    const auto* secHdr = reinterpret_cast<const IMAGE_SECTION_HEADER*>(
        reinterpret_cast<const uint8_t*>(&nt->OptionalHeader) + nt->FileHeader.SizeOfOptionalHeader);
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
        const auto& s = secHdr[i];
        if (s.SizeOfRawData == 0 || s.PointerToRawData + s.SizeOfRawData > raw.size()) {
            continue;
        }
        if (s.VirtualAddress >= id.sizeOfImage) {
            continue;
        }
        const size_t maxDst = id.sizeOfImage - s.VirtualAddress;
        const size_t copySz = (s.SizeOfRawData <= maxDst) ? s.SizeOfRawData : maxDst;
        std::memcpy(mapped + s.VirtualAddress, raw.data() + s.PointerToRawData, copySz);
    }

    const uint64_t mappedBase = reinterpret_cast<uint64_t>(mapped);
    bg3fix::PeSection execSecs[16]{};
    const size_t secCount = bg3fix::FindExecutableSections(mappedBase, execSecs, 16);

    const ULONGLONG t0 = GetTickCount64();
    bg3fix::ResolvedMemTargets res{};
    const char* failReason = nullptr;
    const bool ok = bg3fix::ResolveMemTargets(mappedBase, id.sizeOfImage, execSecs, secCount, res, failReason);
    const ULONGLONG scanMs = GetTickCount64() - t0;

    if (ok) {
        std::printf(
            "PASS: Real %s scan (%llums, TS=0x%08X): GA_Get=0x%llX Superpage=0x%llX "
            "PoolCtor=0x%llX PoolBind=0x%llX PoolAlloc=0x%llX LargeAlloc=0x%llX "
            "GlobalPtr=0x%llX GlobalBuf=0x%llX\n",
            label,
            scanMs,
            id.timeDateStamp,
            res.fnGAGet - mappedBase,
            res.fnGAAllocSuperpage - mappedBase,
            res.fnPoolManagerCtor - mappedBase,
            res.fnPoolBindBackend - mappedBase,
            res.fnPoolAlloc - mappedBase,
            res.fnLargeAlloc - mappedBase,
            res.addrGlobalAllocator - mappedBase,
            res.addrGlobalAllocatorStorage - mappedBase);
    } else {
        std::printf("FAIL: Real %s scan failed (%s)\n", label, failReason ? failReason : "unknown");
    }

    VirtualFree(mapped, 0, MEM_RELEASE);
    return ok;
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    if (!TestMultiThreadCommitRace()) return 1;
    if (!TestCommitBoundaryEdgeCases()) return 1;
    if (!TestReservedArenaAndLayer2Spill()) return 1;
    if (!TestNearRelayAndPatching()) return 1;
    if (!TestPeIdentityAndLogBuffer()) return 1;
    if (!TestUniversalAobScannerUnit()) return 1;
    if (!TestLiveLoadUnloadRejection()) return 1;

    std::wstring bg3BinDir;
    for (int i = 1; i + 1 < argc; ++i) {
        if (std::wcscmp(argv[i], L"--bg3-bin") == 0) {
            bg3BinDir = argv[i + 1];
        }
    }
    if (bg3BinDir.empty()) {
        wchar_t envBuf[MAX_PATH]{};
        const DWORD len = GetEnvironmentVariableW(L"BG3_BIN_DIR", envBuf, MAX_PATH);
        if (len > 0 && len < MAX_PATH) {
            bg3BinDir = envBuf;
        }
    }

    if (!bg3BinDir.empty()) {
        if (bg3BinDir.back() != L'\\' && bg3BinDir.back() != L'/') {
            bg3BinDir.push_back(L'\\');
        }
        if (!TestRealPeImageScan(bg3BinDir + L"bg3.exe", "bg3.exe (Vulkan)")) return 1;
        if (!TestRealPeImageScan(bg3BinDir + L"bg3_dx11.exe", "bg3_dx11.exe (DX11)")) return 1;
    } else {
        std::printf("SKIP: Real BG3 binary scan skipped (pass --bg3-bin <dir> to verify installed binaries)\n");
    }

    std::printf("ALL SELF-TESTS PASSED\n");
    return 0;
}
