#include <xrpl/basics/strHex.h>
#include <xrpl/json/json_value.h>
#include <xrpl/protocol/AccountID.h>
#include <xrpl/protocol/KeyType.h>
#include <xrpl/protocol/PublicKey.h>
#include <xrpl/protocol/SecretKey.h>
#include <xrpl/protocol/Seed.h>
#include <xrpl/protocol/Sign.h>
#include <xrpl/protocol/jss.h>

#include <arg_utils.h>
#include <commands.h>
#include <defaults.h>
#include <hybrid_sign.h>
#include <rpc_client.h>
#include <wallet_state.h>

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

namespace pqwallet::cmd {

namespace {

struct FundArgs
{
    std::filesystem::path walletPath{defaults::kWalletPath};
    std::string amountDrops{defaults::kFundDrops};
    std::string rpcUrl{defaults::rpcUrl()};
    std::optional<std::uint32_t> sequence;
    std::uint32_t feeDrops{defaults::kFeeDrops};
};

FundArgs
parseFundArgs(int argc, char** argv)
{
    FundArgs out;
    for (int i = 0; i < argc; ++i)
    {
        std::string_view const a = argv[i];
        if (a == "--wallet" && i + 1 < argc && argv[i + 1][0] != '-')
        {
            out.walletPath = argv[++i];
        }
        else if (a == "--amount-drops" && i + 1 < argc && argv[i + 1][0] != '-')
        {
            out.amountDrops = argv[++i];
        }
        else if (a == "--rpc-url" && i + 1 < argc && argv[i + 1][0] != '-')
        {
            out.rpcUrl = argv[++i];
        }
        else if (a == "--sequence" && i + 1 < argc && argv[i + 1][0] != '-')
        {
            out.sequence = args::parseU32(argv[++i], "--sequence");
        }
        else if (a == "--fee" && i + 1 < argc && argv[i + 1][0] != '-')
        {
            out.feeDrops = args::parseU32(argv[++i], "--fee");
        }
        else
        {
            throw std::runtime_error(
                "fund: unknown or incomplete argument '" + std::string(a) + "'");
        }
    }
    return out;
}

}  // namespace

int
fund(int argc, char** argv)
{
    auto const args = parseFundArgs(argc, argv);
    auto const wallet = state::load(args.walletPath);

    // The genesis account holds all XRP on a fresh network. We sign its
    // Payment locally with the well-known test key so the airdrop needs no
    // node-side `sign` RPC and no external tooling.
    auto const [genesisPub, genesisSec] = xrpl::generateKeyPair(
        xrpl::KeyType::Secp256k1, xrpl::generateSeed(defaults::kGenesisSecret));
    auto const genesisAccount = xrpl::toBase58(xrpl::calcAccountID(genesisPub));

    auto const sequence =
        args.sequence ? *args.sequence : rpc::fetchSequence(args.rpcUrl, genesisAccount);

    json::Value tx(json::ValueType::Object);
    tx[xrpl::jss::TransactionType] = "Payment";
    tx[xrpl::jss::Account] = genesisAccount;
    tx[xrpl::jss::Destination] = wallet.accountId;
    tx[xrpl::jss::Amount] = args.amountDrops;
    tx[xrpl::jss::Fee] = std::to_string(args.feeDrops);
    tx[xrpl::jss::Sequence] = sequence;
    tx[xrpl::jss::SigningPubKey] = xrpl::strHex(genesisPub);

    auto const signed_ = sign::eccSignFromJson("fund", tx, [&](xrpl::Slice payload) {
        return xrpl::sign(genesisPub, genesisSec, payload);
    });

    json::Value submitParams(json::ValueType::Object);
    submitParams["tx_blob"] = signed_.txBlobHex;
    auto const result = rpc::call(args.rpcUrl, "submit", std::move(submitParams));
    auto const engineResult = result.get("engine_result", "").asString();

    std::cout << "fund (genesis airdrop) submitted.\n";
    std::cout << "  to_account    : " << wallet.accountId << '\n';
    std::cout << "  amount_drops  : " << args.amountDrops << '\n';
    std::cout << "  tx_hash       : " << signed_.txHash << '\n';
    std::cout << "  engine_result : " << engineResult << '\n';
    return engineResult == "tesSUCCESS" ? EXIT_SUCCESS : EXIT_FAILURE;
}

}  // namespace pqwallet::cmd
