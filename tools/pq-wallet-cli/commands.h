#pragma once

namespace pqwallet::cmd {

// Each subcommand takes the argv tail (with argv[0] already being the
// subcommand name) and returns the process exit code.

int
keygen(int argc, char** argv);

int
fund(int argc, char** argv);

int
pay(int argc, char** argv);

int
status(int argc, char** argv);

int
signTx(int argc, char** argv);

int
submitTx(int argc, char** argv);

int
showAccount(int argc, char** argv);

int
optIn(int argc, char** argv);

}  // namespace pqwallet::cmd
