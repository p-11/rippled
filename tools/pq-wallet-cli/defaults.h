#pragma once

#include <cstdint>
#include <cstdlib>
#include <string>

namespace pqwallet::defaults {

// The two mock-owned secret files are siblings derived from this path.
inline constexpr char const* kWalletPath = "./pq-wallet.json";

inline constexpr char const* kSignedTxOutPath = "./signed-tx.json";

inline constexpr std::uint32_t kFeeDrops = 12;

// Default JSON-RPC endpoint. Points at the standalone-mode demo xrpld
// from run-demo.sh; pass --rpc-url to target the multi-validator DevNet
// or any other hybrid-aware node.
inline constexpr char const* kRpcUrl = "http://127.0.0.1:5050";

// Resolved default endpoint: PQ_WALLET_RPC_URL if set, else kRpcUrl. Lets a
// demo `export PQ_WALLET_RPC_URL=http://127.0.0.1:5005` once instead of
// repeating --rpc-url on every command.
inline std::string
rpcUrl()
{
    if (char const* env = std::getenv("PQ_WALLET_RPC_URL"); env && env[0] != '\0')
        return env;
    return kRpcUrl;
}

// The well-known genesis account that holds all XRP on a fresh network.
// `fund` airdrops from it; its secret is the standard test passphrase.
inline constexpr char const* kGenesisSecret = "masterpassphrase";

// Default airdrop amount for `fund`: 1,000 XRP, plenty for demo transfers.
inline constexpr char const* kFundDrops = "1000000000";

}  // namespace pqwallet::defaults
