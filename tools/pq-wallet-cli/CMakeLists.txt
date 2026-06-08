add_executable(pq-wallet-cli main.cpp)

target_link_libraries(
    pq-wallet-cli
    PRIVATE Xrpl::opts Xrpl::libs Xrpl::boost xrpl.libxrpl
)

set_target_properties(
    pq-wallet-cli
    PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
)

install(TARGETS pq-wallet-cli RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
