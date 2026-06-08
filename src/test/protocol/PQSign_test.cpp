#include <xrpl/basics/Buffer.h>
#include <xrpl/basics/Slice.h>
#include <xrpl/beast/unit_test/suite.h>
#include <xrpl/protocol/HashPrefix.h>
#include <xrpl/protocol/PQSign.h>
#include <xrpl/protocol/SField.h>
#include <xrpl/protocol/STObject.h>
#include <xrpl/protocol/detail/mldsa.h>

namespace xrpl::test {

class PQSign_test : public beast::unit_test::Suite
{
    static STObject
    makeSample(std::uint32_t seq)
    {
        STObject st(sfGeneric);
        st.setFieldU32(sfSequence, seq);
        st.setFieldVL(sfQuantumPubKey, Buffer(32));  // placeholder signing field
        return st;
    }

public:
    void
    testRoundTrip()
    {
        testcase("Round-trip pqSign/pqVerify over STObject");

        auto [pqPub, pqSec] = mldsa::keypair();
        auto st = makeSample(42);

        BEAST_EXPECT(!st.isFieldPresent(sfQuantumSignature));
        pqSign(st, HashPrefix::Manifest, Slice(pqSec));
        BEAST_EXPECT(st.isFieldPresent(sfQuantumSignature));
        BEAST_EXPECT(st.getFieldVL(sfQuantumSignature).size() == kPQSignatureSize);

        BEAST_EXPECT(pqVerify(st, HashPrefix::Manifest, Slice(pqPub)));
    }

    void
    testVerifyFailsWhenSignatureFieldAbsent()
    {
        testcase("pqVerify returns false when sig field absent");

        auto [pqPub, _] = mldsa::keypair();
        auto const st = makeSample(7);

        BEAST_EXPECT(!pqVerify(st, HashPrefix::Manifest, Slice(pqPub)));
    }

    void
    testVerifyFailsOnTamperedSignature()
    {
        testcase("pqVerify rejects flipped signature bit");

        auto [pqPub, pqSec] = mldsa::keypair();
        auto st = makeSample(9);
        pqSign(st, HashPrefix::Manifest, Slice(pqSec));

        auto sig = st.getFieldVL(sfQuantumSignature);
        sig[sig.size() / 2] ^= 0x01;
        st.setFieldVL(sfQuantumSignature, sig);

        BEAST_EXPECT(!pqVerify(st, HashPrefix::Manifest, Slice(pqPub)));
    }

    void
    testVerifyFailsOnTamperedBody()
    {
        testcase("pqVerify rejects edited signing fields");

        auto [pqPub, pqSec] = mldsa::keypair();
        auto st = makeSample(11);
        pqSign(st, HashPrefix::Manifest, Slice(pqSec));

        st.setFieldU32(sfSequence, 12);
        BEAST_EXPECT(!pqVerify(st, HashPrefix::Manifest, Slice(pqPub)));
    }

    void
    testWrongHashPrefixFails()
    {
        testcase("Signature is bound to the HashPrefix");

        auto [pqPub, pqSec] = mldsa::keypair();
        auto st = makeSample(3);
        pqSign(st, HashPrefix::Manifest, Slice(pqSec));

        BEAST_EXPECT(pqVerify(st, HashPrefix::Manifest, Slice(pqPub)));
        BEAST_EXPECT(!pqVerify(st, HashPrefix::Validation, Slice(pqPub)));
    }

    void
    testCustomSignatureField()
    {
        testcase("Custom sigField stores and verifies independently");

        auto [pqPub, pqSec] = mldsa::keypair();
        auto st = makeSample(5);

        pqSign(st, HashPrefix::Validation, Slice(pqSec), sfQuantumSignature);
        BEAST_EXPECT(pqVerify(st, HashPrefix::Validation, Slice(pqPub), sfQuantumSignature));
    }

    static Buffer
    makeSeed(std::uint8_t base)
    {
        Buffer seed(kPQSeedSize);
        auto* p = seed.data();
        for (std::size_t i = 0; i < kPQSeedSize; ++i)
            p[i] = static_cast<std::uint8_t>(base ^ i);
        return seed;
    }

    void
    testKeypairFromSeedDeterministic()
    {
        testcase("pqKeypair(seed) is deterministic");

        auto const seed = makeSeed(0x01);

        auto const [pubA, secA] = pqKeypair(Slice(seed));
        auto const [pubB, secB] = pqKeypair(Slice(seed));

        BEAST_EXPECT(pubA.size() == kPQPublicKeySize);
        BEAST_EXPECT(secA.size() == kPQSecretKeySize);
        BEAST_EXPECT(Slice(pubA) == Slice(pubB));
        BEAST_EXPECT(Slice(secA) == Slice(secB));
    }

    void
    testKeypairFromSeedSignsAndVerifies()
    {
        testcase("Seed-derived keypair round-trips through pqSign/pqVerify");

        auto const seed = makeSeed(0xA0);
        auto const [pub, sec] = pqKeypair(Slice(seed));
        auto st = makeSample(17);
        pqSign(st, HashPrefix::Manifest, Slice(sec));
        BEAST_EXPECT(pqVerify(st, HashPrefix::Manifest, Slice(pub)));
    }

    void
    run() override
    {
        testRoundTrip();
        testVerifyFailsWhenSignatureFieldAbsent();
        testVerifyFailsOnTamperedSignature();
        testVerifyFailsOnTamperedBody();
        testWrongHashPrefixFails();
        testCustomSignatureField();
        testKeypairFromSeedDeterministic();
        testKeypairFromSeedSignsAndVerifies();
    }
};

BEAST_DEFINE_TESTSUITE(PQSign, protocol, xrpl);

}  // namespace xrpl::test
