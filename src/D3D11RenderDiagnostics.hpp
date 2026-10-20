#pragma once

// v12 bounded, deduplicated diagnostics for rendering-relevant decisions and
// errors (representation choice, rejected copies, capability mismatches).
// Every distinct signature prints on first occurrence and again when its
// repeat count reaches 10, 100, 1000, ... so a fault repeated every frame is
// visible without flooding the log. At most MaxSignatures distinct signatures
// are tracked; later new signatures are counted as "overflow".
#include <windows.h>

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <mutex>

namespace ashes::D3D11_NAMESPACE::rdiag
{
    inline constexpr uint32_t MaxSignatures = 256u;

    struct Entry
    {
        uint64_t signature{};
        uint64_t count{};
        char kind[48]{};
    };

    struct Table
    {
        std::mutex mutex;
        Entry entries[MaxSignatures]{};
        uint32_t used{};
        uint64_t overflow{};
    };

    inline Table &table()
    {
        static Table result;
        return result;
    }

    inline uint64_t hashBytes(uint64_t seed, void const *data, size_t size) noexcept
    {
        auto const *bytes = static_cast<uint8_t const *>(data);
        uint64_t hash = seed ^ 0xcbf29ce484222325ull;
        for (size_t i = 0u; i < size; ++i)
        {
            hash ^= bytes[i];
            hash *= 0x100000001b3ull;
        }
        return hash;
    }

    inline uint64_t hashText(uint64_t seed, char const *text) noexcept
    {
        size_t length{};
        while (text && text[length]) ++length;
        return hashBytes(seed, text, length);
    }

    inline bool shouldPrint(uint64_t count) noexcept
    {
        if (count <= 1u) return true;
        uint64_t power = 10u;
        while (power < count && power < 1000000000000ull) power *= 10u;
        return power == count;
    }

    // Records one occurrence; prints `[RW-GPU-DIAG] kind=... sig=... count=... <message>`
    // when the occurrence is first or reaches a power of ten.
    inline void report(char const *kind, uint64_t signature, char const *format, ...)
    {
        uint64_t count{};
        bool overflowed{};
        {
            auto &t = table();
            std::lock_guard<std::mutex> lock{ t.mutex };
            Entry *entry{};
            for (uint32_t i = 0u; i < t.used; ++i)
            {
                if (t.entries[i].signature == signature)
                {
                    entry = &t.entries[i];
                    break;
                }
            }
            if (!entry && t.used < MaxSignatures)
            {
                entry = &t.entries[t.used++];
                entry->signature = signature;
                std::snprintf(entry->kind, sizeof(entry->kind), "%s", kind ? kind : "unknown");
            }
            if (entry)
            {
                count = ++entry->count;
            }
            else
            {
                overflowed = true;
                count = ++t.overflow;
            }
        }
        if (!shouldPrint(count)) return;
        char message[768]{};
        va_list args;
        va_start(args, format);
        std::vsnprintf(message, sizeof(message), format, args);
        va_end(args);
        std::fprintf(stderr, "[RW-GPU-DIAG] kind=%s sig=%016llx count=%llu%s %s\n",
            kind ? kind : "unknown", static_cast<unsigned long long>(signature),
            static_cast<unsigned long long>(count), overflowed ? " overflow=1" : "", message);
        std::fflush(stderr);
    }

    // One summary line per tracked signature; called at device destruction.
    inline void summary()
    {
        auto &t = table();
        std::lock_guard<std::mutex> lock{ t.mutex };
        if (!t.used && !t.overflow) return;
        std::fprintf(stderr, "[RW-GPU-DIAG-SUMMARY] signatures=%u overflow=%llu\n", t.used,
            static_cast<unsigned long long>(t.overflow));
        for (uint32_t i = 0u; i < t.used; ++i)
        {
            std::fprintf(stderr, "[RW-GPU-DIAG-SUMMARY] kind=%s sig=%016llx count=%llu\n", t.entries[i].kind,
                static_cast<unsigned long long>(t.entries[i].signature),
                static_cast<unsigned long long>(t.entries[i].count));
        }
        std::fflush(stderr);
    }
}
