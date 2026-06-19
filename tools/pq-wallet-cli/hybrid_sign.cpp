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

namespace {

// Parse tx_json into an STTx, failing with a `what`-prefixed message.
xrpl::STTx
parseTx(std::string const& what, json::Value const& txJson)
{
    xrpl::STParsedJSONObject parsed("tx_json", txJson);
    if (!parsed.object)
        throw std::runtime_error(
            what + ": tx_json failed to parse: " + parsed.error.toStyledString());
    return xrpl::STTx(std::move(*parsed.object));
}

// Canonical signing payload, identical to what STTx::sign produces:
// HashPrefix::TxSign || serialize(tx without signature fields). Both the
// ECC and the PQ signatures commit to exactly these bytes.
xrpl::Serializer
signingPayload(xrpl::STTx const& stTx)
{
    xrpl::Serializer payload;
    payload.add32(static_cast<std::uint32_t>(xrpl::HashPrefix::TxSign));
    stTx.addWithoutSigningFields(payload);
    return payload;
}

}  // namespace

HybridSignResult
hybridSignFromJson(
    std::string const& what,
    json::Value const& txJson,
    custody::EccCustodyMock const& eccCustody,
    custody::PqCustodyMock const& pqCustody)
{
    xrpl::STTx stTx = parseTx(what, txJson);

    // Attach the PQ pubkey before building the payload so both signatures
    // commit to it; the server reconstructs the same bytes when verifying
    // and rejects the pair otherwise (tefBAD_AUTH).
    stTx.setFieldVL(xrpl::sfQuantumPubKey, pqCustody.publicKey());

    xrpl::Serializer payload = signingPayload(stTx);
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

EccSignResult
eccSignFromJson(std::string const& what, json::Value const& txJson, EccSigner const& eccSign)
{
    xrpl::STTx stTx = parseTx(what, txJson);

    xrpl::Serializer payload = signingPayload(stTx);
    auto eccSig = eccSign(xrpl::makeSlice(payload.peekData()));

    stTx.setFieldVL(xrpl::sfTxnSignature, eccSig);

    xrpl::Serializer txSerial;
    stTx.add(txSerial);

    return EccSignResult{
        xrpl::strHex(txSerial.peekData()),
        to_string(stTx.getHash(xrpl::HashPrefix::TransactionId))};
}

}  // namespace pqwallet::sign
