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
  sign-tx         Build, hybrid-sign, and emit a Payment transaction.
  submit-tx       Submit a previously signed tx_blob to a hybrid-aware rippled.
  show-account    Display the wallet's on-ledger AccountRoot, including QuantumPubKey.

Common options:
  --wallet <path>     Wallet state file. Defaults to ./pq-wallet.json.
                      Secret material lives in <path>.custody.json (ECC mock)
                      and <path>.pq.json (PQ keystore mock); the wallet binary
                      never opens those files directly.
  --rpc-url <url>     Hybrid-aware rippled JSON-RPC endpoint.
                      Defaults to http://127.0.0.1:5050 (the standalone-mode
                      demo at scripts/demo/). For a multi-validator DevNet
                      pass --rpc-url http://127.0.0.1:5005.
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
        if (command == "sign-tx")
            return pqwallet::cmd::signTx(argc - 2, argv + 2);
        if (command == "submit-tx")
            return pqwallet::cmd::submitTx(argc - 2, argv + 2);
        if (command == "show-account")
            return pqwallet::cmd::showAccount(argc - 2, argv + 2);
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
