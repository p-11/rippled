#include <xrpl/basics/BenchProbe.h>

#include <atomic>

namespace xrpl {

namespace {
std::atomic<ProbeSink> g_probeSink{nullptr};
}  // namespace

void
setProbeSink(ProbeSink sink) noexcept
{
    g_probeSink.store(sink, std::memory_order_relaxed);
}

ProbeSink
getProbeSink() noexcept
{
    return g_probeSink.load(std::memory_order_relaxed);
}

}  // namespace xrpl
