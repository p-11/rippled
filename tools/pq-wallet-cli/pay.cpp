#include <xrpl/basics/strHex.h>
#include <xrpl/json/json_value.h>
#include <xrpl/protocol/PublicKey.h>
#include <xrpl/protocol/jss.h>

#include <arg_utils.h>
#include <commands.h>
#include <defaults.h>
#include <ecc_custody_mock.h>
#include <hybrid_sign.h>
#include <pq_custody_mock.h>
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

struct PayArgs
{
    std::filesystem::path walletPath{defaults::kWalletPath};
    std::optional<std::string> toAddress;
    std::optional<std::filesystem::path> toWallet;
    std::optional<std::string> amountDrops;
    std::string rpcUrl{defaults::rpcUrl()};
    std::optional<std::uint32_t> sequence;
    std::uint32_t feeDrops{defaults::kFeeDrops};
    bool eccOnly{false};
};

PayArgs
parsePayArgs(int argc, char** argv)
{
    PayArgs out;
    for (int i = 0; i < argc; ++i)
    {
        std::string_view const a = argv[i];
        if (a == "--wallet" && i + 1 < argc && argv[i + 1][0] != '-')
        {
            out.walletPath = argv[++i];
        }
        else if (a == "--to" && i + 1 < argc && argv[i + 1][0] != '-')
        {
            out.toAddress = argv[++i];
        }
        else if (a == "--to-wallet" && i + 1 < argc && argv[i + 1][0] != '-')
        {
            out.toWallet = argv[++i];
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
        else if (a == "--ecc-only")
        {
            out.eccOnly = true;
        }
        else
        {
            throw std::runtime_error(
                "pay: unknown or incomplete argument '" + std::string(a) + "'");
        }
    }
    if (out.toAddress && out.toWallet)
        throw std::runtime_error("pay: --to and --to-wallet are mutually exclusive");
    if (!out.toAddress && !out.toWallet)
        throw std::runtime_error("pay: one of --to <address> or --to-wallet <path> is required");
    if (!out.amountDrops)
        throw std::runtime_error("pay: --amount-drops <n> is required");
    return out;
}

}  // namespace

int
pay(int argc, char** argv)
{
    auto const args = parsePayArgs(argc, argv);
    auto const wallet = state::load(args.walletPath);

    // --to-wallet resolves the destination from another wallet's state file,
    // so the demo never has to copy an account id by hand.
    auto const destination =
        args.toAddress ? *args.toAddress : state::load(*args.toWallet).accountId;

    auto const sequence =
        args.sequence ? *args.sequence : rpc::fetchSequence(args.rpcUrl, wallet.accountId);

    custody::EccCustodyMock custodyMock(state::custodyStateFileFor(args.walletPath));
    custodyMock.load();
    custody::PqCustodyMock pqMock(state::pqStateFileFor(args.walletPath));
    pqMock.load();

    json::Value tx(json::ValueType::Object);
    tx[xrpl::jss::TransactionType] = "Payment";
    tx[xrpl::jss::Account] = wallet.accountId;
    tx[xrpl::jss::Destination] = destination;
    tx[xrpl::jss::Amount] = *args.amountDrops;
    tx[xrpl::jss::Fee] = std::to_string(args.feeDrops);
    tx[xrpl::jss::Sequence] = sequence;
    tx[xrpl::jss::SigningPubKey] = xrpl::strHex(custodyMock.publicKey());

    std::string txBlobHex;
    std::string txHash;
    if (args.eccOnly)
    {
        // Bad-weather test: sign the classical signature only. Once the
        // source account has opted in to hybrid, the server rejects this
        // with tefBAD_AUTH (Transactor: account requires a quantum signature).
        auto const signed_ = sign::eccSignFromJson(
            "pay", tx, [&](xrpl::Slice payload) { return custodyMock.signWithECC(payload); });
        txBlobHex = signed_.txBlobHex;
        txHash = signed_.txHash;
    }
    else
    {
        auto const signed_ = sign::hybridSignFromJson("pay", tx, custodyMock, pqMock);
        txBlobHex = signed_.txBlobHex;
        txHash = signed_.txHash;
    }

    json::Value submitParams(json::ValueType::Object);
    submitParams["tx_blob"] = txBlobHex;
    auto const result = rpc::call(args.rpcUrl, "submit", std::move(submitParams));
    auto const engineResult = result.get("engine_result", "").asString();

    std::cout << "pay (" << (args.eccOnly ? "ECC-only" : "hybrid") << ") submitted.\n";
    std::cout << "  from          : " << wallet.accountId << '\n';
    std::cout << "  to            : " << destination << '\n';
    std::cout << "  amount_drops  : " << *args.amountDrops << '\n';
    std::cout << "  sequence      : " << sequence << '\n';
    std::cout << "  tx_hash       : " << txHash << '\n';
    std::cout << "  engine_result : " << engineResult << '\n';
    return engineResult == "tesSUCCESS" ? EXIT_SUCCESS : EXIT_FAILURE;
}

}  // namespace pqwallet::cmd
