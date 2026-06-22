#include <test/jtx/Env.h>
#include <test/jtx/envconfig.h>
#include <test/unit_test/utils.h>

#include <xrpld/app/main/HybridValidatorTokenGen.h>
#include <xrpld/app/misc/ValidatorKeys.h>
#include <xrpld/core/Config.h>
#include <xrpld/core/ConfigSections.h>

#include <xrpl/basics/Slice.h>
#include <xrpl/basics/base64.h>
#include <xrpl/basics/strHex.h>
#include <xrpl/beast/unit_test/suite.h>
#include <xrpl/beast/utility/Journal.h>
#include <xrpl/protocol/HashPrefix.h>
#include <xrpl/protocol/KeyType.h>
#include <xrpl/protocol/PQSign.h>
#include <xrpl/protocol/PublicKey.h>
#include <xrpl/protocol/SField.h>
#include <xrpl/protocol/STObject.h>
#include <xrpl/protocol/SecretKey.h>
#include <xrpl/protocol/Seed.h>
#include <xrpl/protocol/Serializer.h>
#include <xrpl/protocol/Sign.h>
#include <xrpl/protocol/UintTypes.h>
#include <xrpl/protocol/detail/mldsa.h>
#include <xrpl/protocol/tokens.h>
#include <xrpl/server/Manifest.h>

#include <string>
#include <vector>

namespace xrpl::test {

class ValidatorKeys_test : public beast::unit_test::Suite
{
    // Used with [validation_seed]
    std::string const seed_ = "shUwVw52ofnCUX5m7kPTKzJdr4HEH";

    // Used with [validation_token]
    std::string const tokenSecretStr_ = "paQmjZ37pKKPMrgadBLsuf9ab7Y7EUNzh27LQrZqoexpAs31nJi";

    std::vector<std::string> const tokenBlob_ = {
        "    eyJ2YWxpZGF0aW9uX3NlY3JldF9rZXkiOiI5ZWQ0NWY4NjYyNDFjYzE4YTI3NDdiNT\n",
        " \tQzODdjMDYyNTkwNzk3MmY0ZTcxOTAyMzFmYWE5Mzc0NTdmYTlkYWY2IiwibWFuaWZl     \n",
        "\tc3QiOiJKQUFBQUFGeEllMUZ0d21pbXZHdEgyaUNjTUpxQzlnVkZLaWxHZncxL3ZDeE\n",
        "\t hYWExwbGMyR25NaEFrRTFhZ3FYeEJ3RHdEYklENk9NU1l1TTBGREFscEFnTms4U0tG\t  \t\n",
        "bjdNTzJmZGtjd1JRSWhBT25ndTlzQUtxWFlvdUorbDJWMFcrc0FPa1ZCK1pSUzZQU2\n",
        "hsSkFmVXNYZkFpQnNWSkdlc2FhZE9KYy9hQVpva1MxdnltR21WcmxIUEtXWDNZeXd1\n",
        "NmluOEhBU1FLUHVnQkQ2N2tNYVJGR3ZtcEFUSGxHS0pkdkRGbFdQWXk1QXFEZWRGdj\n",
        "VUSmEydzBpMjFlcTNNWXl3TFZKWm5GT3I3QzBrdzJBaVR6U0NqSXpkaXRROD0ifQ==\n",
    };

    std::string const tokenManifest_ =
        "JAAAAAFxIe1FtwmimvGtH2iCcMJqC9gVFKilGfw1/vCxHXXLplc2GnMhAkE1agqXxBwD"
        "wDbID6OMSYuM0FDAlpAgNk8SKFn7MO2fdkcwRQIhAOngu9sAKqXYouJ+l2V0W+sAOkVB"
        "+ZRS6PShlJAfUsXfAiBsVJGesaadOJc/aAZokS1vymGmVrlHPKWX3Yywu6in8HASQKPu"
        "gBD67kMaRFGvmpATHlGKJdvDFlWPYy5AqDedFv5TJa2w0i21eq3MYywLVJZnFOr7C0kw"
        "2AiTzSCjIzditQ8=";

    // Manifest does not match private key
    std::vector<std::string> const invalidTokenBlob_ = {
        "eyJtYW5pZmVzdCI6IkpBQUFBQVZ4SWUyOVVBdzViZFJudHJ1elVkREk4aDNGV1JWZl\n",
        "k3SXVIaUlKQUhJd3MxdzZzM01oQWtsa1VXQWR2RnFRVGRlSEpvS1pNY0hlS0RzOExo\n",
        "b3d3bDlHOEdkVGNJbmFka1l3UkFJZ0h2Q01lQU1aSzlqQnV2aFhlaFRLRzVDQ3BBR1\n",
        "k0bGtvZHRXYW84UGhzR3NDSUREVTA1d1c3bWNiMjlVNkMvTHBpZmgvakZPRGhFR21i\n",
        "NWF6dTJMVHlqL1pjQkpBbitmNGhtQTQ0U0tYbGtTTUFqak1rSWRyR1Rxa21SNjBzVG\n",
        "JaTjZOOUYwdk9UV3VYcUZ6eDFoSGIyL0RqWElVZXhDVGlITEcxTG9UdUp1eXdXbk55\n",
        "RFE9PSIsInZhbGlkYXRpb25fc2VjcmV0X2tleSI6IjkyRDhCNDBGMzYwMTc5MTkwMU\n",
        "MzQTUzMzI3NzBDMkUwMTA4MDI0NTZFOEM2QkI0NEQ0N0FFREQ0NzJGMDQ2RkYifQ==\n",
    };

public:
    // Build a hybrid manifest + validator token. `pqTokenSecret` is the PQ
    // secret embedded in the token; pass the matching ephemeral secret for a
    // valid token or an unrelated one for a mismatch.
    static std::vector<std::string>
    makeHybridToken(
        SecretKey const& masterSecret,
        SecretKey const& ephSecret,
        Slice pqMasterPub,
        Slice pqMasterSec,
        Slice pqEphPub,
        Slice pqEphSec,
        Slice pqTokenSecret)
    {
        auto const masterPublic = derivePublicKey(KeyType::Ed25519, masterSecret);
        auto const ephPublic = derivePublicKey(KeyType::Secp256k1, ephSecret);

        STObject st(sfGeneric);
        st[sfSequence] = 1;
        st[sfPublicKey] = masterPublic;
        st[sfSigningPubKey] = ephPublic;
        st.setFieldVL(sfQuantumPubKey, pqEphPub);
        st.setFieldVL(sfQuantumMasterPublicKey, pqMasterPub);
        sign(st, HashPrefix::Manifest, KeyType::Secp256k1, ephSecret);
        sign(st, HashPrefix::Manifest, KeyType::Ed25519, masterSecret, sfMasterSignature);
        pqSign(st, HashPrefix::Manifest, pqEphSec, sfQuantumSignature);
        pqSign(st, HashPrefix::Manifest, pqMasterSec, sfQuantumMasterSignature);

        Serializer s;
        st.add(s);
        std::string const manifestB64 =
            base64Encode(std::string(static_cast<char const*>(s.data()), s.size()));

        std::string const json = "{\"validation_secret_key\":\"" +
            strHex(Slice{ephSecret.data(), ephSecret.size()}) +
            "\",\"pq_validation_secret_key\":\"" + strHex(pqTokenSecret) + "\",\"manifest\":\"" +
            manifestB64 + "\"}";
        return {base64Encode(json)};
    }

    void
    run() override
    {
        // We're only using Env for its Journal.  That Journal gives better
        // coverage in unit tests.
        test::jtx::Env env{*this, test::jtx::envconfig(), nullptr, beast::Severity::Disabled};
        beast::Journal const journal{env.app().getJournal("ValidatorKeys_test")};

        // Keys/ID when using [validation_seed]
        SecretKey const seedSecretKey =
            // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
            generateSecretKey(KeyType::Secp256k1, *parseBase58<Seed>(seed_));
        PublicKey const seedPublicKey = derivePublicKey(KeyType::Secp256k1, seedSecretKey);
        NodeID const seedNodeID = calcNodeID(seedPublicKey);

        // Keys when using [validation_token]
        auto const tokenSecretKey =
            // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
            *parseBase58<SecretKey>(TokenType::NodePrivate, tokenSecretStr_);

        auto const tokenPublicKey = derivePublicKey(KeyType::Secp256k1, tokenSecretKey);

        auto const m = deserializeManifest(base64Decode(tokenManifest_));
        BEAST_EXPECT(m);

        // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
        NodeID const tokenNodeID = calcNodeID(m->masterKey);

        {
            // No config -> no key but valid
            Config const c;
            ValidatorKeys const k{c, journal};
            BEAST_EXPECT(!k.keys);
            BEAST_EXPECT(k.manifest.empty());
            BEAST_EXPECT(!k.configInvalid());
        }
        {
            // validation seed section -> empty manifest and valid seeds
            Config c;
            c.section(SECTION_VALIDATION_SEED).append(seed_);

            ValidatorKeys k{c, journal};
            if (BEAST_EXPECT(k.keys); k.keys.has_value())
            {
                BEAST_EXPECT(k.keys->publicKey == seedPublicKey);
                BEAST_EXPECT(test::equal(k.keys->secretKey, seedSecretKey));
            }
            BEAST_EXPECT(k.nodeID == seedNodeID);
            BEAST_EXPECT(k.manifest.empty());
            BEAST_EXPECT(!k.configInvalid());
        }

        {
            // validation seed bad seed -> invalid
            Config c;
            c.section(SECTION_VALIDATION_SEED).append("badseed");

            ValidatorKeys const k{c, journal};
            BEAST_EXPECT(k.configInvalid());
            BEAST_EXPECT(!k.keys);
            BEAST_EXPECT(k.manifest.empty());
        }

        {
            // validator token
            Config c;
            c.section(SECTION_VALIDATOR_TOKEN).append(tokenBlob_);
            ValidatorKeys k{c, journal};

            if (BEAST_EXPECT(k.keys); k.keys.has_value())
            {
                BEAST_EXPECT(k.keys->publicKey == tokenPublicKey);
                BEAST_EXPECT(test::equal(k.keys->secretKey, tokenSecretKey));
            }
            BEAST_EXPECT(k.nodeID == tokenNodeID);
            BEAST_EXPECT(k.manifest == tokenManifest_);
            BEAST_EXPECT(!k.configInvalid());
        }
        {
            // invalid validator token
            Config c;
            c.section(SECTION_VALIDATOR_TOKEN).append("badtoken");
            ValidatorKeys const k{c, journal};
            BEAST_EXPECT(k.configInvalid());
            BEAST_EXPECT(!k.keys);
            BEAST_EXPECT(k.manifest.empty());
        }

        {
            // Cannot specify both
            Config c;
            c.section(SECTION_VALIDATION_SEED).append(seed_);
            c.section(SECTION_VALIDATOR_TOKEN).append(tokenBlob_);
            ValidatorKeys const k{c, journal};

            BEAST_EXPECT(k.configInvalid());
            BEAST_EXPECT(!k.keys);
            BEAST_EXPECT(k.manifest.empty());
        }

        {
            // Token manifest and private key must match
            Config c;
            c.section(SECTION_VALIDATOR_TOKEN).append(invalidTokenBlob_);
            ValidatorKeys const k{c, journal};

            BEAST_EXPECT(k.configInvalid());
            BEAST_EXPECT(!k.keys);
            BEAST_EXPECT(k.manifest.empty());
        }

        {
            // Hybrid token whose PQ secret matches the manifest -> valid.
            auto const masterSecret = randomSecretKey();
            auto const [ephPub, ephSec] = randomKeyPair(KeyType::Secp256k1);
            auto [pqMPub, pqMSec] = mldsa::keypair();
            auto [pqEPub, pqESec] = mldsa::keypair();

            Config c;
            c.section(SECTION_VALIDATOR_TOKEN)
                .append(makeHybridToken(
                    masterSecret,
                    ephSec,
                    Slice(pqMPub),
                    Slice(pqMSec),
                    Slice(pqEPub),
                    Slice(pqESec),
                    Slice(pqESec)));
            ValidatorKeys const k{c, journal};
            BEAST_EXPECT(!k.configInvalid());
            if (BEAST_EXPECT(k.keys); k.keys.has_value())
                BEAST_EXPECT(k.keys->pqSecretKey.has_value());
        }

        {
            // Hybrid token whose PQ secret does NOT correspond to the
            // manifest's PQ ephemeral -> rejected at startup, not discovered
            // by the network silently dropping our validations.
            auto const masterSecret = randomSecretKey();
            auto const [ephPub, ephSec] = randomKeyPair(KeyType::Secp256k1);
            auto [pqMPub, pqMSec] = mldsa::keypair();
            auto [pqEPub, pqESec] = mldsa::keypair();
            auto [pqWrongPub, pqWrongSec] = mldsa::keypair();

            Config c;
            c.section(SECTION_VALIDATOR_TOKEN)
                .append(makeHybridToken(
                    masterSecret,
                    ephSec,
                    Slice(pqMPub),
                    Slice(pqMSec),
                    Slice(pqEPub),
                    Slice(pqESec),
                    Slice(pqWrongSec)));
            ValidatorKeys const k{c, journal};
            BEAST_EXPECT(k.configInvalid());
        }

        {
            // A token from the production generator (xrpld
            // --generate-hybrid-validator-token, used to stand up the DevNet)
            // must load cleanly, including the PQ-secret-matches-manifest
            // self-test: the generator's ephemeral PQ keypair has to line up
            // with what ValidatorKeys verifies at startup.
            auto const out = detail::generateHybridValidatorToken(detail::HybridTokenInputs{});
            Config c;
            c.section(SECTION_VALIDATOR_TOKEN)
                .append(std::vector<std::string>{out.validatorTokenBase64});
            ValidatorKeys const k{c, journal};
            BEAST_EXPECT(!k.configInvalid());
            if (BEAST_EXPECT(k.keys); k.keys.has_value())
                BEAST_EXPECT(k.keys->pqSecretKey.has_value());
        }
    }
};  // namespace test

BEAST_DEFINE_TESTSUITE(ValidatorKeys, app, xrpl);

}  // namespace xrpl::test
