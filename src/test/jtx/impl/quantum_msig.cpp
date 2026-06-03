#include <test/jtx/quantum_msig.h>

#include <test/jtx/JTx.h>
#include <test/jtx/utility.h>

#include <xrpl/basics/Slice.h>
#include <xrpl/basics/contract.h>
#include <xrpl/basics/strHex.h>
#include <xrpl/json/json_value.h>
#include <xrpl/json/to_string.h>
#include <xrpl/protocol/PQSign.h>
#include <xrpl/protocol/PublicKey.h>
#include <xrpl/protocol/SField.h>
#include <xrpl/protocol/STObject.h>
#include <xrpl/protocol/SecretKey.h>
#include <xrpl/protocol/Serializer.h>
#include <xrpl/protocol/Sign.h>
#include <xrpl/protocol/jss.h>

#include <algorithm>
#include <cstddef>

namespace xrpl::test::jtx {

QuantumMsig::QuantumMsig(std::vector<QuantumSigner> s) : signers(std::move(s))
{
    std::ranges::sort(signers, [](QuantumSigner const& a, QuantumSigner const& b) {
        return a.reg.acct < b.reg.acct;
    });
}

void
QuantumMsig::operator()(Env& env, JTx& jt) const
{
    auto const mySigners = signers;
    auto callback = [mySigners, &env](Env&, JTx& jtx) {
        // Top-level multi-sign always presents an empty SigningPubKey so
        // checkSign routes to the multi-sign path.
        jtx.jv[sfSigningPubKey] = "";

        std::optional<STObject> st;
        try
        {
            st = parse(jtx.jv);
        }
        catch (ParseError const&)
        {
            env.test.log << pretty(jtx.jv) << std::endl;
            rethrow();
        }

        // Mirror of multiSignHelper's per-signer payload construction on the
        // JSON side: the PQ pubkey is bound into the canonical payload via
        // buildMultiSigningData's pqPublicKey overload, then ECC and PQ both
        // sign that same payload. If multiSignHelper's ordering ever changes,
        // this lambda has to change with it.
        auto& js = jtx.jv[sfSigners];
        for (std::size_t i = 0; i < mySigners.size(); ++i)
        {
            auto const& e = mySigners[i];
            auto& jo = js[i][sfSigner.getJsonName()];
            jo[jss::Account] = e.reg.acct.human();
            jo[jss::SigningPubKey] = strHex(e.reg.sig.pk().slice());
            if (e.pqKey)
                jo[jss::QuantumPubKey] = strHex(e.pqKey->publicKey());

            Slice const pqPub = e.pqKey ? e.pqKey->publicKey() : Slice{};
            Serializer const ss{buildMultiSigningData(*st, e.reg.acct.id(), pqPub)};

            auto const sig =
                xrpl::sign(*publicKeyType(e.reg.sig.pk().slice()), e.reg.sig.sk(), ss.slice());
            jo[sfTxnSignature.getJsonName()] = strHex(Slice{sig.data(), sig.size()});

            if (e.pqKey)
            {
                auto const pqSig = pqSign(e.pqKey->secretKey(), ss.slice());
                jo[jss::QuantumSignature] = strHex(Slice{pqSig.data(), pqSig.size()});
            }
        }
    };
    jt.mainSigners.emplace_back(callback);
}

}  // namespace xrpl::test::jtx
