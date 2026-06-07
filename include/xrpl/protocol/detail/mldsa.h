#pragma once

#include <xrpl/basics/Buffer.h>
#include <xrpl/basics/Slice.h>

#include <cstddef>
#include <utility>

namespace xrpl::mldsa {

inline constexpr std::size_t kPublicKeySize = 1312;
inline constexpr std::size_t kSecretKeySize = 2560;
inline constexpr std::size_t kSignatureSize = 2420;
inline constexpr std::size_t kSeedSize = 32;

[[nodiscard]] std::pair<Buffer, Buffer>
keypair();

/** Deterministic ML-DSA-44 keypair from a 32-byte seed.

    Wraps the mldsa-native `crypto_sign_keypair_internal` primitive
    that takes a caller-supplied seed instead of drawing entropy from
    randombytes. Two calls with the same seed produce identical
    keypairs, which lets `wallet_propose` honour the same
    passphrase/seed/seed_hex flow it uses for ECC.

    A wrong-size `seed` terminates via logicError, matching the
    contract of the other primitives in this header.
*/
[[nodiscard]] std::pair<Buffer, Buffer>
keypair(Slice seed);

[[nodiscard]] Buffer
sign(Slice msg, Slice secretKey);

[[nodiscard]] Buffer
sign(Slice msg, Slice secretKey, Slice context);

[[nodiscard]] bool
verify(Slice sig, Slice msg, Slice publicKey) noexcept;

[[nodiscard]] bool
verify(Slice sig, Slice msg, Slice publicKey, Slice context) noexcept;

}  // namespace xrpl::mldsa
