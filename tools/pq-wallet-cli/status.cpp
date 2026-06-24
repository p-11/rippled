#include <xrpl/json/json_value.h>

#include <commands.h>
#include <defaults.h>
#include <rpc_client.h>

#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace pqwallet::cmd {

namespace {

struct StatusArgs
{
    std::string rpcUrl{defaults::rpcUrl()};
};

StatusArgs
parseStatusArgs(int argc, char** argv)
{
    StatusArgs out;
    for (int i = 0; i < argc; ++i)
    {
        std::string_view const a = argv[i];
        if (a == "--rpc-url" && i + 1 < argc && argv[i + 1][0] != '-')
        {
            out.rpcUrl = argv[++i];
        }
        else
        {
            throw std::runtime_error(
                "status: unknown or incomplete argument '" + std::string(a) + "'");
        }
    }
    return out;
}

}  // namespace

int
status(int argc, char** argv)
{
    auto const args = parseStatusArgs(argc, argv);

    auto const result = rpc::call(args.rpcUrl, "server_info", json::Value(json::ValueType::Object));
    auto const info = result.get("info", json::Value(json::ValueType::Object));

    auto const serverState = info.get("server_state", "").asString();
    auto const completeLedgers = info.get("complete_ledgers", "").asString();
    auto const peers = info.get("peers", 0u).asUInt();
    auto const buildVersion = info.get("build_version", "").asString();
    auto const validated = info.get("validated_ledger", json::Value(json::ValueType::Object));
    auto const validatedSeq = validated.get("seq", 0u).asUInt();

    std::cout << "server status:\n";
    std::cout << "  rpc_url          : " << args.rpcUrl << '\n';
    std::cout << "  server_state     : " << serverState << '\n';
    std::cout << "  validated_ledger : " << validatedSeq << '\n';
    std::cout << "  complete_ledgers : " << completeLedgers << '\n';
    std::cout << "  peers            : " << peers << '\n';
    std::cout << "  build_version    : " << buildVersion << '\n';
    if (info.get("amendment_blocked", false).asBool())
        std::cout << "  amendment_blocked: YES (node needs an upgrade)\n";

    // server_state "full" or "proposing" means the node is synced and usable.
    return (serverState == "full" || serverState == "proposing" || serverState == "validating")
        ? EXIT_SUCCESS
        : EXIT_FAILURE;
}

}  // namespace pqwallet::cmd
