#pragma once

#include <xrpl/basics/Buffer.h>
#include <xrpl/json/json_value.h>

#include <ecc_custody_mock.h>
#include <pq_custody_mock.h>

#include <cstdint>
#include <string>
#include <vector>

namespace pqwallet::sign {

// Artifacts of one hybrid signing pass. sign-tx persists these into its
// JSON sidecar; opt-in only consumes txBlobHex.
struct HybridSignResult
{
    std::string txBlobHex;
    std::string txHash;
    // HashPrefix::TxSign || serialize(tx without signing fields): the exact
    // bytes both signatures commit to.
    std::vector<std::uint8_t> payload;
    xrpl::Buffer eccSignature;
    xrpl::Buffer pqSignature;
};

// Parse `txJson` into an STTx, attach the PQ pubkey, and sign the canonical
// payload with both custody mocks. Single definition of the hybrid-sign
// pipeline so the bytes both signatures commit to cannot drift between
// commands. `what` prefixes error messages.
HybridSignResult
hybridSignFromJson(
    std::string const& what,
    json::Value const& txJson,
    custody::EccCustodyMock const& eccCustody,
    custody::PqCustodyMock const& pqCustody);

}  // namespace pqwallet::sign
