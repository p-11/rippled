#include <xrpl/protocol/PQSign.h>

#include <xrpl/basics/Buffer.h>
#include <xrpl/basics/Slice.h>
#include <xrpl/protocol/detail/mldsa.h>

namespace xrpl {

Buffer
pqSign(Slice secretKey, Slice msg)
{
    return mldsa::sign(msg, secretKey);
}

bool
pqVerify(Slice publicKey, Slice msg, Slice sig) noexcept
{
    return mldsa::verify(sig, msg, publicKey);
}

}  // namespace xrpl
