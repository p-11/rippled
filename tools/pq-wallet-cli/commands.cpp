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
#include <xrpl/protocol/digest.h>

#include <defaults.h>
#include <ecc_custody_mock.h>
#include <pq_keystore_mock.h>
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
                  << "' already exists. Remove it (and its sibling .custody.json"
                     " / .pq.json files) before generating a new keypair.\n";
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
        auto const pqSeedHash = xrpl::sha512Half(xrpl::Slice(eccSeed.data(), eccSeed.size()));
        static_assert(
            decltype(pqSeedHash)::kBytes >= xrpl::kPQSeedSize,
            "sha512Half output narrower than kPQSeedSize; "
            "revisit the PQ-seed derivation if kPQSeedSize ever grows past 32 bytes.");
        pqSeedBytes.assign(pqSeedHash.data(), pqSeedHash.data() + pqSeedHash.size());
    }

    pqstore::PqKeystoreMock pqMock(state::pqStateFileFor(args.walletPath));
    pqMock.initializeFromPqSeed(xrpl::Slice(pqSeedBytes.data(), pqSeedBytes.size()));

    auto const pqPub = pqMock.publicKey();

    state::WalletState s;
    s.accountId = accountAddress;
    s.keyType = xrpl::to_string(eccKeyType);
    s.eccPublicKeyHex = xrpl::strHex(eccPub);
    s.algorithm = "ML-DSA-44";
    s.pqPublicKeyHex = xrpl::strHex(pqPub);
    state::save(args.walletPath, s);

    auto const pqPubHex = xrpl::strHex(pqPub);
    std::cout << "Wallet keygen complete.\n";
    std::cout << "  account_id              : " << s.accountId << '\n';
    std::cout << "  key_type                : " << s.keyType << '\n';
    std::cout << "  algorithm               : " << s.algorithm << '\n';
    std::cout << "  ecc_public_key (hex)    : " << s.eccPublicKeyHex.substr(0, 32) << "... ("
              << s.eccPublicKeyHex.size() / 2 << " bytes)\n";
    std::cout << "  pq_public_key (hex)     : " << pqPubHex.substr(0, 32) << "... ("
              << pqPubHex.size() / 2 << " bytes)\n";
    std::cout << "  wallet state file       : " << args.walletPath.string() << '\n';
    std::cout << "  ECC custody state file  : " << custodyMock.stateFile().string() << '\n';
    std::cout << "  PQ keystore state file  : " << pqMock.stateFile().string() << '\n';
    return EXIT_SUCCESS;
}

}  // namespace pqwallet::cmd
