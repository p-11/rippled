#include <xrpl/basics/Buffer.h>
#include <xrpl/basics/Slice.h>
#include <xrpl/basics/base_uint.h>
#include <xrpl/beast/hash/uhash.h>
#include <xrpl/beast/unit_test/suite.h>
#include <xrpl/protocol/KeyType.h>
#include <xrpl/protocol/PQSign.h>
#include <xrpl/protocol/PublicKey.h>
#include <xrpl/protocol/Rules.h>
#include <xrpl/protocol/SField.h>
#include <xrpl/protocol/STTx.h>
#include <xrpl/protocol/SecretKey.h>
#include <xrpl/protocol/Serializer.h>
#include <xrpl/protocol/TxFormats.h>
#include <xrpl/protocol/detail/mldsa.h>

#include <string>
#include <unordered_set>
#include <utility>

namespace xrpl::test {

class HybridSign_test : public beast::unit_test::Suite
{
    Rules
    defaultRules() const
    {
        static std::unordered_set<uint256, beast::Uhash<>> const kPresets;
        return Rules{kPresets};
    }

    struct HybridTx
    {
        STTx tx;
        std::pair<PublicKey, SecretKey> eccKeys;
        Buffer pqPub;
        Buffer pqSec;
    };

    HybridTx
    makeHybridTx(KeyType keyType)
    {
        auto eccKeys = randomKeyPair(keyType);
        auto [pqPub, pqSec] = mldsa::keypair();

        STTx tx(ttACCOUNT_SET, [&](auto& obj) {
            obj.setAccountID(sfAccount, calcAccountID(eccKeys.first));
            obj.setFieldVL(sfSigningPubKey, eccKeys.first.slice());
        });
        tx.sign(eccKeys.first, eccKeys.second, {}, Slice(pqPub), Slice(pqSec));
        return HybridTx{std::move(tx), std::move(eccKeys), std::move(pqPub), std::move(pqSec)};
    }

    static std::string
    label(char const* what, KeyType keyType)
    {
        return std::string(what) + " [" + to_string(keyType) + "]";
    }

public:
    void
    testHybridRoundTrip(KeyType keyType)
    {
        testcase(label("Hybrid sign/verify round-trip", keyType));
        auto h = makeHybridTx(keyType);

        BEAST_EXPECT(h.tx.isFieldPresent(sfQuantumPubKey));
        BEAST_EXPECT(h.tx.isFieldPresent(sfQuantumSignature));
        BEAST_EXPECT(h.tx.getFieldVL(sfQuantumPubKey).size() == kPQPublicKeySize);
        BEAST_EXPECT(h.tx.getFieldVL(sfQuantumSignature).size() == kPQSignatureSize);
        BEAST_EXPECT(h.tx.checkSign(defaultRules()));
    }

    void
    testEccOnlyStillVerifies(KeyType keyType)
    {
        testcase(label("ECC-only sign/verify is unaffected", keyType));
        auto const eccKeys = randomKeyPair(keyType);
        STTx tx(ttACCOUNT_SET, [&](auto& obj) {
            obj.setAccountID(sfAccount, calcAccountID(eccKeys.first));
            obj.setFieldVL(sfSigningPubKey, eccKeys.first.slice());
        });
        tx.sign(eccKeys.first, eccKeys.second);

        BEAST_EXPECT(!tx.isFieldPresent(sfQuantumPubKey));
        BEAST_EXPECT(!tx.isFieldPresent(sfQuantumSignature));
        BEAST_EXPECT(tx.checkSign(defaultRules()));
    }

    void
    testTamperedPQSignature(KeyType keyType)
    {
        testcase(label("Tampered PQ signature is rejected", keyType));
        auto h = makeHybridTx(keyType);

        Blob sig = h.tx.getFieldVL(sfQuantumSignature);
        sig[0] ^= 0x01;
        h.tx.setFieldVL(sfQuantumSignature, sig);

        BEAST_EXPECT(!h.tx.checkSign(defaultRules()));
    }

    void
    testSwappedPQPubKeyBreaksECC(KeyType keyType)
    {
        testcase(label("Swapping PQ pubkey invalidates the ECC signature", keyType));
        auto h = makeHybridTx(keyType);

        auto const [otherPub, otherSec] = mldsa::keypair();
        h.tx.setFieldVL(sfQuantumPubKey, otherPub);

        // sfQuantumPubKey is a signing field, so the ECC signature no longer
        // matches the reconstructed payload. The PQ signature also fails
        // (signed against the original payload), but the binding property we
        // care about is that the ECC half catches the swap by itself.
        BEAST_EXPECT(!h.tx.checkSign(defaultRules()));
    }

    void
    testTamperedEccSignature(KeyType keyType)
    {
        testcase(label("Tampered ECC signature on a hybrid tx is rejected", keyType));
        auto h = makeHybridTx(keyType);

        Blob sig = h.tx.getFieldVL(sfTxnSignature);
        sig[0] ^= 0x01;
        h.tx.setFieldVL(sfTxnSignature, sig);

        BEAST_EXPECT(!h.tx.checkSign(defaultRules()));
    }

    void
    testHalfPresentPQFields(KeyType keyType)
    {
        testcase(label("Half-present PQ fields are rejected", keyType));
        {
            auto h = makeHybridTx(keyType);
            h.tx.makeFieldAbsent(sfQuantumSignature);
            BEAST_EXPECT(!h.tx.checkSign(defaultRules()));
        }
        {
            auto h = makeHybridTx(keyType);
            h.tx.makeFieldAbsent(sfQuantumPubKey);
            BEAST_EXPECT(!h.tx.checkSign(defaultRules()));
        }
    }

    void
    testWrongSizePQFields(KeyType keyType)
    {
        testcase(label("Wrong-size PQ fields are rejected", keyType));
        auto const exercise = [this](HybridTx h, SField const& field, Blob blob) {
            h.tx.setFieldVL(field, blob);
            BEAST_EXPECT(!h.tx.checkSign(defaultRules()));
        };

        exercise(makeHybridTx(keyType), sfQuantumPubKey, Blob(kPQPublicKeySize - 1, 0xCC));
        exercise(makeHybridTx(keyType), sfQuantumPubKey, Blob(kPQPublicKeySize + 1, 0xCC));
        exercise(makeHybridTx(keyType), sfQuantumSignature, Blob(kPQSignatureSize - 1, 0xCC));
        exercise(makeHybridTx(keyType), sfQuantumSignature, Blob(kPQSignatureSize + 1, 0xCC));
    }

    void
    testWireRoundTripPreservesValidity(KeyType keyType)
    {
        testcase(label("Wire round-trip preserves hybrid signature validity", keyType));
        auto h = makeHybridTx(keyType);
        Serializer s;
        h.tx.add(s);
        SerialIter sit(s.slice());
        STTx const wireCopy(sit);
        BEAST_EXPECT(wireCopy.checkSign(defaultRules()));
    }

    void
    runFor(KeyType keyType)
    {
        testHybridRoundTrip(keyType);
        testEccOnlyStillVerifies(keyType);
        testTamperedPQSignature(keyType);
        testSwappedPQPubKeyBreaksECC(keyType);
        testTamperedEccSignature(keyType);
        testHalfPresentPQFields(keyType);
        testWrongSizePQFields(keyType);
        testWireRoundTripPreservesValidity(keyType);
    }

    void
    run() override
    {
        runFor(KeyType::Secp256k1);
        runFor(KeyType::Ed25519);
    }
};

BEAST_DEFINE_TESTSUITE(HybridSign, protocol, xrpl);

}  // namespace xrpl::test
