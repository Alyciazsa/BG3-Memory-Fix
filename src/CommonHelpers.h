#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#include <windows.h>

#include <cstdint>
#include <cstring>

namespace bg3fix {

inline uint64_t AlignUpU64(uint64_t value, uint64_t alignment) noexcept {
    return (value + alignment - 1ULL) & ~(alignment - 1ULL);
}

inline void WriteAbsJump(uint8_t* dst, uint64_t target) noexcept {
    dst[0] = 0xFF;
    dst[1] = 0x25;
    dst[2] = 0x00;
    dst[3] = 0x00;
    dst[4] = 0x00;
    dst[5] = 0x00;
    std::memcpy(dst + 6, &target, sizeof(target));
}

// Stack-only formatting helper for Win32 WriteFile logging (zero CRT heap or USER32 calls).
struct LogBuffer {
    char data[2048]{};
    DWORD len = 0;

    void PutChar(char c) noexcept {
        if (len < sizeof(data)) {
            data[len++] = c;
        }
    }

    void PutStr(const char* s) noexcept {
        if (!s) {
            return;
        }
        while (*s && len < sizeof(data)) {
            data[len++] = *s++;
        }
    }

    void PutWideAsAscii(const wchar_t* s) noexcept {
        if (!s) {
            return;
        }
        while (*s && len < sizeof(data)) {
            const wchar_t wc = *s++;
            data[len++] = (wc >= 0x20 && wc <= 0x7E) ? static_cast<char>(wc) : '?';
        }
    }

    void PutDec(uint64_t v) noexcept {
        char tmp[24]{};
        int n = 0;
        if (v == 0) {
            PutChar('0');
            return;
        }
        while (v > 0 && n < 24) {
            tmp[n++] = static_cast<char>('0' + (v % 10ULL));
            v /= 10ULL;
        }
        while (n > 0) {
            PutChar(tmp[--n]);
        }
    }

    void PutDecPad2(uint32_t v) noexcept {
        PutChar(static_cast<char>('0' + ((v / 10U) % 10U)));
        PutChar(static_cast<char>('0' + (v % 10U)));
    }

    void PutDecPad4(uint32_t v) noexcept {
        PutChar(static_cast<char>('0' + ((v / 1000U) % 10U)));
        PutChar(static_cast<char>('0' + ((v / 100U) % 10U)));
        PutChar(static_cast<char>('0' + ((v / 10U) % 10U)));
        PutChar(static_cast<char>('0' + (v % 10U)));
    }

    void PutHex(uint64_t v, int minDigits = 1) noexcept {
        static constexpr char kHex[] = "0123456789ABCDEF";
        if (minDigits < 1) {
            minDigits = 1;
        }
        char tmp[16]{};
        int n = 0;
        while (v > 0 && n < 16) {
            tmp[n++] = kHex[v & 0xFULL];
            v >>= 4;
        }
        while (n < minDigits && n < 16) {
            tmp[n++] = '0';
        }
        PutStr("0x");
        while (n > 0) {
            PutChar(tmp[--n]);
        }
    }

    void PutTimestamp() noexcept {
        SYSTEMTIME st{};
        GetLocalTime(&st);
        PutChar('[');
        PutDecPad4(st.wYear);
        PutChar('-');
        PutDecPad2(st.wMonth);
        PutChar('-');
        PutDecPad2(st.wDay);
        PutChar(' ');
        PutDecPad2(st.wHour);
        PutChar(':');
        PutDecPad2(st.wMinute);
        PutChar(':');
        PutDecPad2(st.wSecond);
        PutStr("] ");
    }

    void FlushToFile(HANDLE hFile) noexcept {
        if (hFile != INVALID_HANDLE_VALUE && len > 0) {
            DWORD written = 0;
            WriteFile(hFile, data, len, &written, nullptr);
        }
        len = 0;
    }
};

inline void BuildLogPath(HMODULE hModule, const wchar_t* logFileName, wchar_t (&outPath)[MAX_PATH]) noexcept {
    outPath[0] = L'\0';
    if (!logFileName || logFileName[0] == L'\0') {
        return;
    }

    wchar_t modPath[MAX_PATH]{};
    const DWORD n = GetModuleFileNameW(hModule, modPath, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) {
        return;
    }

    int lastSlash = -1;
    for (DWORD i = 0; i < n; ++i) {
        if (modPath[i] == L'\\' || modPath[i] == L'/') {
            lastSlash = static_cast<int>(i);
        }
    }

    size_t nameLen = 0;
    while (nameLen < MAX_PATH && logFileName[nameLen] != L'\0') {
        ++nameLen;
    }
    if (nameLen == 0 || nameLen >= MAX_PATH) {
        return;
    }

    const size_t dirLen = (lastSlash >= 0) ? static_cast<size_t>(lastSlash + 1) : 0;
    if (dirLen + nameLen + 1 + 8 >= MAX_PATH) {
        return;
    }
    if (dirLen > 0) {
        std::memcpy(outPath, modPath, dirLen * sizeof(wchar_t));
    }
    std::memcpy(outPath + dirLen, logFileName, (nameLen + 1) * sizeof(wchar_t));
}

inline const wchar_t* GetModuleBaseName(HMODULE hModule, wchar_t (&buf)[MAX_PATH]) noexcept {
    buf[0] = L'\0';
    const DWORD n = GetModuleFileNameW(hModule, buf, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) {
        return L"<unknown>";
    }
    int lastSlash = -1;
    for (DWORD i = 0; i < n; ++i) {
        if (buf[i] == L'\\' || buf[i] == L'/') {
            lastSlash = static_cast<int>(i);
        }
    }
    return (lastSlash >= 0) ? (buf + lastSlash + 1) : buf;
}

inline bool EndsWithLogExt(const wchar_t* path, size_t len) noexcept {
    if (len < 4) {
        return false;
    }
    const auto toLower = [](wchar_t c) noexcept -> wchar_t {
        return (c >= L'A' && c <= L'Z') ? static_cast<wchar_t>(c + (L'a' - L'A')) : c;
    };
    return path[len - 4] == L'.' &&
           toLower(path[len - 3]) == L'l' &&
           toLower(path[len - 2]) == L'o' &&
           toLower(path[len - 1]) == L'g';
}

inline HANDLE OpenLogFile(const wchar_t* logPath, bool rotateExisting) noexcept {
    if (!logPath || logPath[0] == L'\0') {
        return INVALID_HANDLE_VALUE;
    }
    if (rotateExisting) {
        size_t len = 0;
        while (len < MAX_PATH && logPath[len] != L'\0') {
            ++len;
        }
        if (EndsWithLogExt(logPath, len) && len + 5 < MAX_PATH) {
            wchar_t prevPath[MAX_PATH]{};
            std::memcpy(prevPath, logPath, (len - 4) * sizeof(wchar_t));
            static constexpr wchar_t kPrevSuffix[] = L"-prev.log";
            std::memcpy(prevPath + (len - 4), kPrevSuffix, sizeof(kPrevSuffix));
            MoveFileExW(logPath, prevPath, MOVEFILE_REPLACE_EXISTING);
        }
    }
    return CreateFileW(
        logPath,
        FILE_APPEND_DATA,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr,
        OPEN_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
}

inline void LogLine(HANDLE hFile, const char* msg) noexcept {
    LogBuffer buf;
    buf.PutTimestamp();
    buf.PutStr(msg);
    buf.PutStr("\r\n");
    buf.FlushToFile(hFile);
}

struct PeImageIdentity {
    uint32_t timeDateStamp = 0;
    uint32_t sizeOfImage = 0;
};

struct PeSection {
    uint64_t start = 0;
    uint64_t size = 0;
};

enum class ScanStatus {
    NotFound,
    Found,
    Ambiguous,
};

inline bool ReadPeImageIdentity(uint64_t base, PeImageIdentity& out) noexcept {
    out = PeImageIdentity{};
    if (base == 0) {
        return false;
    }
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
        return false;
    }
    if (dos->e_lfanew <= 0 || dos->e_lfanew > 0x01000000L) {
        return false;
    }
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + static_cast<uint32_t>(dos->e_lfanew));
    if (nt->Signature != IMAGE_NT_SIGNATURE || nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) {
        return false;
    }
    out.timeDateStamp = nt->FileHeader.TimeDateStamp;
    out.sizeOfImage = nt->OptionalHeader.SizeOfImage;
    return true;
}

inline size_t FindExecutableSections(uint64_t base, PeSection* outSecs, size_t maxSecs) noexcept {
    if (base == 0 || !outSecs || maxSecs == 0) {
        return 0;
    }
    PeImageIdentity id{};
    if (!ReadPeImageIdentity(base, id) || id.sizeOfImage == 0) {
        return 0;
    }
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + static_cast<uint32_t>(dos->e_lfanew));
    const WORD numSections = nt->FileHeader.NumberOfSections;
    const WORD optSize = nt->FileHeader.SizeOfOptionalHeader;
    const auto* secHdr = reinterpret_cast<const IMAGE_SECTION_HEADER*>(
        reinterpret_cast<const uint8_t*>(&nt->OptionalHeader) + optSize);

    size_t count = 0;
    for (WORD i = 0; i < numSections && count < maxSecs; ++i) {
        const auto& s = secHdr[i];
        if ((s.Characteristics & IMAGE_SCN_MEM_EXECUTE) == 0) {
            continue;
        }
        if (s.VirtualAddress >= id.sizeOfImage) {
            continue;
        }
        const uint64_t rawSpan = (s.Misc.VirtualSize > 0) ? s.Misc.VirtualSize : s.SizeOfRawData;
        const uint64_t maxSpan = static_cast<uint64_t>(id.sizeOfImage) - s.VirtualAddress;
        const uint64_t clampedSpan = (rawSpan <= maxSpan) ? rawSpan : maxSpan;
        if (clampedSpan == 0) {
            continue;
        }
        outSecs[count].start = base + s.VirtualAddress;
        outSecs[count].size = clampedSpan;
        ++count;
    }
    return count;
}

inline bool IsRareAnchorByte(uint8_t b) noexcept {
    switch (b) {
        case 0x00:
        case 0x24:
        case 0x48:
        case 0x89:
        case 0x8B:
        case 0x90:
        case 0xCC:
        case 0xFF:
            return false;
        default:
            return true;
    }
}

inline ScanStatus FindPatternUniqueInRange(
    const uint8_t* start,
    size_t size,
    const uint8_t* pat,
    const char* mask,
    size_t patLen,
    uint64_t& outAddr) noexcept {
    outAddr = 0;
    if (!start || !pat || !mask || patLen == 0 || size < patLen) {
        return ScanStatus::NotFound;
    }
    if (std::strlen(mask) < patLen) {
        return ScanStatus::NotFound;
    }

    size_t anchorIdx = patLen;
    size_t fallbackIdx = patLen;
    for (size_t i = 0; i < patLen; ++i) {
        if (mask[i] == 'x') {
            if (fallbackIdx == patLen) {
                fallbackIdx = i;
            }
            if (IsRareAnchorByte(pat[i])) {
                anchorIdx = i;
                break;
            }
        }
    }
    if (anchorIdx == patLen) {
        anchorIdx = fallbackIdx;
    }
    if (anchorIdx == patLen) {
        return ScanStatus::NotFound;
    }

    const uint8_t anchorByte = pat[anchorIdx];
    const uint8_t* cursor = start + anchorIdx;
    const uint8_t* const endCursor = start + size - (patLen - anchorIdx);

    bool haveMatch = false;
    uint64_t firstMatch = 0;

    while (cursor <= endCursor) {
        const size_t remaining = static_cast<size_t>(endCursor - cursor) + 1ULL;
        const void* hit = std::memchr(cursor, anchorByte, remaining);
        if (!hit) {
            break;
        }
        const uint8_t* const cand = static_cast<const uint8_t*>(hit) - anchorIdx;
        bool matched = true;
        for (size_t j = 0; j < patLen; ++j) {
            if (mask[j] == 'x' && cand[j] != pat[j]) {
                matched = false;
                break;
            }
        }
        if (matched) {
            if (haveMatch) {
                return ScanStatus::Ambiguous;
            }
            haveMatch = true;
            firstMatch = reinterpret_cast<uint64_t>(cand);
        }
        cursor = static_cast<const uint8_t*>(hit) + 1;
    }

    if (!haveMatch) {
        return ScanStatus::NotFound;
    }
    outAddr = firstMatch;
    return ScanStatus::Found;
}

inline ScanStatus FindPatternUniqueInExecSections(
    const PeSection* secs,
    size_t secCount,
    const uint8_t* pat,
    const char* mask,
    size_t patLen,
    uint64_t& outAddr,
    size_t* outSecIdx = nullptr) noexcept {
    outAddr = 0;
    if (outSecIdx) {
        *outSecIdx = 0;
    }
    if (!secs || secCount == 0) {
        return ScanStatus::NotFound;
    }

    bool haveMatch = false;
    uint64_t matchAddr = 0;
    size_t matchSec = 0;

    for (size_t i = 0; i < secCount; ++i) {
        uint64_t localMatch = 0;
        const ScanStatus st = FindPatternUniqueInRange(
            reinterpret_cast<const uint8_t*>(secs[i].start),
            static_cast<size_t>(secs[i].size),
            pat,
            mask,
            patLen,
            localMatch);
        if (st == ScanStatus::Ambiguous) {
            return ScanStatus::Ambiguous;
        }
        if (st == ScanStatus::Found) {
            if (haveMatch) {
                return ScanStatus::Ambiguous;
            }
            haveMatch = true;
            matchAddr = localMatch;
            matchSec = i;
        }
    }

    if (!haveMatch) {
        return ScanStatus::NotFound;
    }
    outAddr = matchAddr;
    if (outSecIdx) {
        *outSecIdx = matchSec;
    }
    return ScanStatus::Found;
}

inline uint64_t ResolveRipRel32(uint64_t instrAddr, size_t dispOffset, size_t instrLen) noexcept {
    int32_t disp = 0;
    std::memcpy(&disp, reinterpret_cast<const void*>(instrAddr + dispOffset), sizeof(disp));
    return static_cast<uint64_t>(
        static_cast<int64_t>(instrAddr + instrLen) + static_cast<int64_t>(disp));
}

inline bool HasRel32CallToTarget(
    uint64_t searchStart,
    size_t searchLen,
    uint64_t sectionEnd,
    uint64_t expectedTarget) noexcept {
    if (searchStart == 0 || searchStart >= sectionEnd || searchLen < 5) {
        return false;
    }
    const uint64_t maxAvailable = sectionEnd - searchStart;
    const size_t boundedLen = (static_cast<uint64_t>(searchLen) <= maxAvailable)
        ? searchLen
        : static_cast<size_t>(maxAvailable);
    if (boundedLen < 5) {
        return false;
    }
    const auto* code = reinterpret_cast<const uint8_t*>(searchStart);
    for (size_t i = 0; i + 5 <= boundedLen; ++i) {
        if (code[i] == 0xE8 && ResolveRipRel32(searchStart + i, 1, 5) == expectedTarget) {
            return true;
        }
    }
    return false;
}

inline bool ContainsBytes(
    uint64_t searchStart,
    size_t searchLen,
    uint64_t sectionEnd,
    const uint8_t* needle,
    size_t needleLen) noexcept {
    if (searchStart == 0 || searchStart >= sectionEnd || !needle || needleLen == 0 || searchLen < needleLen) {
        return false;
    }
    const uint64_t maxAvailable = sectionEnd - searchStart;
    const size_t boundedLen = (static_cast<uint64_t>(searchLen) <= maxAvailable)
        ? searchLen
        : static_cast<size_t>(maxAvailable);
    if (boundedLen < needleLen) {
        return false;
    }
    const auto* code = reinterpret_cast<const uint8_t*>(searchStart);
    for (size_t i = 0; i + needleLen <= boundedLen; ++i) {
        if (code[i] == needle[0] && std::memcmp(code + i, needle, needleLen) == 0) {
            return true;
        }
    }
    return false;
}

inline bool FitsRel32(uint64_t fromNextIp, uint64_t target) noexcept {
    const int64_t diff = static_cast<int64_t>(target) - static_cast<int64_t>(fromNextIp);
    return diff >= -0x7F000000LL && diff <= 0x7F000000LL;
}

inline uint8_t* AllocateNearRelay(
    uint64_t base,
    uint32_t sizeOfImage,
    uint64_t minAddr,
    uint64_t maxAddr,
    size_t relaySize) noexcept {
    constexpr uint64_t kGranularity = 0x10000ULL;
    constexpr uint64_t kMinSafeAddr = 0x100000ULL;
    constexpr uint64_t kMaxSearchSpan = 0x7E000000ULL;

    if (base == 0 || relaySize == 0 || relaySize > kGranularity || base > UINT64_MAX - sizeOfImage) {
        return nullptr;
    }
    if (minAddr > maxAddr) {
        const uint64_t tmp = minAddr;
        minAddr = maxAddr;
        maxAddr = tmp;
    }

    const auto TryRange = [&](uint64_t start, uint64_t limit, bool upward) -> uint8_t* {
        if ((upward && start > limit) || (!upward && start < limit)) {
            return nullptr;
        }
        uint64_t addr = start;
        MEMORY_BASIC_INFORMATION mbi{};
        for (int iter = 0; iter < 4096; ++iter) {
            if (VirtualQuery(reinterpret_cast<const void*>(addr), &mbi, sizeof(mbi)) == 0) {
                break;
            }
            const uint64_t regBase = reinterpret_cast<uint64_t>(mbi.BaseAddress);
            const uint64_t regEnd = regBase + mbi.RegionSize;
            if (regEnd <= regBase) {
                break;
            }

            if (mbi.State == MEM_FREE && mbi.RegionSize >= relaySize) {
                if (upward) {
                    const uint64_t floorAddr = (regBase > addr) ? regBase : addr;
                    uint64_t cand = AlignUpU64(floorAddr, kGranularity);
                    if (cand < kMinSafeAddr) {
                        cand = kMinSafeAddr;
                    }
                    for (int retry = 0; retry < 256 && cand >= regBase && cand <= limit && cand + relaySize <= regEnd; ++retry) {
                        if (FitsRel32(maxAddr, cand) && FitsRel32(minAddr, cand + relaySize)) {
                            void* p = VirtualAlloc(
                                reinterpret_cast<void*>(cand),
                                relaySize,
                                MEM_RESERVE | MEM_COMMIT,
                                PAGE_READWRITE);
                            if (p) {
                                return static_cast<uint8_t*>(p);
                            }
                        }
                        if (cand > UINT64_MAX - kGranularity) {
                            break;
                        }
                        cand += kGranularity;
                    }
                } else {
                    const uint64_t maxSlotInReg = (regEnd - relaySize) & ~(kGranularity - 1ULL);
                    const uint64_t maxSlotFromAddr = addr & ~(kGranularity - 1ULL);
                    uint64_t cand = (maxSlotInReg < maxSlotFromAddr) ? maxSlotInReg : maxSlotFromAddr;
                    const uint64_t minSlot = (regBase > limit) ? regBase : limit;
                    for (int retry = 0; retry < 256 && cand >= minSlot && cand >= kMinSafeAddr && cand + relaySize <= regEnd; ++retry) {
                        if (FitsRel32(maxAddr, cand) && FitsRel32(minAddr, cand + relaySize)) {
                            void* p = VirtualAlloc(
                                reinterpret_cast<void*>(cand),
                                relaySize,
                                MEM_RESERVE | MEM_COMMIT,
                                PAGE_READWRITE);
                            if (p) {
                                return static_cast<uint8_t*>(p);
                            }
                        }
                        if (cand < kGranularity) {
                            break;
                        }
                        cand -= kGranularity;
                    }
                }
            }

            if (upward) {
                if (regEnd <= addr || regEnd > limit) {
                    break;
                }
                addr = regEnd;
            } else {
                if (regBase <= kGranularity || regBase <= limit) {
                    break;
                }
                addr = regBase - kGranularity;
            }
        }
        return nullptr;
    };

    const uint64_t alignedBase = base & ~(kGranularity - 1ULL);
    if (alignedBase >= kMinSafeAddr + kGranularity) {
        const uint64_t lowStart = alignedBase - kGranularity;
        const uint64_t relLowBound = (maxAddr > kMaxSearchSpan) ? (maxAddr - kMaxSearchSpan) : kMinSafeAddr;
        const uint64_t lowLimit = (relLowBound > kMinSafeAddr) ? relLowBound : kMinSafeAddr;
        if (lowStart >= lowLimit) {
            if (uint8_t* p = TryRange(lowStart, lowLimit, false)) {
                return p;
            }
        }
    }

    const uint64_t rawHighStart = base + sizeOfImage;
    if (rawHighStart > UINT64_MAX - (kGranularity - 1ULL)) {
        return nullptr;
    }
    uint64_t highStart = AlignUpU64(rawHighStart, kGranularity);
    if (highStart < kMinSafeAddr) {
        highStart = kMinSafeAddr;
    }
    const uint64_t highLimit =
        (minAddr <= UINT64_MAX - kMaxSearchSpan) ? (minAddr + kMaxSearchSpan) : (UINT64_MAX - kGranularity);
    return TryRange(highStart, highLimit, true);
}

inline bool WriteNearJmpPatch(uint64_t patchAddr, uint64_t targetAddr, size_t totalBytes) noexcept {
    if (patchAddr == 0 || targetAddr == 0 || totalBytes < 5 || patchAddr > UINT64_MAX - totalBytes) {
        return false;
    }
    if (!FitsRel32(patchAddr + 5ULL, targetAddr)) {
        return false;
    }
    const int32_t rel = static_cast<int32_t>(
        static_cast<int64_t>(targetAddr) - static_cast<int64_t>(patchAddr + 5ULL));

    DWORD oldProt = 0;
    if (!VirtualProtect(reinterpret_cast<void*>(patchAddr), totalBytes, PAGE_EXECUTE_READWRITE, &oldProt)) {
        return false;
    }
    auto* dst = reinterpret_cast<uint8_t*>(patchAddr);
    dst[0] = 0xE9;
    std::memcpy(dst + 1, &rel, sizeof(rel));
    for (size_t i = 5; i < totalBytes; ++i) {
        dst[i] = 0x90;
    }
    FlushInstructionCache(GetCurrentProcess(), dst, totalBytes);
    VirtualProtect(reinterpret_cast<void*>(patchAddr), totalBytes, oldProt, &oldProt);
    return true;
}

} // namespace bg3fix
