// Copyright (c) 2019 Dev Null Productions

#include <xrpld/app/main/Application.h>
#include <xrpld/rpc/Context.h>

#include <xrpl/basics/Slice.h>
#include <xrpl/basics/base64.h>
#include <xrpl/basics/strHex.h>
#include <xrpl/json/json_value.h>
#include <xrpl/protocol/ErrorCodes.h>
#include <xrpl/protocol/PublicKey.h>
#include <xrpl/protocol/jss.h>
#include <xrpl/protocol/tokens.h>

namespace xrpl {
json::Value
doManifest(RPC::JsonContext& context)
{
    auto& params = context.params;

    if (!params.isMember(jss::public_key))
        return RPC::missingFieldError(jss::public_key);

    auto const requested = params[jss::public_key].asString();

    json::Value ret;
    ret[jss::requested] = requested;

    auto const pk = parseBase58<PublicKey>(TokenType::NodePublic, requested);
    if (!pk)
    {
        RPC::injectError(RpcInvalidParams, ret);
        return ret;
    }

    // first attempt to use params as ephemeral key,
    // if this lookup succeeds master key will be returned,
    // else an unseated optional is returned
    auto const mk = context.app.getValidatorManifests().getMasterKey(*pk);

    auto const ek = context.app.getValidatorManifests().getSigningKey(mk);

    // if ephemeral key not found, we don't have specified manifest
    if (!ek)
        return ret;

    if (auto const manifest = context.app.getValidatorManifests().getManifest(mk))
        ret[jss::manifest] = base64Encode(*manifest);
    json::Value details;

    details[jss::master_key] = toBase58(TokenType::NodePublic, mk);
    details[jss::ephemeral_key] = toBase58(TokenType::NodePublic, *ek);

    if (auto const seq = context.app.getValidatorManifests().getSequence(mk))
        details[jss::seq] = *seq;

    if (auto const domain = context.app.getValidatorManifests().getDomain(mk))
        details[jss::domain] = *domain;

    // Hybrid manifests carry an ML-DSA-44 master pubkey and ephemeral
    // signing pubkey alongside the ECC pair. Surface both as hex when
    // present (base58 is impractical at 1312 bytes).
    auto const& mc = context.app.getValidatorManifests();
    if (auto const pqMaster = mc.getQuantumMasterKey(mk))
        details[jss::pq_master_key] = strHex(Slice(pqMaster->data(), pqMaster->size()));
    if (auto const pqEphemeral = mc.getQuantumSigningKey(mk))
        details[jss::pq_ephemeral_key] = strHex(Slice(pqEphemeral->data(), pqEphemeral->size()));

    ret[jss::details] = details;
    return ret;
}
}  // namespace xrpl
