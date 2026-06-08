#include <xrpl/json/json_reader.h>
#include <xrpl/json/json_value.h>
#include <xrpl/json/json_writer.h>
#include <xrpl/protocol/jss.h>

#include <commands.h>
#include <defaults.h>
#include <rpc_client.h>
#include <wallet_state.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace pqwallet::cmd {

namespace {

struct SubmitArgs
{
    std::optional<std::string> blob;
    std::optional<std::filesystem::path> inPath;
    std::string rpcUrl{defaults::kRpcUrl};
    bool verbose{false};
};

SubmitArgs
parseSubmitArgs(int argc, char** argv)
{
    SubmitArgs out;
    for (int i = 0; i < argc; ++i)
    {
        std::string_view const a = argv[i];
        if (a == "--blob" && i + 1 < argc && argv[i + 1][0] != '-')
        {
            out.blob = argv[++i];
        }
        else if (a == "--in" && i + 1 < argc && argv[i + 1][0] != '-')
        {
            out.inPath = argv[++i];
        }
        else if (a == "--rpc-url" && i + 1 < argc && argv[i + 1][0] != '-')
        {
            out.rpcUrl = argv[++i];
        }
        else if (a == "--verbose" || a == "-v")
        {
            out.verbose = true;
        }
        else
        {
            throw std::runtime_error(
                "submit-tx: unknown or incomplete argument '" + std::string(a) + "'");
        }
    }
    if (!out.blob && !out.inPath)
        throw std::runtime_error("submit-tx: one of --blob <hex> or --in <path> is required");
    if (out.blob && out.inPath)
        throw std::runtime_error("submit-tx: --blob and --in are mutually exclusive");
    return out;
}

std::string
readTxBlobFromSidecar(std::filesystem::path const& p)
{
    std::ifstream in(p);
    if (!in)
        throw std::runtime_error("submit-tx: cannot read sidecar file: " + p.string());
    std::stringstream ss;
    ss << in.rdbuf();

    json::Value root;
    json::Reader reader;
    if (!reader.parse(ss.str(), root))
        throw std::runtime_error(
            "submit-tx: sidecar is not valid JSON: " + reader.getFormattedErrorMessages());
    auto const blob = root.get("tx_blob", "").asString();
    if (blob.empty())
        throw std::runtime_error("submit-tx: sidecar has no tx_blob field");
    return blob;
}

struct ShowArgs
{
    std::filesystem::path walletPath{defaults::kWalletPath};
    std::string rpcUrl{defaults::kRpcUrl};
};

ShowArgs
parseShowArgs(int argc, char** argv)
{
    ShowArgs out;
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
        else
        {
            throw std::runtime_error(
                "show-account: unknown or incomplete argument '" + std::string(a) + "'");
        }
    }
    return out;
}

}  // namespace

int
submitTx(int argc, char** argv)
{
    auto const args = parseSubmitArgs(argc, argv);
    auto const blob = args.blob ? *args.blob : readTxBlobFromSidecar(*args.inPath);

    json::Value params(json::ValueType::Object);
    params["tx_blob"] = blob;

    auto const result = rpc::call(args.rpcUrl, "submit", std::move(params));
    auto const engineResult = result.get("engine_result", "").asString();
    auto const txHash =
        result.get("tx_json", json::Value(json::ValueType::Object)).get("hash", "").asString();

    std::cout << "submit-tx result:\n";
    std::cout << "  engine_result : " << engineResult << '\n';
    if (!txHash.empty())
        std::cout << "  tx_hash       : " << txHash << '\n';
    std::cout << "  rpc_url       : " << args.rpcUrl << '\n';
    if (args.verbose)
    {
        json::StyledWriter writer;
        std::cout << "\nFull RPC response:\n" << writer.write(result);
    }
    return engineResult == "tesSUCCESS" ? EXIT_SUCCESS : EXIT_FAILURE;
}

int
showAccount(int argc, char** argv)
{
    auto const args = parseShowArgs(argc, argv);
    auto const wallet = state::load(args.walletPath);

    json::Value params(json::ValueType::Object);
    params["account"] = wallet.accountId;
    params["ledger_index"] = "validated";

    auto const result = rpc::call(args.rpcUrl, "account_info", std::move(params));
    auto const data = result.get("account_data", json::Value(json::ValueType::Object));

    auto const balance = data.get("Balance", "").asString();
    auto const sequence = data.get("Sequence", 0u).asUInt();
    auto const onChainPq = data.get("QuantumPubKey", "").asString();

    std::cout << "show-account:\n";
    std::cout << "  account_id            : " << wallet.accountId << '\n';
    std::cout << "  balance (drops)       : " << balance << '\n';
    std::cout << "  sequence              : " << sequence << '\n';
    if (onChainPq.empty())
    {
        std::cout << "  on-chain QuantumPubKey: <absent> (account not opted in"
                     " to hybrid)\n";
    }
    else
    {
        std::cout << "  on-chain QuantumPubKey: " << onChainPq.substr(0, 32) << "... ("
                  << onChainPq.size() / 2 << " bytes)\n";
        if (onChainPq == wallet.pqPublicKeyHex)
            std::cout << "  match local pq_pub    : YES\n";
        else
            std::cout << "  match local pq_pub    : NO — on-chain key differs"
                         " from this wallet's PQ pubkey\n";
    }
    return EXIT_SUCCESS;
}

}  // namespace pqwallet::cmd
