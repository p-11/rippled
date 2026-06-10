#include <hybrid_sign.h>

#include <xrpl/basics/Slice.h>
#include <xrpl/basics/strHex.h>
#include <xrpl/protocol/HashPrefix.h>
#include <xrpl/protocol/SField.h>
#include <xrpl/protocol/STParsedJSON.h>
#include <xrpl/protocol/STTx.h>
#include <xrpl/protocol/Serializer.h>

#include <stdexcept>
#include <utility>

namespace pqwallet::sign {

HybridSignResult
hybridSignFromJson(
    std::string const& what,
    json::Value const& txJson,
    custody::EccCustodyMock const& eccCustody,
    custody::PqCustodyMock const& pqCustody)
{
    xrpl::STParsedJSONObject parsed("tx_json", txJson);
    if (!parsed.object)
        throw std::runtime_error(
            what + ": tx_json failed to parse: " + parsed.error.toStyledString());

    xrpl::STTx stTx(std::move(*parsed.object));

    // Attach the PQ pubkey before building the payload so both signatures
    // commit to it; the server reconstructs the same bytes when verifying
    // and rejects the pair otherwise (tefBAD_AUTH).
    stTx.setFieldVL(xrpl::sfQuantumPubKey, pqCustody.publicKey());

    // Canonical signing payload, identical to what STTx::sign produces:
    // HashPrefix::TxSign || serialize(tx without signature fields).
    xrpl::Serializer payload;
    payload.add32(static_cast<std::uint32_t>(xrpl::HashPrefix::TxSign));
    stTx.addWithoutSigningFields(payload);
    auto const payloadSlice = xrpl::makeSlice(payload.peekData());

    auto eccSig = eccCustody.signWithECC(payloadSlice);
    auto pqSig = pqCustody.signWithPq(payloadSlice);

    stTx.setFieldVL(xrpl::sfTxnSignature, eccSig);
    stTx.setFieldVL(xrpl::sfQuantumSignature, pqSig);

    xrpl::Serializer txSerial;
    stTx.add(txSerial);

    return HybridSignResult{
        xrpl::strHex(txSerial.peekData()),
        to_string(stTx.getHash(xrpl::HashPrefix::TransactionId)),
        payload.peekData(),
        std::move(eccSig),
        std::move(pqSig)};
}

}  // namespace pqwallet::sign
