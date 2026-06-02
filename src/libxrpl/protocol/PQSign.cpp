#include <xrpl/protocol/PQSign.h>

#include <xrpl/basics/Buffer.h>
#include <xrpl/basics/Slice.h>
#include <xrpl/protocol/detail/mldsa.h>

namespace xrpl {

Buffer
pqSign(Slice msg, Slice secretKey)
{
    return mldsa::sign(msg, secretKey);
}

bool
pqVerify(Slice sig, Slice msg, Slice publicKey) noexcept
{
    return mldsa::verify(sig, msg, publicKey);
}

}  // namespace xrpl
