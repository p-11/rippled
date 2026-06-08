#pragma once

#include <cstdint>

namespace pqwallet::defaults {

// The two mock-owned secret files are siblings derived from this path.
inline constexpr char const* kWalletPath = "./pq-wallet.json";

inline constexpr char const* kSignedTxOutPath = "./signed-tx.json";

inline constexpr std::uint32_t kFeeDrops = 12;

}  // namespace pqwallet::defaults
