add_executable(
    pq-wallet-cli
    main.cpp
    commands.cpp
    sign_tx.cpp
    submit_show.cpp
    opt_in.cpp
    rpc_client.cpp
    ecc_custody_mock.cpp
    pq_custody_mock.cpp
    wallet_state.cpp
)

# The clang-format hook rewrites local includes from "x.h" to <x.h> across the
# rippled tree. Listing the tool's own directory on the include path keeps the
# angle-bracket form resolvable for headers that sit next to main.cpp.
target_include_directories(pq-wallet-cli PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(
    pq-wallet-cli
    PRIVATE Xrpl::opts Xrpl::libs Xrpl::boost xrpl.libxrpl
)

set_target_properties(
    pq-wallet-cli
    PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
)

install(TARGETS pq-wallet-cli RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
