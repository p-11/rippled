#include <xrpl/basics/strHex.h>
#include <xrpl/json/json_value.h>
#include <xrpl/protocol/TxFlags.h>
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

struct OptInArgs
{
    std::filesystem::path walletPath{defaults::kWalletPath};
    std::string rpcUrl{defaults::kRpcUrl};
    std::optional<std::uint32_t> sequence;
    std::uint32_t feeDrops{defaults::kFeeDrops};
};

OptInArgs
parseOptInArgs(int argc, char** argv)
{
    OptInArgs out;
    for (int i = 0; i < argc; ++i)
    {
        std::string_view const a = argv[i];
        if (a == "--wallet" && i + 1 < argc && argv[i + 1][0] != '-')
        {
            out.walletPath = argv[++i];
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
                "opt-in: unknown or incomplete argument '" + std::string(a) + "'");
        }
    }
    return out;
}

}  // namespace

int
optIn(int argc, char** argv)
{
    auto const args = parseOptInArgs(argc, argv);
    auto const wallet = state::load(args.walletPath);
    auto const sequence =
        args.sequence ? *args.sequence : rpc::fetchSequence(args.rpcUrl, wallet.accountId);

    custody::EccCustodyMock custodyMock(state::custodyStateFileFor(args.walletPath));
    custodyMock.load();
    custody::PqCustodyMock pqMock(state::pqStateFileFor(args.walletPath));
    pqMock.load();

    auto const eccPubHex = xrpl::strHex(custodyMock.publicKey());
    auto const pqPub = pqMock.publicKey();
    auto const pqPubHex = xrpl::strHex(pqPub);

    // AccountSet asfQuantum registers the PQ pubkey on the AccountRoot;
    // every later tx from this account must carry a matching PQ signature.
    json::Value tx(json::ValueType::Object);
    tx[xrpl::jss::TransactionType] = "AccountSet";
    tx[xrpl::jss::Account] = wallet.accountId;
    tx[xrpl::jss::SetFlag] = xrpl::asfQuantum;
    tx[xrpl::jss::Fee] = std::to_string(args.feeDrops);
    tx[xrpl::jss::Sequence] = sequence;
    tx[xrpl::jss::SigningPubKey] = eccPubHex;

    auto const signedTx = sign::hybridSignFromJson("opt-in", tx, custodyMock, pqMock);

    json::Value submitParams(json::ValueType::Object);
    submitParams["tx_blob"] = signedTx.txBlobHex;
    auto const result = rpc::call(args.rpcUrl, "submit", std::move(submitParams));
    auto const engineResult = result.get("engine_result", "").asString();

    std::cout << "opt-in (AccountSet asfQuantum) submitted.\n";
    std::cout << "  account_id    : " << wallet.accountId << '\n';
    std::cout << "  pq_pub_key    : " << pqPubHex.substr(0, 32) << "...\n";
    std::cout << "  sequence      : " << sequence << '\n';
    std::cout << "  engine_result : " << engineResult << '\n';
    return engineResult == "tesSUCCESS" ? EXIT_SUCCESS : EXIT_FAILURE;
}

}  // namespace pqwallet::cmd
