#pragma once

#include <test/jtx/Env.h>
#include <test/jtx/SignerUtils.h>
#include <test/jtx/quantum_sign.h>

#include <optional>
#include <utility>
#include <vector>

namespace xrpl::test::jtx {

/** One entry in a hybrid multi-signature.

    `pqKey` is optional so a single transaction can mix hybrid signers and
    plain ECC signers; nullopt means the entry signs with ECC only.
*/
struct QuantumSigner
{
    Reg reg;
    std::optional<PQKey> pqKey;

    QuantumSigner(Reg r, PQKey k) : reg(std::move(r)), pqKey(std::move(k))
    {
    }

    QuantumSigner(Reg r) : reg(std::move(r))
    {
    }
};

/** JTx requirement: attach a hybrid ECC + ML-DSA-44 multi-signature.

    Mirror of `Msig` for the top-level sfSigners path. Each entry independently
    chooses whether to sign with ECC alone or with both ECC and PQ; the verifier
    in multiSignHelper accepts either per signer.
*/
class QuantumMsig
{
public:
    std::vector<QuantumSigner> signers;

    explicit QuantumMsig(std::vector<QuantumSigner> s);

    void
    operator()(Env&, JTx& jt) const;
};

inline QuantumMsig
quantum_msig(std::vector<QuantumSigner> signers)
{
    return QuantumMsig{std::move(signers)};
}

}  // namespace xrpl::test::jtx
