#pragma once

#include <xrpl/basics/Buffer.h>
#include <xrpl/basics/Slice.h>
#include <xrpl/json/json_value.h>

#include <ecc_custody_mock.h>
#include <pq_custody_mock.h>

#include <cstdint>
#include <functional>
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

// Artifacts of an ECC-only signing pass: no PQ fields are attached, so a
// transaction signed this way is rejected with tefBAD_AUTH once its source
// account has opted in to hybrid. Used by `fund` (genesis key) and by
// `pay --ecc-only` (the bad-weather negative test).
struct EccSignResult
{
    std::string txBlobHex;
    std::string txHash;
};

// Callback that produces the ECC signature over the canonical payload. The
// indirection lets `fund` sign with a raw genesis key and `pay --ecc-only`
// sign through the custody mock (preserving its blackbox boundary and the
// RIPPLE_CUSTODY_FAIL hook) without duplicating the serialization.
using EccSigner = std::function<xrpl::Buffer(xrpl::Slice)>;

// Parse `txJson` into an STTx and sign the canonical payload with `eccSign`,
// attaching only sfTxnSignature. `what` prefixes error messages.
EccSignResult
eccSignFromJson(std::string const& what, json::Value const& txJson, EccSigner const& eccSign);

}  // namespace pqwallet::sign
