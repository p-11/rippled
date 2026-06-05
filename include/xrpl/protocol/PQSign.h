#pragma once

#include <xrpl/basics/Buffer.h>
#include <xrpl/basics/Slice.h>
#include <xrpl/protocol/HashPrefix.h>
#include <xrpl/protocol/SField.h>
#include <xrpl/protocol/STObject.h>
#include <xrpl/protocol/detail/mldsa.h>

#include <cstddef>

namespace xrpl {

// Protocol-layer aliases. Callers reach for these names rather than
// xrpl::mldsa::* so the underlying post-quantum algorithm stays an
// implementation detail of this header.
inline constexpr std::size_t kPQPublicKeySize = mldsa::kPublicKeySize;
inline constexpr std::size_t kPQSecretKeySize = mldsa::kSecretKeySize;
inline constexpr std::size_t kPQSignatureSize = mldsa::kSignatureSize;

/** Produce a post-quantum signature over `msg` using `secretKey`.

    Post-quantum sibling of xrpl::sign. Raw-byte surface — payload assembly
    (HashPrefix, serialization) is the caller's responsibility, same as the
    detail/ wrappers it sits on top of. Parameter order matches xrpl::sign
    (key-args first, message last) rather than the underlying mldsa
    primitive's (msg, sk) order.

    A wrong-size `secretKey` terminates via logicError rather than throwing,
    matching the contract of the underlying xrpl::mldsa::sign primitive.
*/
[[nodiscard]] Buffer
pqSign(Slice secretKey, Slice msg);

/** Verify a post-quantum signature `sig` over `msg` against `publicKey`.

    Parameter order matches xrpl::verify (publicKey, msg, sig) rather than
    the underlying mldsa primitive's (sig, msg, pk) order, so call sites
    that verify both ECC and PQ in sequence read uniformly.

    Wrong-size inputs return false rather than throwing or terminating, so
    untrusted data can be verified without a precheck.
*/
[[nodiscard]] bool
pqVerify(Slice publicKey, Slice msg, Slice sig) noexcept;

/** Produce a post-quantum signature over an STObject and store it on the object.

    Post-quantum sibling of the ECC `xrpl::sign(STObject&, ...)` helper. The
    signed payload is `HashPrefix || serialize(st without signing fields)`,
    identical to the ECC path, so PQ and ECC signatures over the same
    object commit to the same bytes. `sigField` defaults to
    `sfQuantumSignature`; it is set on `st` after signing (overwriting any
    prior value).
*/
void
pqSign(
    STObject& st,
    HashPrefix const& prefix,
    Slice secretKey,
    SF_VL const& sigField = sfQuantumSignature);

/** Verify a post-quantum signature stored on an STObject.

    Symmetric counterpart to `pqSign(STObject&, ...)`. Returns false if
    `sigField` is absent from `st`, the signature is the wrong size, or
    the signature does not verify against the same
    `HashPrefix || serialize(st without signing fields)` payload.
*/
[[nodiscard]] bool
pqVerify(
    STObject const& st,
    HashPrefix const& prefix,
    Slice publicKey,
    SF_VL const& sigField = sfQuantumSignature);

}  // namespace xrpl
