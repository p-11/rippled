#include <xrpld/app/misc/ValidatorKeys.h>

#include <xrpld/core/Config.h>
#include <xrpld/core/ConfigSections.h>

#include <xrpl/basics/Log.h>
#include <xrpl/basics/Slice.h>
#include <xrpl/basics/base64.h>
#include <xrpl/beast/utility/Journal.h>
#include <xrpl/protocol/KeyType.h>
#include <xrpl/protocol/PQSign.h>
#include <xrpl/protocol/PublicKey.h>
#include <xrpl/protocol/SecretKey.h>
#include <xrpl/protocol/Seed.h>
#include <xrpl/server/Manifest.h>

#include <utility>

namespace xrpl {

// Fixed message signed and verified at startup to prove the validator
// token's PQ secret corresponds to the manifest's PQ ephemeral pubkey.
static constexpr char kPqProbe[] = "validator-keys-pq-self-test";

ValidatorKeys::ValidatorKeys(Config const& config, beast::Journal j)
{
    if (config.exists(SECTION_VALIDATOR_TOKEN) && config.exists(SECTION_VALIDATION_SEED))
    {
        configInvalid_ = true;
        JLOG(j.fatal()) << "Cannot specify both [" SECTION_VALIDATION_SEED
                           "] and [" SECTION_VALIDATOR_TOKEN "]";
        return;
    }

    if (config.exists(SECTION_VALIDATOR_TOKEN))
    {
        // token is non-const so it can be moved from
        if (auto token = loadValidatorToken(config.section(SECTION_VALIDATOR_TOKEN).lines()))
        {
            auto const pk = derivePublicKey(KeyType::Secp256k1, token->validationSecret);
            auto const m = deserializeManifest(base64Decode(token->manifest));

            bool const manifestHybrid = m && m->quantumMasterKey && m->quantumSigningKey;
            bool const tokenHybrid = token->pqValidationSecret.has_value();

            if (!m || pk != m->signingKey || (manifestHybrid != tokenHybrid))
            {
                configInvalid_ = true;
                JLOG(j.fatal()) << "Invalid token specified in [" SECTION_VALIDATOR_TOKEN "]";
            }
            else if (
                manifestHybrid &&
                !pqVerify(
                    Slice{m->quantumSigningKey->data(), m->quantumSigningKey->size()},
                    Slice{kPqProbe, sizeof(kPqProbe) - 1},
                    pqSign(
                        Slice{token->pqValidationSecret->data(), token->pqValidationSecret->size()},
                        Slice{kPqProbe, sizeof(kPqProbe) - 1})))
            {
                // The token's PQ secret must produce signatures the manifest's
                // declared PQ ephemeral verifies. There is no sk->pk derivation
                // helper for ML-DSA, so prove correspondence with a
                // sign/verify round-trip. Without this, a mismatched secret
                // starts up cleanly and every validation we emit is silently
                // dropped by all peers.
                configInvalid_ = true;
                JLOG(j.fatal()) << "PQ validation secret does not match the manifest "
                                   "PQ ephemeral key in [" SECTION_VALIDATOR_TOKEN "]";
            }
            else
            {
                keys.emplace(m->masterKey, pk, token->validationSecret);
                if (manifestHybrid)
                {
                    keys->pqMasterPublicKey = m->quantumMasterKey;
                    keys->pqPublicKey = m->quantumSigningKey;
                    keys->pqSecretKey = std::move(token->pqValidationSecret);
                }
                nodeID = calcNodeID(m->masterKey);
                sequence = m->sequence;
                manifest = std::move(token->manifest);
            }
        }
        else
        {
            configInvalid_ = true;
            JLOG(j.fatal()) << "Invalid token specified in [" SECTION_VALIDATOR_TOKEN "]";
        }
    }
    else if (config.exists(SECTION_VALIDATION_SEED))
    {
        auto const seed =
            parseBase58<Seed>(config.section(SECTION_VALIDATION_SEED).lines().front());
        if (!seed)
        {
            configInvalid_ = true;
            JLOG(j.fatal()) << "Invalid seed specified in [" SECTION_VALIDATION_SEED "]";
        }
        else
        {
            SecretKey const sk = generateSecretKey(KeyType::Secp256k1, *seed);
            PublicKey const pk = derivePublicKey(KeyType::Secp256k1, sk);
            keys.emplace(pk, pk, sk);
            nodeID = calcNodeID(pk);
            sequence = 0;
        }
    }
}
}  // namespace xrpl
