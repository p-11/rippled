#include <test/jtx/quantum_sign.h>

#include <test/jtx/JTx.h>
#include <test/jtx/utility.h>

#include <xrpl/basics/Slice.h>
#include <xrpl/basics/strHex.h>
#include <xrpl/protocol/HashPrefix.h>
#include <xrpl/protocol/PQSign.h>
#include <xrpl/protocol/STObject.h>
#include <xrpl/protocol/SecretKey.h>
#include <xrpl/protocol/Serializer.h>
#include <xrpl/protocol/detail/mldsa.h>
#include <xrpl/protocol/jss.h>

namespace xrpl::test::jtx {

PQKey
PQKey::generate()
{
    auto [pk, sk] = mldsa::keypair();
    return PQKey(std::move(pk), std::move(sk));
}

void
QuantumSign::operator()(Env&, JTx& jt) const
{
    jt.fillSig = false;

    auto const account = account_;
    auto const pqKey = pqKey_;

    jt.mainSigners.emplace_back([account, pqKey](Env&, JTx& jtx) {
        // Mirror of STTx::sign's single-sign path on the JSON side: place
        // both pubkeys on the object first so the canonical payload built by
        // addWithoutSigningFields includes sfQuantumPubKey, then sign that
        // same payload with both ECC and PQ. If STTx::sign's ordering ever
        // changes, this loop has to change with it.
        auto& sigObject = jtx.jv;

        sigObject[jss::SigningPubKey] = strHex(account.pk().slice());
        sigObject[jss::QuantumPubKey] = strHex(pqKey.publicKey());

        Serializer ss;
        ss.add32(HashPrefix::TxSign);
        parse(jtx.jv).addWithoutSigningFields(ss);

        auto const eccSig = xrpl::sign(account.pk(), account.sk(), ss.slice());
        auto const pqSig = pqSign(ss.slice(), pqKey.secretKey());

        sigObject[jss::TxnSignature] = strHex(Slice{eccSig.data(), eccSig.size()});
        sigObject[jss::QuantumSignature] = strHex(Slice{pqSig.data(), pqSig.size()});
    });
}

}  // namespace xrpl::test::jtx
