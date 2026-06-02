#pragma once

#include <xrpl/basics/Buffer.h>
#include <xrpl/basics/Slice.h>
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
    detail/ wrappers it sits on top of.

    A wrong-size `secretKey` terminates via logicError rather than throwing,
    matching the contract of the underlying xrpl::mldsa::sign primitive.
*/
[[nodiscard]] Buffer
pqSign(Slice msg, Slice secretKey);

/** Verify a post-quantum signature `sig` over `msg` against `publicKey`.

    Wrong-size inputs return false rather than throwing or terminating, so
    untrusted data can be verified without a precheck.
*/
[[nodiscard]] bool
pqVerify(Slice sig, Slice msg, Slice publicKey) noexcept;

}  // namespace xrpl
