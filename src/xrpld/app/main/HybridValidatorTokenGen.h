#pragma once

#include <xrpl/basics/Buffer.h>
#include <xrpl/protocol/PublicKey.h>
#include <xrpl/protocol/SecretKey.h>
#include <xrpl/protocol/Seed.h>

#include <cstdint>
#include <optional>
#include <string>

namespace xrpl::detail {

/** Inputs for hybrid validator-token generation.
 *
 *  Each seed is optional: when std::nullopt the generator draws fresh
 *  randomness. Providing seeds gives deterministic / reproducible tokens,
 *  which is handy when standing up a known-pubkey DevNet.
 */
struct HybridTokenInputs
{
    std::optional<Seed> masterEccSeed;
    std::optional<Buffer> masterPqSeed;
    std::string domain;
    std::uint32_t sequence = 1;
};

/** Outputs from hybrid validator-token generation.
 *
 *  Everything needed to (a) print the [validator_token] payload that goes
 *  into the validator node's xrpld.cfg, and (b) record the master
 *  identities for the operator to keep offline and to publish to peers.
 */
struct HybridTokenOutput
{
    Seed masterEccSeed;
    Buffer masterPqSeed;
    PublicKey masterEccPubKey;
    Buffer masterPqPubKey;

    PublicKey ephemeralEccPubKey;
    Buffer ephemeralPqPubKey;

    /// Base64 of the validator-token JSON, ready to paste under
    /// [validator_token] in xrpld.cfg (one or more lines, no leading
    /// or trailing whitespace).
    std::string validatorTokenBase64;
};

/** Generate a hybrid validator token: ECC + ML-DSA-44 master / ephemeral
 *  manifest, signed four ways, plus the ephemeral secrets that the
 *  running validator needs.
 *
 *  Throws std::runtime_error on input validation failure.
 */
HybridTokenOutput
generateHybridValidatorToken(HybridTokenInputs const& in);

}  // namespace xrpl::detail
