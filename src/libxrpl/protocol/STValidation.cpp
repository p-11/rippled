#include <xrpl/protocol/STValidation.h>

#include <xrpl/basics/Blob.h>
#include <xrpl/basics/Slice.h>
#include <xrpl/basics/base_uint.h>
#include <xrpl/basics/chrono.h>
#include <xrpl/beast/utility/instrumentation.h>
#include <xrpl/protocol/HashPrefix.h>
#include <xrpl/protocol/KeyType.h>
#include <xrpl/protocol/PQSign.h>
#include <xrpl/protocol/PublicKey.h>
#include <xrpl/protocol/SField.h>
#include <xrpl/protocol/SOTemplate.h>
#include <xrpl/protocol/STBase.h>
#include <xrpl/protocol/STObject.h>
#include <xrpl/protocol/Serializer.h>

#include <cstddef>
#include <utility>

namespace xrpl {

STBase*
STValidation::copy(std::size_t n, void* buf) const
{
    return emplace(n, buf, *this);
}

STBase*
STValidation::move(std::size_t n, void* buf)
{
    return emplace(n, buf, std::move(*this));
}

SOTemplate const&
STValidation::validationFormat()
{
    // We can't have this be a magic static at namespace scope because
    // it relies on the SField's below being initialized, and we can't
    // guarantee the initialization order.
    // clang-format off
    static SOTemplate const kFormat{
        {sfFlags,               SoeRequired},
        {sfLedgerHash,          SoeRequired},
        {sfLedgerSequence,      SoeRequired},
        {sfCloseTime,           SoeOptional},
        {sfLoadFee,             SoeOptional},
        {sfAmendments,          SoeOptional},
        {sfBaseFee,             SoeOptional},
        {sfReserveBase,         SoeOptional},
        {sfReserveIncrement,    SoeOptional},
        {sfSigningTime,         SoeRequired},
        {sfSigningPubKey,       SoeRequired},
        {sfSignature,           SoeRequired},
        {sfConsensusHash,       SoeOptional},
        {sfCookie,              SoeDefault},
        {sfValidatedHash,       SoeOptional},
        {sfServerVersion,       SoeOptional},
        // featureXRPFees
        {sfBaseFeeDrops,          SoeOptional},
        {sfReserveBaseDrops,      SoeOptional},
        {sfReserveIncrementDrops, SoeOptional},
        // featureQuantum
        {sfQuantumPubKey,         SoeOptional},
        {sfQuantumSignature,      SoeOptional},
    };
    // clang-format on

    return kFormat;
};

uint256
STValidation::getSigningHash() const
{
    return STObject::getSigningHash(HashPrefix::Validation);
}

uint256
STValidation::getLedgerHash() const
{
    return getFieldH256(sfLedgerHash);
}

uint256
STValidation::getConsensusHash() const
{
    return getFieldH256(sfConsensusHash);
}

NetClock::time_point
STValidation::getSignTime() const
{
    return NetClock::time_point{NetClock::duration{getFieldU32(sfSigningTime)}};
}

NetClock::time_point
STValidation::getSeenTime() const noexcept
{
    return seenTime_;
}

bool
STValidation::isValid() const noexcept
{
    if (!valid_)
    {
        // verifyDigest is hardcoded to secp256k1; gate the call so a
        // non-secp256k1-signed validation reaching this path is reported
        // invalid instead of triggering a logic_error inside a noexcept
        // context.
        if (publicKeyType(getSignerPublic()) != KeyType::Secp256k1)
        {
            valid_ = false;
            return false;
        }

        // Tx-path parity ("Mismatched post-quantum signature fields"):
        // sfQuantumSignature is excluded from the ECC signing hash
        // (kNotSigning), so without this guard anyone could append junk PQ
        // bytes to a captured validation, keep the ECC signature valid, and
        // relay unlimited distinct-suppression variants of it.
        bool const hasPQPub = isFieldPresent(sfQuantumPubKey);
        bool const hasPQSig = isFieldPresent(sfQuantumSignature);
        if (hasPQPub != hasPQSig)
        {
            valid_ = false;
            return false;
        }

        bool ok = verifyDigest(
            getSignerPublic(),
            getSigningHash(),
            makeSlice(getFieldVL(sfSignature)),
            (getFlags() & kVfFullyCanonicalSig) != 0u);

        if (ok && hasPQPub)
        {
            try
            {
                // Zero-copy: the 1312-byte PQ pubkey is only read by pqVerify,
                // so getFieldVL's heap copy is wasted on this per-validation
                // consensus path.
                ok = pqVerify(*this, HashPrefix::Validation, (*this)[sfQuantumPubKey]);
            }
            catch (...)
            {
                ok = false;
            }
        }

        valid_ = ok;
    }

    return valid_.value();
}

bool
STValidation::isFull() const noexcept
{
    return (getFlags() & kVfFullValidation) != 0;
}

Blob
STValidation::getSignature() const
{
    return getFieldVL(sfSignature);
}

Blob
STValidation::getSerialized() const
{
    Serializer s;
    add(s);
    return s.peekData();
}

}  // namespace xrpl
