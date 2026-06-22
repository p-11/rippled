#pragma once

#include <atomic>
#include <chrono>
#include <string_view>

namespace xrpl {

// A process-global timing sink used by the benchmark probes. libxrpl cannot
// depend on xrpld (where PerfLog lives), and the verification call sites are
// free/static/member functions with no Application handle, so the probe reaches
// its consumer through a single settable function pointer instead. xrpld
// installs a PerfLog-backed sink at startup; when no sink is installed (unit
// tests, the CLI, any libxrpl consumer) the probe is a no-op.
using ProbeSink = void (*)(std::string_view tag, std::chrono::microseconds dur);

void
setProbeSink(ProbeSink sink) noexcept;

ProbeSink
getProbeSink() noexcept;

// RAII probe: samples a steady clock at construction and reports the elapsed
// time to the installed sink on destruction. The sink is captured once at
// construction, so it stays consistent for the probe's lifetime and the clock
// is not even read when no sink is installed.
class BenchProbe
{
    ProbeSink sink_;
    std::string_view tag_;
    std::chrono::steady_clock::time_point t0_;

public:
    explicit BenchProbe(std::string_view tag) noexcept
        : sink_(getProbeSink())
        , tag_(tag)
        , t0_(sink_ ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{})
    {
    }

    ~BenchProbe()
    {
        if (sink_)
            sink_(
                tag_,
                std::chrono::duration_cast<std::chrono::microseconds>(
                    std::chrono::steady_clock::now() - t0_));
    }

    BenchProbe(BenchProbe const&) = delete;
    BenchProbe&
    operator=(BenchProbe const&) = delete;
};

}  // namespace xrpl
