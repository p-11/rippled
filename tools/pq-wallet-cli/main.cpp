#include <commands.h>

#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

constexpr std::string_view kUsage =
    R"usage(pq-wallet-cli — off-chain hybrid (ECC + ML-DSA-44) custody wallet PoC

Usage:
  pq-wallet-cli <command> [options]

Commands:
  keygen          Generate (or import) the ECC + PQ keypair and persist wallet state.
  fund            Airdrop XRP from genesis to the wallet account (no node sign RPC).
  pay             Hybrid-sign and submit a Payment in one step (--ecc-only for the
                  bad-weather test that an opted-in account rejects with tefBAD_AUTH).
  status          Summarize a node's server_info (state, ledgers, peers).
  opt-in          Register the wallet's PQ pubkey on its AccountRoot via AccountSet asfQuantum.
  sign-tx         Build, hybrid-sign, and emit a Payment transaction (custody sidecar).
  submit-tx       Submit a previously signed tx_blob to a hybrid-aware xrpld.
  show-account    Display the wallet's on-ledger AccountRoot, including QuantumPubKey.

Common options:
  --wallet <path>     Wallet state file. Defaults to ./pq-wallet.json.
                      Secret material lives in <path>.custody.json (ECC mock)
                      and <path>.pq.json (PQ custody mock); the wallet binary
                      never opens those files directly.
  --rpc-url <url>     Hybrid-aware xrpld JSON-RPC endpoint. Defaults to
                      $PQ_WALLET_RPC_URL if set, else http://127.0.0.1:5050
                      (the standalone-mode demo). For a multi-validator DevNet
                      pass --rpc-url http://127.0.0.1:5005 (stock-1).
  -h, --help          Show this help.

The wallet treats the ECC half as a blackbox standing in for the Ripple
Custody Application (MPC + HSM) and exposes a parallel module boundary on
the PQ side where a PQ-capable HSM would plug in. See README.md for the
full conceptual mapping.
)usage";

}  // namespace

int
main(int argc, char** argv)
{
    if (argc < 2)
    {
        std::cout << kUsage;
        return EXIT_SUCCESS;
    }

    std::string_view const command = argv[1];

    if (command == "-h" || command == "--help" || command == "help")
    {
        std::cout << kUsage;
        return EXIT_SUCCESS;
    }

    try
    {
        if (command == "keygen")
            return pqwallet::cmd::keygen(argc - 2, argv + 2);
        if (command == "fund")
            return pqwallet::cmd::fund(argc - 2, argv + 2);
        if (command == "pay")
            return pqwallet::cmd::pay(argc - 2, argv + 2);
        if (command == "status")
            return pqwallet::cmd::status(argc - 2, argv + 2);
        if (command == "sign-tx")
            return pqwallet::cmd::signTx(argc - 2, argv + 2);
        if (command == "submit-tx")
            return pqwallet::cmd::submitTx(argc - 2, argv + 2);
        if (command == "show-account")
            return pqwallet::cmd::showAccount(argc - 2, argv + 2);
        if (command == "opt-in")
            return pqwallet::cmd::optIn(argc - 2, argv + 2);
    }
    catch (std::exception const& e)
    {
        std::cerr << "pq-wallet-cli: " << command << " failed: " << e.what() << '\n';
        return EXIT_FAILURE;
    }

    std::cerr << "pq-wallet-cli: unknown command '" << command << "'.\n\n";
    std::cerr << kUsage;
    return EXIT_FAILURE;
}
