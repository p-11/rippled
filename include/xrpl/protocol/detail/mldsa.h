#pragma once

#include <xrpl/basics/Buffer.h>
#include <xrpl/basics/Slice.h>

#include <cstddef>
#include <utility>

namespace xrpl::mldsa {

inline constexpr std::size_t publicKeySize = 1312;
inline constexpr std::size_t secretKeySize = 2560;
inline constexpr std::size_t signatureSize = 2420;

[[nodiscard]] std::pair<Buffer, Buffer>
keypair();

[[nodiscard]] Buffer
sign(Slice msg, Slice secretKey);

[[nodiscard]] bool
verify(Slice sig, Slice msg, Slice publicKey);

[[nodiscard]] bool
verify(Slice sig, Slice msg, Slice publicKey, Slice context);

}  // namespace xrpl::mldsa
