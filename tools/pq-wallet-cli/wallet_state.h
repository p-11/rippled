#pragma once

#include <filesystem>
#include <string>

namespace pqwallet::state {

// Public, non-secret view of the wallet. Persisted alongside the two
// mock-owned secret state files but holds no secret material itself —
// the EccCustodyMock owns the ECC secret and the PqCustodyMock owns
// the PQ seed.
struct WalletState
{
    std::string accountId;
    std::string keyType;  // "secp256k1" / "ed25519"
    std::string eccPublicKeyHex;
    std::string algorithm;  // "ML-DSA-44"
    std::string pqPublicKeyHex;
};

// Derive the two mock state-file paths from the wallet state file path.
// <wallet>.custody.json holds the ECC mock's secret material;
// <wallet>.pq.json holds the PQ custody mock's 32-byte seed.
std::filesystem::path
custodyStateFileFor(std::filesystem::path const& walletStateFile);

std::filesystem::path
pqStateFileFor(std::filesystem::path const& walletStateFile);

void
save(std::filesystem::path const& walletStateFile, WalletState const& s);

WalletState
load(std::filesystem::path const& walletStateFile);

bool
exists(std::filesystem::path const& walletStateFile);

}  // namespace pqwallet::state
