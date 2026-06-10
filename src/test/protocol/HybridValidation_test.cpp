#include <xrpl/basics/Buffer.h>
#include <xrpl/basics/Slice.h>
#include <xrpl/basics/base_uint.h>
#include <xrpl/basics/chrono.h>
#include <xrpl/beast/unit_test/suite.h>
#include <xrpl/json/to_string.h>  // IWYU pragma: keep
#include <xrpl/protocol/KeyType.h>
#include <xrpl/protocol/PQSign.h>
#include <xrpl/protocol/PublicKey.h>
#include <xrpl/protocol/SField.h>
#include <xrpl/protocol/STValidation.h>
#include <xrpl/protocol/SecretKey.h>
#include <xrpl/protocol/Serializer.h>
#include <xrpl/protocol/UintTypes.h>
#include <xrpl/protocol/detail/mldsa.h>

#include <memory>
#include <string>
#include <utility>

namespace xrpl::test {

class HybridValidation_test : public beast::unit_test::Suite
{
    struct HybridKeys
    {
        PublicKey ecPub;
        SecretKey ecSec;
        Buffer pqPub;
        Buffer pqSec;
        NodeID nodeID;
    };

    static HybridKeys
    makeHybridKeys(KeyType keyType)
    {
        auto const [ecPub, ecSec] = randomKeyPair(keyType);
        auto [pqPub, pqSec] = mldsa::keypair();
        auto const nodeID = calcNodeID(ecPub);
        return HybridKeys{ecPub, ecSec, std::move(pqPub), std::move(pqSec), nodeID};
    }

    static std::shared_ptr<STValidation>
    makeValidation(HybridKeys const& k, std::uint32_t ledgerSeq = 1)
    {
        return std::make_shared<STValidation>(
            NetClock::time_point{NetClock::duration{1000}},
            k.ecPub,
            k.ecSec,
            k.nodeID,
            [ledgerSeq](STObject& obj) {
                obj.setFieldU32(sfLedgerSequence, ledgerSeq);
                obj.setFieldH256(sfLedgerHash, uint256{42});
            },
            Slice(k.pqPub),
            Slice(k.pqSec));
    }

    static std::string
    label(char const* what, KeyType keyType)
    {
        return std::string(what) + " [" + to_string(keyType) + "]";
    }

public:
    void
    testRoundTrip(KeyType keyType)
    {
        testcase(label("Hybrid sign / verify round-trip", keyType));
        auto const k = makeHybridKeys(keyType);
        auto const val = makeValidation(k);

        BEAST_EXPECT(val->isFieldPresent(sfQuantumPubKey));
        BEAST_EXPECT(val->isFieldPresent(sfQuantumSignature));
        BEAST_EXPECT(val->getFieldVL(sfQuantumPubKey).size() == kPQPublicKeySize);
        BEAST_EXPECT(val->getFieldVL(sfQuantumSignature).size() == kPQSignatureSize);
        BEAST_EXPECT(val->isValid());
    }

    void
    testWireRoundTrip(KeyType keyType)
    {
        testcase(label("Serialize and reparse preserves hybrid validity", keyType));
        auto const k = makeHybridKeys(keyType);
        auto const val = makeValidation(k);

        Serializer s;
        val->add(s);

        SerialIter sit(s.slice());
        auto const reparsed = std::make_shared<STValidation>(
            sit, [](PublicKey const& pk) { return calcNodeID(pk); }, true);

        BEAST_EXPECT(reparsed);
        BEAST_EXPECT(reparsed->isFieldPresent(sfQuantumPubKey));
        BEAST_EXPECT(reparsed->isValid());
    }

    void
    testTamperedPqSignature(KeyType keyType)
    {
        testcase(label("Flipping a PQ signature byte fails verify", keyType));
        auto const k = makeHybridKeys(keyType);
        auto const val = makeValidation(k);

        Serializer s;
        val->add(s);
        auto bytes = s.peekData();

        // Find sfQuantumSignature on the wire by scanning for its raw
        // size header. Cheaper proxy: locate the 2420-byte signature
        // suffix and flip a byte deep inside it.
        bytes[bytes.size() - 100] ^= 0x01;

        try
        {
            SerialIter sit(makeSlice(bytes));
            auto const reparsed = std::make_shared<STValidation>(
                sit, [](PublicKey const& pk) { return calcNodeID(pk); }, true);
            fail("Tampered hybrid validation should not verify");
        }
        catch (std::exception const&)
        {
            pass();
        }
    }

    void
    testStrippingPqPubKeyBreaksEcc(KeyType keyType)
    {
        testcase(label("Stripping the PQ pubkey breaks the ECC signature", keyType));
        auto const k = makeHybridKeys(keyType);
        auto val = makeValidation(k);
        // Wipe the PQ pubkey in place to simulate an attacker stripping
        // hybrid material; the ECC sig commits to it via the signing
        // fields template, so verify must fail.
        Buffer zeroPub(kPQPublicKeySize);
        val->setFieldVL(sfQuantumPubKey, Slice(zeroPub));
        val->setFieldVL(sfQuantumSignature, Slice(Buffer(kPQSignatureSize)));

        // Force re-evaluation by reparsing the serialized form so isValid()
        // recomputes (its cached `valid_` was set during construction).
        Serializer s;
        val->add(s);
        try
        {
            SerialIter sit(s.slice());
            auto const reparsed = std::make_shared<STValidation>(
                sit, [](PublicKey const& pk) { return calcNodeID(pk); }, true);
            fail("Validation with stripped PQ pubkey should not verify");
        }
        catch (std::exception const&)
        {
            pass();
        }
    }

    void
    testDanglingPqSignature(KeyType keyType)
    {
        testcase(label("PQ signature without PQ pubkey is rejected", keyType));
        auto const k = makeHybridKeys(keyType);

        // ECC-only validation (empty PQ slices), then bolt on a quantum
        // signature with no pubkey. sfQuantumSignature is kNotSigning, so
        // the ECC signature stays valid; the mismatched-fields guard must
        // reject the validation anyway (tx-path parity).
        auto val = std::make_shared<STValidation>(
            NetClock::time_point{NetClock::duration{1000}},
            k.ecPub,
            k.ecSec,
            k.nodeID,
            [](STObject& obj) {
                obj.setFieldU32(sfLedgerSequence, 1);
                obj.setFieldH256(sfLedgerHash, uint256{42});
            },
            Slice{},
            Slice{});
        BEAST_EXPECT(val->isValid());

        Buffer junk(kPQSignatureSize);
        val->setFieldVL(sfQuantumSignature, Slice(junk));

        // Reparse so isValid() recomputes (valid_ is cached at signing).
        Serializer s;
        val->add(s);
        try
        {
            SerialIter sit(s.slice());
            auto const reparsed = std::make_shared<STValidation>(
                sit, [](PublicKey const& pk) { return calcNodeID(pk); }, true);
            fail("Validation with dangling PQ signature should not verify");
        }
        catch (std::exception const&)
        {
            pass();
        }
    }

    void
    runFor(KeyType keyType)
    {
        testRoundTrip(keyType);
        testWireRoundTrip(keyType);
        testTamperedPqSignature(keyType);
        testStrippingPqPubKeyBreaksEcc(keyType);
        testDanglingPqSignature(keyType);
    }

    void
    run() override
    {
        // signDigest is hardcoded to secp256k1, so the validator-side
        // signing path cannot produce ed25519 ECC validation signatures
        // even though RD-445 lifted the envelope-level keytype check.
        // Ed25519 hybrid validations are a follow-up once signDigest /
        // verifyDigest gain ed25519 support.
        runFor(KeyType::Secp256k1);
    }
};

BEAST_DEFINE_TESTSUITE(HybridValidation, protocol, xrpl);

}  // namespace xrpl::test
