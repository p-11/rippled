#include <xrpld/app/main/HybridValidatorTokenGen.h>

#include <xrpl/basics/Buffer.h>
#include <xrpl/basics/Slice.h>
#include <xrpl/basics/base64.h>
#include <xrpl/basics/strHex.h>
#include <xrpl/json/json_value.h>
#include <xrpl/json/json_writer.h>
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
#include <xrpl/protocol/detail/mldsa.h>

#include <openssl/rand.h>

#include <stdexcept>
#include <string>
#include <utility>

namespace xrpl::detail {

namespace {

Buffer
randomPqSeed()
{
    Buffer seed(kPQSeedSize);
    if (RAND_bytes(reinterpret_cast<unsigned char*>(seed.data()), kPQSeedSize) != 1)
        throw std::runtime_error("PQ seed generation: RAND_bytes failed");
    return seed;
}

}  // namespace

HybridTokenOutput
generateHybridValidatorToken(HybridTokenInputs const& in)
{
    // Resolve master ECC seed
    Seed const masterEccSeed = in.masterEccSeed ? *in.masterEccSeed : randomSeed();
    auto const [masterEccPub, masterEccSec] = generateKeyPair(KeyType::Ed25519, masterEccSeed);

    // Resolve master PQ seed
    Buffer masterPqSeed;
    if (in.masterPqSeed)
    {
        if (in.masterPqSeed->size() != kPQSeedSize)
            throw std::runtime_error("master PQ seed must be 32 bytes");
        masterPqSeed = Buffer(in.masterPqSeed->data(), in.masterPqSeed->size());
    }
    else
    {
        masterPqSeed = randomPqSeed();
    }
    auto [masterPqPub, masterPqSec] = pqKeypair(Slice(masterPqSeed));

    // Ephemeral keys: always fresh per call
    Seed const ephemeralEccSeed = randomSeed();
    auto const [ephemeralEccPub, ephemeralEccSec] =
        generateKeyPair(KeyType::Secp256k1, ephemeralEccSeed);
    Buffer const ephemeralPqSeed = randomPqSeed();
    auto [ephemeralPqPub, ephemeralPqSec] = pqKeypair(Slice(ephemeralPqSeed));

    // Build the manifest STObject.
    STObject st(sfGeneric);
    st[sfSequence] = in.sequence;
    st[sfPublicKey] = masterEccPub;
    st[sfSigningPubKey] = ephemeralEccPub;
    if (!in.domain.empty())
        st.setFieldVL(sfDomain, makeSlice(in.domain));
    st.setFieldVL(sfQuantumMasterPublicKey, Slice(masterPqPub));
    st.setFieldVL(sfQuantumPubKey, Slice(ephemeralPqPub));

    // Sign four ways. All signing fields are set; signature fields are
    // NotSigning so they don't perturb each other's payloads.
    sign(st, HashPrefix::Manifest, KeyType::Secp256k1, ephemeralEccSec);
    sign(st, HashPrefix::Manifest, KeyType::Ed25519, masterEccSec, sfMasterSignature);
    pqSign(st, HashPrefix::Manifest, Slice(ephemeralPqSec), sfQuantumSignature);
    pqSign(st, HashPrefix::Manifest, Slice(masterPqSec), sfQuantumMasterSignature);

    // Serialize the manifest, base64-encode it, then wrap into the
    // validator-token JSON.
    Serializer s;
    st.add(s);
    std::string const manifestB64 =
        base64Encode(reinterpret_cast<std::uint8_t const*>(s.data()), s.size());

    json::Value token(json::ValueType::Object);
    token["manifest"] = manifestB64;
    token["validation_secret_key"] = strHex(ephemeralEccSec);
    token["pq_validation_secret_key"] = strHex(Slice(ephemeralPqSec.data(), ephemeralPqSec.size()));

    std::string const tokenJson = json::FastWriter().write(token);
    std::string const tokenB64 = base64Encode(tokenJson);

    HybridTokenOutput out{
        .masterEccSeed = masterEccSeed,
        .masterPqSeed = std::move(masterPqSeed),
        .masterEccPubKey = masterEccPub,
        .masterPqPubKey = std::move(masterPqPub),
        .ephemeralEccPubKey = ephemeralEccPub,
        .ephemeralPqPubKey = std::move(ephemeralPqPub),
        .validatorTokenBase64 = std::move(tokenB64),
    };
    return out;
}

}  // namespace xrpl::detail
