#pragma once

#include <xrpl/basics/Buffer.h>
#include <xrpl/basics/Slice.h>

#include <filesystem>
#include <optional>

namespace pqwallet::custody {

// Parallel module on the PQ side, where a PQ-capable HSM would plug in.
// Same shape and file-isolation discipline as EccCustodyMock. Persists
// only the 32-byte seed; pqKeypair() re-expands it on load.
class PqCustodyMock
{
public:
    explicit PqCustodyMock(std::filesystem::path stateFile);

    [[nodiscard]] bool
    exists() const;

    void
    initializeFromPqSeed(xrpl::Slice seed);

    // Throws on missing or malformed state.
    void
    load();

    [[nodiscard]] xrpl::Buffer
    publicKey() const;

    // The blackbox sign request. PQ_CUSTODY_FAIL=1 mirrors
    // RIPPLE_CUSTODY_FAIL on the ECC side.
    [[nodiscard]] xrpl::Buffer
    signWithPq(xrpl::Slice payload) const;

    [[nodiscard]] std::filesystem::path const&
    stateFile() const noexcept;

private:
    void
    persist() const;

    std::filesystem::path stateFile_;
    std::optional<xrpl::Buffer> pqSeed_;
    std::optional<xrpl::Buffer> publicKey_;
    std::optional<xrpl::Buffer> secretKey_;
};

}  // namespace pqwallet::custody
