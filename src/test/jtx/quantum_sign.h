#pragma once

#include <test/jtx/Account.h>
#include <test/jtx/Env.h>

#include <xrpl/basics/Buffer.h>
#include <xrpl/basics/Slice.h>

#include <utility>

namespace xrpl::test::jtx {

/** Ephemeral ML-DSA-44 keypair for hybrid-signed test transactions.

    Held by value in `QuantumSign` so a single keypair can be reused across
    several `env(...)` calls in the same test, or rotated to model swap /
    mismatch scenarios.
*/
class PQKey
{
public:
    /** Generate a fresh keypair. */
    [[nodiscard]] static PQKey
    generate();

    [[nodiscard]] Slice
    publicKey() const noexcept
    {
        return Slice(pk_);
    }

    [[nodiscard]] Slice
    secretKey() const noexcept
    {
        return Slice(sk_);
    }

private:
    PQKey(Buffer pk, Buffer sk) : pk_(std::move(pk)), sk_(std::move(sk))
    {
    }

    Buffer pk_;
    Buffer sk_;
};

/** JTx requirement: attach a hybrid ECC + PQ signature pair to the transaction.

    Bypasses Env's autofill ECC-only path and fills sfSigningPubKey,
    sfQuantumPubKey, sfTxnSignature, and sfQuantumSignature in one step so the
    canonical signing payload reflects both pubkeys when each signature is
    computed.
*/
class QuantumSign
{
public:
    QuantumSign(Account account, PQKey pqKey)
        : account_(std::move(account)), pqKey_(std::move(pqKey))
    {
    }

    void
    operator()(Env&, JTx& jt) const;

private:
    Account account_;
    PQKey pqKey_;
};

inline QuantumSign
quantum_sign(Account account, PQKey pqKey)
{
    return QuantumSign(std::move(account), std::move(pqKey));
}

}  // namespace xrpl::test::jtx
