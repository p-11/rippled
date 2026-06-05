#pragma once

#include <xrpl/basics/Buffer.h>
#include <xrpl/beast/utility/Journal.h>
#include <xrpl/protocol/PublicKey.h>
#include <xrpl/protocol/SecretKey.h>
#include <xrpl/protocol/UintTypes.h>

#include <optional>
#include <string>

namespace xrpl {

class Config;

/** Validator keys and manifest as set in configuration file.  Values will be
    empty if not configured as a validator or not configured with a manifest.
*/
class ValidatorKeys
{
public:
    // Group all keys in a struct. Either all keys are valid or none are.
    struct Keys
    {
        PublicKey masterPublicKey;
        PublicKey publicKey;
        SecretKey secretKey;

        // Hybrid validators: ML-DSA-44 master pubkey, ephemeral pubkey,
        // and ephemeral secret key. Either all three are set or none are.
        // The master and ephemeral pubkeys mirror the values published in
        // the manifest; the secret key is supplied by the validator token.
        std::optional<Buffer> pqMasterPublicKey;
        std::optional<Buffer> pqPublicKey;
        std::optional<Buffer> pqSecretKey;

        Keys() = delete;
        Keys(PublicKey const& masterPublic, PublicKey const& pub, SecretKey const& secret)
            : masterPublicKey(masterPublic), publicKey(pub), secretKey(secret)
        {
        }
    };

    // Note: The existence of keys cannot be used as a proxy for checking the
    // validity of a configuration. It is possible to have a valid
    // configuration while not setting the keys, as per the constructor of
    // the ValidatorKeys class.
    std::optional<Keys> keys;
    NodeID nodeID;
    std::string manifest;
    std::uint32_t sequence = 0;

    ValidatorKeys() = delete;
    ValidatorKeys(Config const& config, beast::Journal j);

    [[nodiscard]] bool
    configInvalid() const
    {
        return configInvalid_;
    }

private:
    bool configInvalid_ = false;  //< Set to true if config was invalid
};

}  // namespace xrpl
