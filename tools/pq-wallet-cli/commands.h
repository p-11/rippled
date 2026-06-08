#pragma once

namespace pqwallet::cmd {

// Each subcommand takes the argv tail (with argv[0] already being the
// subcommand name) and returns the process exit code.

int
keygen(int argc, char** argv);

}  // namespace pqwallet::cmd
