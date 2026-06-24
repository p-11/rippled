#pragma once

#include <cctype>
#include <cstdint>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string>

namespace pqwallet::args {

// Strict u32 parse for CLI flag values: rejects empty or whitespace input,
// partial parses, negative numbers, and anything above UINT32_MAX. `flag`
// is the flag name used in the error message.
inline std::uint32_t
parseU32(char const* arg, char const* flag)
{
    try
    {
        if (!arg || arg[0] == '\0' || std::isspace(static_cast<unsigned char>(arg[0])))
            throw std::invalid_argument{""};
        std::size_t pos = 0;
        auto const v = std::stoull(arg, &pos);
        if (pos != std::strlen(arg))
            throw std::invalid_argument{""};
        if (v > std::numeric_limits<std::uint32_t>::max())
            throw std::out_of_range{""};
        return static_cast<std::uint32_t>(v);
    }
    catch (std::exception const&)
    {
        throw std::runtime_error(
            std::string{flag} + " requires a non-negative u32; got '" + std::string{arg} + "'");
    }
}

}  // namespace pqwallet::args
