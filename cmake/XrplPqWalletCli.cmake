option(
    pq_wallet_cli
    "Build the pq-wallet-cli standalone tool that demonstrates the off-chain hybrid ECC + ML-DSA-44 custody signing flow against a hybrid-aware rippled."
    OFF
)

if(pq_wallet_cli)
    add_subdirectory(tools/pq-wallet-cli)
endif()
