#pragma once

#include <xrpl/basics/Buffer.h>
#include <xrpl/basics/Slice.h>

#include <cstddef>
#include <utility>

namespace xrpl::mldsa {

inline constexpr std::size_t kPublicKeySize = 1312;
inline constexpr std::size_t kSecretKeySize = 2560;
inline constexpr std::size_t kSignatureSize = 2420;

[[nodiscard]] std::pair<Buffer, Buffer>
keypair();

[[nodiscard]] Buffer
sign(Slice msg, Slice secretKey);

[[nodiscard]] Buffer
sign(Slice msg, Slice secretKey, Slice context);

[[nodiscard]] bool
verify(Slice sig, Slice msg, Slice publicKey);

[[nodiscard]] bool
verify(Slice sig, Slice msg, Slice publicKey, Slice context);

}  // namespace xrpl::mldsa
