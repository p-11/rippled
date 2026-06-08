#pragma once

#include <xrpl/json/json_value.h>

#include <cstdint>
#include <string>

namespace pqwallet::rpc {

// Synchronous JSON-RPC POST to a hybrid-aware rippled. Returns the parsed
// `result` object from the JSON-RPC envelope on success. Throws on
// transport failure, HTTP non-2xx, malformed JSON, or a result that does
// not contain "status":"success".
json::Value
call(std::string const& rpcUrl, std::string const& method, json::Value params);

// Fetch the next-sequence number for `accountId` via the `account_info`
// RPC against `rpcUrl`. Used by sign-tx and opt-in when --sequence is
// not supplied explicitly. Throws if the response is not success-shaped
// or if Sequence is missing/zero (the account is not funded).
std::uint32_t
fetchSequence(std::string const& rpcUrl, std::string const& accountId);

}  // namespace pqwallet::rpc
