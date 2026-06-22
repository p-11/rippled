#include <commands.h>

#include <xrpl/basics/Slice.h>
#include <xrpl/basics/StringUtilities.h>
#include <xrpl/basics/strHex.h>
#include <xrpl/protocol/AccountID.h>
#include <xrpl/protocol/KeyType.h>
#include <xrpl/protocol/PQSign.h>
#include <xrpl/protocol/PublicKey.h>
#include <xrpl/protocol/SecretKey.h>
#include <xrpl/protocol/Seed.h>

#include <defaults.h>
#include <ecc_custody_mock.h>
#include <pq_custody_mock.h>
#include <wallet_state.h>

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace pqwallet::cmd {

namespace {

struct KeygenArgs
{
    std::filesystem::path walletPath{defaults::kWalletPath};
    std::optional<std::string> importPqSeedHex;
    bool quantum = false;
};

KeygenArgs
parseKeygenArgs(int argc, char** argv)
{
    KeygenArgs out;
    for (int i = 0; i < argc; ++i)
    {
        std::string_view const a = argv[i];
        if (a == "--wallet" && i + 1 < argc && argv[i + 1][0] != '-')
        {
            out.walletPath = argv[++i];
        }
        else if (a == "--import-pq-seed" && i + 1 < argc && argv[i + 1][0] != '-')
        {
            out.importPqSeedHex = argv[++i];
        }
        else if (a == "--quantum")
        {
            out.quantum = true;
        }
        else
        {
            throw std::runtime_error(
                "keygen: unknown or incomplete argument '" + std::string(a) + "'");
        }
    }
    return out;
}

}  // namespace

int
keygen(int argc, char** argv)
{
    auto const args = parseKeygenArgs(argc, argv);

    if (state::exists(args.walletPath))
    {
        std::cerr << "keygen: wallet state file '" << args.walletPath.string()
                  << "' already exists. Remove it (and its sibling .ecc-custody.json"
                     " / .pq-custody.json files) before generating a new keypair.\n";
        return EXIT_FAILURE;
    }

    // In production the ECC seed is minted inside the Custody Application's
    // MPC/HSM ceremony and never reaches this binary.
    auto const eccSeed = xrpl::randomSeed();
    constexpr auto eccKeyType = xrpl::KeyType::Secp256k1;

    custody::EccCustodyMock custodyMock(state::custodyStateFileFor(args.walletPath));
    custodyMock.initializeFromSeed(eccSeed, eccKeyType);

    auto const eccPub = custodyMock.publicKey();
    auto const accountId = xrpl::calcAccountID(eccPub);
    auto const accountAddress = xrpl::toBase58(accountId);

    state::WalletState s;
    s.accountId = accountAddress;
    s.keyType = xrpl::to_string(eccKeyType);
    s.eccPublicKeyHex = xrpl::strHex(eccPub);

    // Post-quantum is opt-in: a bare keygen mints an ECC-only wallet (a normal
    // pre-migration account). --quantum, or importing a PQ seed, adds the
    // ML-DSA-44 key, which is what later lets the account opt in on-ledger.
    bool const wantPq = args.quantum || args.importPqSeedHex.has_value();
    std::optional<std::filesystem::path> pqStateFile;
    if (wantPq)
    {
        // Deriving the PQ seed from the ECC seed keeps keygen reproducible from
        // what `wallet_propose key_type=dilithium` mints for the same seed;
        // --import-pq-seed lets an operator reuse a server-minted key.
        std::vector<std::uint8_t> pqSeedBytes;
        if (args.importPqSeedHex)
        {
            auto const decoded = xrpl::strUnHex(*args.importPqSeedHex);
            if (!decoded)
            {
                std::cerr << "keygen: --import-pq-seed is not valid hex\n";
                return EXIT_FAILURE;
            }
            if (decoded->size() != xrpl::kPQSeedSize)
            {
                std::cerr << "keygen: --import-pq-seed must be exactly " << xrpl::kPQSeedSize
                          << " bytes (" << (xrpl::kPQSeedSize * 2) << " hex chars); got "
                          << decoded->size() << " bytes.\n";
                return EXIT_FAILURE;
            }
            pqSeedBytes = std::move(*decoded);
        }
        else
        {
            auto const pqSeedHash = xrpl::pqSeedFromSeed(eccSeed);
            pqSeedBytes.assign(pqSeedHash.data(), pqSeedHash.data() + pqSeedHash.size());
        }

        custody::PqCustodyMock pqMock(state::pqStateFileFor(args.walletPath));
        pqMock.initializeFromPqSeed(xrpl::Slice(pqSeedBytes.data(), pqSeedBytes.size()));
        s.algorithm = "ML-DSA-44";
        s.pqPublicKeyHex = xrpl::strHex(pqMock.publicKey());
        pqStateFile = pqMock.stateFile();
    }

    state::save(args.walletPath, s);

    std::cout << "Wallet keygen complete.\n";
    std::cout << "  account_id              : " << s.accountId << '\n';
    std::cout << "  key_type                : " << s.keyType << '\n';
    std::cout << "  ecc_public_key (hex)    : " << s.eccPublicKeyHex.substr(0, 32) << "... ("
              << s.eccPublicKeyHex.size() / 2 << " bytes)\n";
    if (wantPq)
    {
        std::cout << "  algorithm               : " << s.algorithm << '\n';
        std::cout << "  pq_public_key (hex)     : " << s.pqPublicKeyHex.substr(0, 32) << "... ("
                  << s.pqPublicKeyHex.size() / 2 << " bytes)\n";
    }
    else
    {
        std::cout << "  post-quantum            : not generated (ECC-only wallet; re-run "
                     "with --quantum to add an ML-DSA-44 key)\n";
    }
    std::cout << "  wallet state file       : " << args.walletPath.string() << '\n';
    std::cout << "  ECC custody state file  : " << custodyMock.stateFile().string() << '\n';
    if (pqStateFile)
        std::cout << "  PQ custody state file   : " << pqStateFile->string() << '\n';
    return EXIT_SUCCESS;
}

}  // namespace pqwallet::cmd
