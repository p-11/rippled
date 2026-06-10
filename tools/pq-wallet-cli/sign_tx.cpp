#include <xrpl/basics/strHex.h>
#include <xrpl/json/json_value.h>
#include <xrpl/json/json_writer.h>
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
#include <fstream>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

namespace pqwallet::cmd {

namespace {

struct SignTxArgs
{
    std::filesystem::path walletPath{defaults::kWalletPath};
    std::filesystem::path outPath{defaults::kSignedTxOutPath};
    std::optional<std::string> toAddress;
    std::optional<std::string> amountDrops;
    std::optional<std::uint32_t> sequence;
    std::uint32_t feeDrops{defaults::kFeeDrops};
    std::string rpcUrl{defaults::kRpcUrl};
};

SignTxArgs
parseSignTxArgs(int argc, char** argv)
{
    SignTxArgs out;
    for (int i = 0; i < argc; ++i)
    {
        std::string_view const a = argv[i];
        if (a == "--wallet" && i + 1 < argc && argv[i + 1][0] != '-')
        {
            out.walletPath = argv[++i];
        }
        else if (a == "--out" && i + 1 < argc && argv[i + 1][0] != '-')
        {
            out.outPath = argv[++i];
        }
        else if (a == "--to" && i + 1 < argc && argv[i + 1][0] != '-')
        {
            out.toAddress = argv[++i];
        }
        else if (a == "--amount-drops" && i + 1 < argc && argv[i + 1][0] != '-')
        {
            out.amountDrops = argv[++i];
        }
        else if (a == "--sequence" && i + 1 < argc && argv[i + 1][0] != '-')
        {
            out.sequence = args::parseU32(argv[++i], "--sequence");
        }
        else if (a == "--fee" && i + 1 < argc && argv[i + 1][0] != '-')
        {
            out.feeDrops = args::parseU32(argv[++i], "--fee");
        }
        else if (a == "--rpc-url" && i + 1 < argc && argv[i + 1][0] != '-')
        {
            out.rpcUrl = argv[++i];
        }
        else
        {
            throw std::runtime_error(
                "sign-tx: unknown or incomplete argument '" + std::string(a) + "'");
        }
    }
    if (!out.toAddress)
        throw std::runtime_error("sign-tx: --to <address> is required");
    if (!out.amountDrops)
        throw std::runtime_error("sign-tx: --amount-drops <n> is required");
    return out;
}

}  // namespace

int
signTx(int argc, char** argv)
{
    auto const args = parseSignTxArgs(argc, argv);
    auto const wallet = state::load(args.walletPath);
    auto const sequence =
        args.sequence ? *args.sequence : rpc::fetchSequence(args.rpcUrl, wallet.accountId);

    custody::EccCustodyMock custodyMock(state::custodyStateFileFor(args.walletPath));
    custodyMock.load();

    custody::PqCustodyMock pqMock(state::pqStateFileFor(args.walletPath));
    pqMock.load();

    // SigningPubKey is set up front to satisfy the tx template when the
    // JSON is parsed into an STTx; the PQ pubkey is attached during signing.
    auto const eccPubHex = xrpl::strHex(custodyMock.publicKey());

    json::Value tx(json::ValueType::Object);
    tx[xrpl::jss::TransactionType] = "Payment";
    tx[xrpl::jss::Account] = wallet.accountId;
    tx[xrpl::jss::Destination] = *args.toAddress;
    tx[xrpl::jss::Amount] = *args.amountDrops;
    tx[xrpl::jss::Fee] = std::to_string(args.feeDrops);
    tx[xrpl::jss::Sequence] = sequence;
    tx[xrpl::jss::SigningPubKey] = eccPubHex;

    auto const signedTx = sign::hybridSignFromJson("sign-tx", tx, custodyMock, pqMock);

    // The sidecar carries the five fields the PoC project description calls
    // for (PQ signature and pubkey, algorithm, payload, ECC signature) plus
    // tx_blob and tx_hash so submit-tx can consume the same file.
    json::Value sidecar(json::ValueType::Object);
    sidecar["algorithm"] = wallet.algorithm;
    sidecar["pq_public_key_hex"] = xrpl::strHex(pqMock.publicKey());
    sidecar["pq_signature_hex"] = xrpl::strHex(signedTx.pqSignature);
    sidecar["ecc_signature_hex"] = xrpl::strHex(signedTx.eccSignature);
    sidecar["ecc_public_key_hex"] = eccPubHex;
    sidecar["serialized_payload_hex"] = xrpl::strHex(signedTx.payload);
    sidecar["tx_blob"] = signedTx.txBlobHex;
    sidecar["tx_hash"] = signedTx.txHash;

    auto const outDir = args.outPath.parent_path();
    if (!outDir.empty())
        std::filesystem::create_directories(outDir);

    std::ofstream out(args.outPath);
    if (!out)
        throw std::runtime_error("sign-tx: cannot open output file: " + args.outPath.string());
    json::StyledWriter writer;
    out << writer.write(sidecar);
    if (!out)
        throw std::runtime_error("sign-tx: failed to write output file: " + args.outPath.string());

    std::cout << "Hybrid Payment signed.\n";
    std::cout << "  account_id      : " << wallet.accountId << '\n';
    std::cout << "  destination     : " << *args.toAddress << '\n';
    std::cout << "  amount_drops    : " << *args.amountDrops << '\n';
    std::cout << "  sequence        : " << sequence << '\n';
    std::cout << "  fee_drops       : " << args.feeDrops << '\n';
    std::cout << "  tx_hash         : " << signedTx.txHash << '\n';
    std::cout << "  sidecar         : " << args.outPath.string() << '\n';
    std::cout << "  ecc_sig (bytes) : " << signedTx.eccSignature.size() << '\n';
    std::cout << "  pq_sig  (bytes) : " << signedTx.pqSignature.size() << '\n';
    std::cout << "\ntx_blob:\n" << signedTx.txBlobHex << '\n';
    return EXIT_SUCCESS;
}

}  // namespace pqwallet::cmd
