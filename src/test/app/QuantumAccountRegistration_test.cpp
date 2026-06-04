
#include <test/jtx/Account.h>
#include <test/jtx/Env.h>
#include <test/jtx/amount.h>
#include <test/jtx/delegate.h>
#include <test/jtx/fee.h>
#include <test/jtx/multisign.h>
#include <test/jtx/noop.h>
#include <test/jtx/pay.h>
#include <test/jtx/quantum_msig.h>
#include <test/jtx/quantum_sign.h>
#include <test/jtx/sig.h>
#include <test/jtx/ter.h>

#include <xrpl/basics/Blob.h>
#include <xrpl/basics/Slice.h>
#include <xrpl/basics/strHex.h>
#include <xrpl/beast/unit_test/suite.h>
#include <xrpl/json/json_value.h>
#include <xrpl/protocol/Feature.h>
#include <xrpl/protocol/KeyType.h>
#include <xrpl/protocol/SField.h>
#include <xrpl/protocol/TER.h>
#include <xrpl/protocol/TxFlags.h>
#include <xrpl/protocol/jss.h>

#include <string>

namespace xrpl::test {

class QuantumAccountRegistration_test : public beast::unit_test::Suite
{
    static FeatureBitset
    withQuantum()
    {
        return jtx::testableAmendments() | featureQuantum;
    }

    static json::Value
    registerQuantum(jtx::Account const& a, jtx::PQKey const& key)
    {
        json::Value jv;
        jv[jss::Account] = a.human();
        jv[jss::TransactionType] = jss::AccountSet;
        jv[jss::SetFlag] = asfQuantum;
        jv[jss::QuantumPubKey] = strHex(key.publicKey());
        return jv;
    }

    static json::Value
    clearQuantum(jtx::Account const& a)
    {
        json::Value jv;
        jv[jss::Account] = a.human();
        jv[jss::TransactionType] = jss::AccountSet;
        jv[jss::ClearFlag] = asfQuantum;
        return jv;
    }

    static Blob
    pkBytes(jtx::PQKey const& key)
    {
        auto const s = key.publicKey();
        return Blob(s.data(), s.data() + s.size());
    }

    static std::string
    label(char const* what, KeyType keyType)
    {
        return std::string(what) + " [" + to_string(keyType) + "]";
    }

public:
    void
    testRegisterAndClear(KeyType keyType)
    {
        testcase(label("Register and clear quantum public key", keyType));
        using namespace jtx;

        Env env{*this, withQuantum()};
        Account const alice{"alice", keyType};
        env.fund(XRP(10000), alice);
        env.close();

        auto pq = PQKey::generate();
        env(registerQuantum(alice, pq), quantum_sign(alice, pq));
        env.close();
        BEAST_EXPECT(env.le(alice)->isFieldPresent(sfQuantumPubKey));
        BEAST_EXPECT(env.le(alice)->getFieldVL(sfQuantumPubKey) == pkBytes(pq));

        // Idempotent re-set with same value.
        env(registerQuantum(alice, pq), quantum_sign(alice, pq));
        env.close();
        BEAST_EXPECT(env.le(alice)->getFieldVL(sfQuantumPubKey) == pkBytes(pq));

        // Clearing requires the tx itself to be hybrid-signed (source opt-in).
        env(clearQuantum(alice), quantum_sign(alice, pq));
        env.close();
        BEAST_EXPECT(!env.le(alice)->isFieldPresent(sfQuantumPubKey));
    }

    void
    testSingleSignAuthentication(KeyType keyType)
    {
        testcase(label("Single-sign authentication", keyType));
        using namespace jtx;

        Env env{*this, withQuantum()};
        Account const alice{"alice", keyType};
        env.fund(XRP(10000), alice);
        env.close();

        // Before opt-in, ECC-only is fine.
        env(noop(alice));
        env.close();

        auto pq = PQKey::generate();
        env(registerQuantum(alice, pq), quantum_sign(alice, pq));
        env.close();

        // After opt-in, ECC-only must be rejected at authentication.
        env(noop(alice), Ter(tefBAD_AUTH));
        env.close();

        // Wrong PQ pubkey must be rejected.
        auto pqOther = PQKey::generate();
        env(noop(alice), quantum_sign(alice, pqOther), Ter(tefBAD_AUTH));
        env.close();

        // Matching PQ pubkey succeeds.
        env(noop(alice), quantum_sign(alice, pq));
        env.close();
    }

    void
    testMultiSignFundedSigners(KeyType keyType)
    {
        testcase(label("Multi-sign with funded hybrid signers", keyType));
        using namespace jtx;

        Env env{*this, withQuantum()};
        Account const alice{"alice", keyType};
        Account const bob{"bob", keyType};
        Account const carol{"carol", keyType};
        env.fund(XRP(10000), alice, bob, carol);
        env.close();

        auto pqAlice = PQKey::generate();
        auto pqBob = PQKey::generate();
        auto pqCarol = PQKey::generate();

        // Register PQ key on each signer's own AccountRoot.
        env(registerQuantum(bob, pqBob), quantum_sign(bob, pqBob));
        env(registerQuantum(carol, pqCarol), quantum_sign(carol, pqCarol));
        env.close();

        // Set the SignerList on alice with bob and carol (no SignerEntry PQ).
        env(signers(alice, 2, {{bob, 1}, {carol, 1}}));
        env.close();

        // Opt alice in.
        env(registerQuantum(alice, pqAlice), quantum_sign(alice, pqAlice));
        env.close();

        auto const baseFee = env.current()->fees().base;

        // Hybrid multi-sign succeeds: each signer's per-tx PQ pubkey
        // equals their own AccountRoot's registered value.
        env(noop(alice),
            quantum_msig({QuantumSigner{Reg{bob}, pqBob}, QuantumSigner{Reg{carol}, pqCarol}}),
            Fee(3 * baseFee));
        env.close();

        // Source opted in, but one signer is ECC-only: rejected by
        // propagation.
        env(noop(alice),
            quantum_msig({QuantumSigner{Reg{bob}, pqBob}, QuantumSigner{Reg{carol}}}),
            Fee(3 * baseFee),
            Ter(tefBAD_AUTH));
        env.close();

        // Wrong PQ pubkey on a signer: rejected.
        auto pqOther = PQKey::generate();
        env(noop(alice),
            quantum_msig({QuantumSigner{Reg{bob}, pqBob}, QuantumSigner{Reg{carol}, pqOther}}),
            Fee(3 * baseFee),
            Ter(tefBAD_AUTH));
        env.close();
    }

    void
    testMultiSignPhantomSigners(KeyType keyType)
    {
        testcase(label("Multi-sign with phantom signers via SignerEntry PQ", keyType));
        using namespace jtx;

        Env env{*this, withQuantum()};
        Account const alice{"alice", keyType};
        Account const bogie{"bogie", KeyType::Secp256k1};
        Account const demon{"demon", KeyType::Ed25519};
        env.fund(XRP(10000), alice);
        env.close();

        auto pqAlice = PQKey::generate();
        auto pqBogie = PQKey::generate();
        auto pqDemon = PQKey::generate();

        // Register PQ pubkeys on the SignerEntries first; otherwise the
        // anti-lockout invariant prevents opting in below.
        env(signers(
            alice,
            1,
            {Signer{bogie, 1, std::nullopt, pkBytes(pqBogie)},
             Signer{demon, 1, std::nullopt, pkBytes(pqDemon)}}));
        env.close();

        // Now opt alice in.
        env(registerQuantum(alice, pqAlice), quantum_sign(alice, pqAlice));
        env.close();

        auto const baseFee = env.current()->fees().base;

        // Phantom hybrid multi-sign succeeds against SignerEntry-registered
        // PQ pubkeys.
        env(noop(alice),
            quantum_msig({QuantumSigner{Reg{bogie}, pqBogie}, QuantumSigner{Reg{demon}, pqDemon}}),
            Fee(3 * baseFee));
        env.close();

        // Wrong PQ pubkey on a phantom: rejected.
        auto pqWrong = PQKey::generate();
        env(noop(alice),
            quantum_msig({QuantumSigner{Reg{bogie}, pqWrong}, QuantumSigner{Reg{demon}, pqDemon}}),
            Fee(3 * baseFee),
            Ter(tefBAD_AUTH));
        env.close();
    }

    void
    testMultiSignDualSourceConsistency(KeyType keyType)
    {
        testcase(label("Multi-sign dual-source consistency", keyType));
        using namespace jtx;

        Env env{*this, withQuantum()};
        Account const alice{"alice", keyType};
        Account const bob{"bob", keyType};
        env.fund(XRP(10000), alice, bob);
        env.close();

        auto pqAlice = PQKey::generate();
        auto pqBob = PQKey::generate();
        auto pqSnapshotMismatch = PQKey::generate();

        // Register pqBob on bob's own AccountRoot.
        env(registerQuantum(bob, pqBob), quantum_sign(bob, pqBob));
        env.close();

        // Set up alice's SignerList. The SignerEntry for bob carries a
        // different PQ pubkey than bob's own AccountRoot: this is the
        // equivocation case the dual-source check rejects.
        env(signers(alice, 1, {Signer{bob, 1, std::nullopt, pkBytes(pqSnapshotMismatch)}}));
        env.close();

        env(registerQuantum(alice, pqAlice), quantum_sign(alice, pqAlice));
        env.close();

        auto const baseFee = env.current()->fees().base;

        // Even though pqBob matches bob's own AccountRoot, the SignerEntry
        // snapshot disagrees: the per-signer authentication rejects.
        env(noop(alice),
            quantum_msig({QuantumSigner{Reg{bob}, pqBob}}),
            Fee(2 * baseFee),
            Ter(tefBAD_AUTH));
        env.close();
    }

    void
    testAntiLockoutInvariant(KeyType keyType)
    {
        testcase(label("Anti-lockout invariant on opt-in", keyType));
        using namespace jtx;

        Env env{*this, withQuantum()};
        Account const alice{"alice", keyType};
        Account const bogie{"bogie", KeyType::Secp256k1};
        env.fund(XRP(10000), alice);
        env.close();

        auto pqAlice = PQKey::generate();
        auto pqBogie = PQKey::generate();

        // SignerList exists but its single entry has no PQ pubkey: opt-in
        // would leave alice with no way to authenticate her own signer.
        env(signers(alice, 1, {{bogie, 1}}));
        env.close();
        env(registerQuantum(alice, pqAlice),
            quantum_sign(alice, pqAlice),
            Ter(tecNO_ALTERNATIVE_KEY));
        env.close();

        // Replace the SignerList with a PQ-registered entry: opt-in
        // succeeds.
        env(signers(alice, 1, {Signer{bogie, 1, std::nullopt, pkBytes(pqBogie)}}));
        env.close();
        env(registerQuantum(alice, pqAlice), quantum_sign(alice, pqAlice));
        env.close();
        BEAST_EXPECT(env.le(alice)->isFieldPresent(sfQuantumPubKey));
    }

    void
    testDelegateOptInPropagation(KeyType keyType)
    {
        testcase(label("Delegate opt-in propagation", keyType));
        using namespace jtx;

        Env env{*this, withQuantum()};
        Account const gw{"gw", keyType};
        Account const alice{"alice", keyType};
        Account const bob{"bob", keyType};
        env.fund(XRP(10000), gw, alice, bob);
        env.close();

        // gw grants alice the Payment permission.
        env(delegate::set(gw, alice, {"Payment"}));
        env.close();

        // Source opt-in.
        auto pqGw = PQKey::generate();
        env(registerQuantum(gw, pqGw), quantum_sign(gw, pqGw));
        env.close();

        // alice (delegate) is not opted in: ECC-only tx as gw's delegate
        // must be rejected even though alice's own AccountRoot is fine.
        env(pay(gw, bob, XRP(1)), delegate::As(alice), Ter(tefBAD_AUTH));
        env.close();

        // alice opts in: now the same delegate path succeeds.
        auto pqAlice = PQKey::generate();
        env(registerQuantum(alice, pqAlice), quantum_sign(alice, pqAlice));
        env.close();

        env(pay(gw, bob, XRP(1)), delegate::As(alice), quantum_sign(alice, pqAlice));
        env.close();
    }

    void
    testAmendmentGating()
    {
        testcase("Amendment gating");
        using namespace jtx;

        // featureQuantum disabled: AccountSet with asfQuantum rejected
        // at preflight; SignerListSet carrying SignerEntry PQ rejected
        // similarly. We exercise the ClearFlag path so the tx itself
        // carries no quantum signing fields and the libxrpl crypto
        // verify does not short-circuit the amendment gate.
        Env env{*this, testableAmendments() - featureQuantum};
        Account const alice{"alice", KeyType::Ed25519};
        Account const bob{"bob", KeyType::Ed25519};
        env.fund(XRP(10000), alice, bob);
        env.close();

        auto pq = PQKey::generate();
        env(clearQuantum(alice), Ter(temDISABLED));
        env(signers(alice, 1, {Signer{bob, 1, std::nullopt, pkBytes(pq)}}), Ter(temDISABLED));
        env.close();
    }

    void
    runFor(KeyType keyType)
    {
        testRegisterAndClear(keyType);
        testSingleSignAuthentication(keyType);
        testMultiSignFundedSigners(keyType);
        testMultiSignPhantomSigners(keyType);
        testMultiSignDualSourceConsistency(keyType);
        testAntiLockoutInvariant(keyType);
        testDelegateOptInPropagation(keyType);
    }

    void
    run() override
    {
        runFor(KeyType::Secp256k1);
        runFor(KeyType::Ed25519);
        testAmendmentGating();
    }
};

BEAST_DEFINE_TESTSUITE(QuantumAccountRegistration, app, xrpl);

}  // namespace xrpl::test
