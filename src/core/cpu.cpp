#include "core/cpu.hpp"

#include <cstdint>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#if defined(_M_X64) || defined(_M_IX86) || defined(__x86_64__) || defined(__i386__)
#define AXIOM_X86 1
#endif

#if defined(AXIOM_X86)
#if defined(_MSC_VER)
#include <intrin.h>
#else
#include <cpuid.h>
#endif
#endif

namespace axiom::core {
namespace {

#if defined(AXIOM_X86)
void cpuid(unsigned leaf, unsigned subleaf, unsigned out[4]) {
#if defined(_MSC_VER)
    int regs[4];
    __cpuidex(regs, static_cast<int>(leaf), static_cast<int>(subleaf));
    out[0] = static_cast<unsigned>(regs[0]);
    out[1] = static_cast<unsigned>(regs[1]);
    out[2] = static_cast<unsigned>(regs[2]);
    out[3] = static_cast<unsigned>(regs[3]);
#else
    unsigned a = 0, b = 0, c = 0, d = 0;
    __cpuid_count(leaf, subleaf, a, b, c, d);
    out[0] = a;
    out[1] = b;
    out[2] = c;
    out[3] = d;
#endif
}

// Read XCR0 (the extended-control register that says which vector state the OS
// preserves). Only called once OSXSAVE is known set, so xgetbv is safe to issue.
std::uint64_t read_xcr0() {
#if defined(_MSC_VER)
    return _xgetbv(0);
#else
    std::uint32_t eax = 0;
    std::uint32_t edx = 0;
    __asm__ volatile("xgetbv" : "=a"(eax), "=d"(edx) : "c"(0));
    return (static_cast<std::uint64_t>(edx) << 32) | eax;
#endif
}
#endif  // AXIOM_X86

CpuFeatures detect() {
    CpuFeatures f;
#if defined(AXIOM_X86)
    unsigned regs[4] = {0, 0, 0, 0};
    cpuid(0, 0, regs);
    const unsigned max_leaf = regs[0];

    if (max_leaf >= 1) {
        cpuid(1, 0, regs);
        const unsigned ecx = regs[2];
        f.sse41 = (ecx >> 19) & 1u;
        f.sse42 = (ecx >> 20) & 1u;
        f.pclmul = (ecx >> 1) & 1u;
        f.popcnt = (ecx >> 23) & 1u;

        const bool osxsave = (ecx >> 27) & 1u;
        const bool avx = (ecx >> 28) & 1u;
        // AVX2 is usable only if the OS preserves YMM state across context
        // switches (XCR0 bits 1 and 2 set), in addition to the CPU advertising it.
        const bool ymm_enabled = osxsave && avx && ((read_xcr0() & 0x6u) == 0x6u);

        if (ymm_enabled && max_leaf >= 7) {
            cpuid(7, 0, regs);
            f.avx2 = (regs[1] >> 5) & 1u;  // EBX bit 5
        }
    }
#endif
    return f;
}

}  // namespace

const CpuFeatures& cpu_features() {
    static const CpuFeatures features = detect();
    return features;
}

namespace {

std::size_t detect_logical_processors() {
#if defined(_WIN32)
    const auto count = GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
    if (count != 0) {
        return static_cast<std::size_t>(count);
    }
#endif
    const auto fallback =
        static_cast<std::size_t>(std::thread::hardware_concurrency());
    return fallback != 0 ? fallback : 1;
}

std::size_t detect_physical_cores() {
#if defined(_WIN32)
    DWORD length = 0;
    GetLogicalProcessorInformationEx(RelationProcessorCore, nullptr, &length);
    if (GetLastError() == ERROR_INSUFFICIENT_BUFFER && length != 0) {
        std::vector<std::uint8_t> buffer(length);
        if (GetLogicalProcessorInformationEx(
                RelationProcessorCore,
                reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(buffer.data()),
                &length)) {
            std::size_t cores = 0;
            std::size_t offset = 0;
            while (offset < length) {
                const auto* info =
                    reinterpret_cast<const SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(
                        buffer.data() + offset);
                if (info->Relationship == RelationProcessorCore) {
                    ++cores;
                }
                offset += info->Size;
            }
            if (cores != 0) {
                return cores;
            }
        }
    }
#endif
    return logical_processor_count();
}

}  // namespace

std::size_t logical_processor_count() {
    static const std::size_t processors = detect_logical_processors();
    return processors;
}

void bind_current_thread_to_processor_group(std::size_t worker_index) {
#if defined(_WIN32)
    const auto group_count = GetActiveProcessorGroupCount();
    if (group_count <= 1) {
        return;
    }

    auto slot = worker_index % logical_processor_count();
    for (WORD group = 0; group < group_count; ++group) {
        const auto active = GetActiveProcessorCount(group);
        if (active == 0) {
            continue;
        }
        if (slot >= active) {
            slot -= active;
            continue;
        }

        constexpr auto kAffinityBits = sizeof(KAFFINITY) * 8;
        GROUP_AFFINITY affinity{};
        affinity.Group = group;
        affinity.Mask = active >= kAffinityBits
            ? ~KAFFINITY{0}
            : (KAFFINITY{1} << active) - 1;
        (void)SetThreadGroupAffinity(GetCurrentThread(), &affinity, nullptr);
        return;
    }
#else
    (void)worker_index;
#endif
}

std::size_t physical_core_count() {
    static const std::size_t cores = detect_physical_cores();
    return cores;
}

const char* cpu_features_string() {
    static const std::string text = [] {
        const auto& f = cpu_features();
        std::string s;
        auto add = [&](bool on, const char* name) {
            if (on) {
                if (!s.empty()) {
                    s += ' ';
                }
                s += name;
            }
        };
        add(f.sse41, "sse4.1");
        add(f.sse42, "sse4.2");
        add(f.pclmul, "pclmul");
        add(f.popcnt, "popcnt");
        add(f.avx2, "avx2");
        return s.empty() ? std::string("scalar") : s;
    }();
    return text.c_str();
}

}  // namespace axiom::core
