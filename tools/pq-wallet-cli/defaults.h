#pragma once

#include <cstdint>

namespace pqwallet::defaults {

// The two mock-owned secret files are siblings derived from this path.
inline constexpr char const* kWalletPath = "./pq-wallet.json";

inline constexpr char const* kSignedTxOutPath = "./signed-tx.json";

inline constexpr std::uint32_t kFeeDrops = 12;

// Default JSON-RPC endpoint. Points at the standalone-mode demo rippled
// from run-demo.sh; pass --rpc-url to target the multi-validator DevNet
// or any other hybrid-aware node.
inline constexpr char const* kRpcUrl = "http://127.0.0.1:5050";

}  // namespace pqwallet::defaults
