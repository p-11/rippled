#pragma once

#include <xrpl/basics/Buffer.h>
#include <xrpl/basics/Slice.h>
#include <xrpl/protocol/KeyType.h>
#include <xrpl/protocol/PublicKey.h>
#include <xrpl/protocol/SecretKey.h>
#include <xrpl/protocol/Seed.h>

#include <filesystem>
#include <optional>

namespace pqwallet::custody {

// Mock standing in for the Ripple Custody Application (MPC + HSM). In
// production every method below is a remote call to the real Custody
// service. The mock keeps its secret material in its own state file,
// which the wallet binary never opens; only this module does.
class EccCustodyMock
{
public:
    explicit EccCustodyMock(std::filesystem::path stateFile);

    void
    initializeFromSeed(xrpl::Seed const& seed, xrpl::KeyType type);

    // Throws on missing or malformed state.
    void
    load();

    [[nodiscard]] xrpl::PublicKey
    publicKey() const;

    // The blackbox sign request. Set RIPPLE_CUSTODY_FAIL=1 to make the
    // mock return an invalid signature, proving the server-side hybrid
    // verification is what gates acceptance.
    [[nodiscard]] xrpl::Buffer
    signWithECC(xrpl::Slice payload) const;

    [[nodiscard]] std::filesystem::path const&
    stateFile() const noexcept;

private:
    void
    persist() const;

    std::filesystem::path stateFile_;
    xrpl::KeyType keyType_{xrpl::KeyType::Secp256k1};
    std::optional<xrpl::PublicKey> publicKey_;
    std::optional<xrpl::SecretKey> secretKey_;
};

}  // namespace pqwallet::custody
