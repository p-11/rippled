#include <xrpl/basics/Buffer.h>
#include <xrpl/basics/Slice.h>
#include <xrpl/basics/base_uint.h>
#include <xrpl/beast/hash/uhash.h>
#include <xrpl/beast/unit_test/suite.h>
#include <xrpl/protocol/AccountID.h>
#include <xrpl/protocol/KeyType.h>
#include <xrpl/protocol/PQSign.h>
#include <xrpl/protocol/PublicKey.h>
#include <xrpl/protocol/Rules.h>
#include <xrpl/protocol/SField.h>
#include <xrpl/protocol/STArray.h>
#include <xrpl/protocol/STObject.h>
#include <xrpl/protocol/STTx.h>
#include <xrpl/protocol/SecretKey.h>
#include <xrpl/protocol/Serializer.h>
#include <xrpl/protocol/Sign.h>
#include <xrpl/protocol/TxFormats.h>
#include <xrpl/protocol/detail/mldsa.h>

#include <algorithm>
#include <cstddef>
#include <optional>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

namespace xrpl::test {

class HybridMultiSign_test : public beast::unit_test::Suite
{
    Rules
    defaultRules() const
    {
        static std::unordered_set<uint256, beast::Uhash<>> const kPresets;
        return Rules{kPresets};
    }

    struct SignerEntry
    {
        std::pair<PublicKey, SecretKey> ecc;
        std::optional<Buffer> pqPub;
        std::optional<Buffer> pqSec;

        AccountID
        id() const
        {
            return calcAccountID(ecc.first);
        }
    };

    struct HybridMultiTx
    {
        STTx tx;
        std::vector<SignerEntry> entries;  // sorted by AccountID
    };

    /** Build a multi-signed ttACCOUNT_SET with one entry per `hybridFlags[i]`.
        Each `true` entry signs with both ECC and ML-DSA-44; `false` is ECC-only.
    */
    HybridMultiTx
    makeMultiSigned(KeyType keyType, std::vector<bool> const& hybridFlags)
    {
        auto const owner = randomKeyPair(keyType);

        std::vector<SignerEntry> entries;
        entries.reserve(hybridFlags.size());
        for (bool hybrid : hybridFlags)
        {
            SignerEntry e{randomKeyPair(keyType), std::nullopt, std::nullopt};
            if (hybrid)
            {
                auto [pqPub, pqSec] = mldsa::keypair();
                e.pqPub = std::move(pqPub);
                e.pqSec = std::move(pqSec);
            }
            entries.push_back(std::move(e));
        }

        // multiSignHelper enforces strictly-sorted AccountIDs.
        std::ranges::sort(
            entries, [](SignerEntry const& a, SignerEntry const& b) { return a.id() < b.id(); });

        STTx tx(ttACCOUNT_SET, [&](auto& obj) {
            obj.setAccountID(sfAccount, calcAccountID(owner.first));
            obj.setFieldVL(sfSigningPubKey, Slice{});
        });

        STArray signersArr(sfSigners, entries.size());
        for (auto const& e : entries)
        {
            STObject signer(sfSigner);
            AccountID const aid = e.id();
            signer.setAccountID(sfAccount, aid);
            signer.setFieldVL(sfSigningPubKey, e.ecc.first.slice());
            if (e.pqPub)
                signer.setFieldVL(sfQuantumPubKey, Slice(*e.pqPub));

            Slice const pqPubSlice = e.pqPub ? Slice(*e.pqPub) : Slice{};
            Serializer const ss{buildMultiSigningData(tx, aid, pqPubSlice)};
            auto const eccSig = xrpl::sign(e.ecc.first, e.ecc.second, ss.slice());
            signer.setFieldVL(sfTxnSignature, eccSig);

            if (e.pqSec)
            {
                auto const pqSig = pqSign(Slice(*e.pqSec), ss.slice());
                signer.setFieldVL(sfQuantumSignature, pqSig);
            }

            signersArr.push_back(std::move(signer));
        }
        tx.setFieldArray(sfSigners, signersArr);

        return HybridMultiTx{std::move(tx), std::move(entries)};
    }

    template <class Mutator>
    static void
    mutateSigner(STTx& tx, std::size_t idx, Mutator&& mutator)
    {
        STArray signers = tx.getFieldArray(sfSigners);
        mutator(signers[idx]);
        tx.setFieldArray(sfSigners, signers);
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
        testcase(label("Hybrid multi-sign round-trip", keyType));
        auto h = makeMultiSigned(keyType, {true, true});

        auto const& signers = h.tx.getFieldArray(sfSigners);
        BEAST_EXPECT(signers.size() == 2);
        for (auto const& s : signers)
        {
            BEAST_EXPECT(s.isFieldPresent(sfQuantumPubKey));
            BEAST_EXPECT(s.isFieldPresent(sfQuantumSignature));
            BEAST_EXPECT(s.getFieldVL(sfQuantumPubKey).size() == kPQPublicKeySize);
            BEAST_EXPECT(s.getFieldVL(sfQuantumSignature).size() == kPQSignatureSize);
        }
        BEAST_EXPECT(h.tx.checkSign(defaultRules()));
    }

    void
    testEccOnlyStillVerifies(KeyType keyType)
    {
        testcase(label("ECC-only multi-sign is unaffected", keyType));
        auto h = makeMultiSigned(keyType, {false, false});

        auto const& signers = h.tx.getFieldArray(sfSigners);
        for (auto const& s : signers)
        {
            BEAST_EXPECT(!s.isFieldPresent(sfQuantumPubKey));
            BEAST_EXPECT(!s.isFieldPresent(sfQuantumSignature));
        }
        BEAST_EXPECT(h.tx.checkSign(defaultRules()));
    }

    void
    testMixedSigners(KeyType keyType)
    {
        testcase(label("Mixed hybrid and ECC-only signers verify", keyType));
        auto h = makeMultiSigned(keyType, {true, false, true});
        BEAST_EXPECT(h.tx.checkSign(defaultRules()));
    }

    void
    testTamperedPQSignature(KeyType keyType)
    {
        testcase(label("Tampered PQ signature on one signer is rejected", keyType));
        auto h = makeMultiSigned(keyType, {true, true});

        mutateSigner(h.tx, 0, [](STObject& s) {
            Blob sig = s.getFieldVL(sfQuantumSignature);
            sig[0] ^= 0x01;
            s.setFieldVL(sfQuantumSignature, sig);
        });

        BEAST_EXPECT(!h.tx.checkSign(defaultRules()));
    }

    void
    testSwappedPQPubKeyBreaksECC(KeyType keyType)
    {
        testcase(label("Swapping PQ pubkey on a signer invalidates ECC", keyType));
        auto h = makeMultiSigned(keyType, {true, true});

        auto const [otherPub, otherSec] = mldsa::keypair();
        mutateSigner(h.tx, 0, [&](STObject& s) { s.setFieldVL(sfQuantumPubKey, Slice(otherPub)); });

        // The ECC signature commits to the PQ pubkey via the per-signer
        // canonical payload, so swapping the PQ pubkey invalidates the ECC
        // half by itself even before the PQ half is checked.
        BEAST_EXPECT(!h.tx.checkSign(defaultRules()));
    }

    void
    testTamperedEccSignature(KeyType keyType)
    {
        testcase(label("Tampered ECC signature on one signer is rejected", keyType));
        auto h = makeMultiSigned(keyType, {true, true});

        mutateSigner(h.tx, 1, [](STObject& s) {
            Blob sig = s.getFieldVL(sfTxnSignature);
            sig[0] ^= 0x01;
            s.setFieldVL(sfTxnSignature, sig);
        });

        BEAST_EXPECT(!h.tx.checkSign(defaultRules()));
    }

    void
    testHalfPresentPQFields(KeyType keyType)
    {
        testcase(label("Half-present PQ fields on a signer are rejected", keyType));
        {
            auto h = makeMultiSigned(keyType, {true, true});
            mutateSigner(h.tx, 0, [](STObject& s) { s.makeFieldAbsent(sfQuantumSignature); });
            BEAST_EXPECT(!h.tx.checkSign(defaultRules()));
        }
        {
            auto h = makeMultiSigned(keyType, {true, true});
            mutateSigner(h.tx, 0, [](STObject& s) { s.makeFieldAbsent(sfQuantumPubKey); });
            BEAST_EXPECT(!h.tx.checkSign(defaultRules()));
        }
    }

    void
    testWrongSizePQFields(KeyType keyType)
    {
        testcase(label("Wrong-size PQ fields on a signer are rejected", keyType));
        auto const exercise = [this, keyType](SField const& field, Blob blob) {
            auto h = makeMultiSigned(keyType, {true, true});
            mutateSigner(h.tx, 0, [&](STObject& s) { s.setFieldVL(field, blob); });
            BEAST_EXPECT(!h.tx.checkSign(defaultRules()));
        };

        exercise(sfQuantumPubKey, Blob(kPQPublicKeySize - 1, 0xCC));
        exercise(sfQuantumPubKey, Blob(kPQPublicKeySize + 1, 0xCC));
        exercise(sfQuantumSignature, Blob(kPQSignatureSize - 1, 0xCC));
        exercise(sfQuantumSignature, Blob(kPQSignatureSize + 1, 0xCC));
    }

    void
    testWireRoundTripPreservesValidity(KeyType keyType)
    {
        testcase(label("Wire round-trip preserves hybrid multi-sign validity", keyType));
        auto h = makeMultiSigned(keyType, {true, false, true});
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
        testMixedSigners(keyType);
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

BEAST_DEFINE_TESTSUITE(HybridMultiSign, protocol, xrpl);

}  // namespace xrpl::test
