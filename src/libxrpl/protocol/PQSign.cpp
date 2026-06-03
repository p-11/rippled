#include <xrpl/protocol/PQSign.h>

#include <xrpl/basics/Buffer.h>
#include <xrpl/basics/Slice.h>
#include <xrpl/protocol/detail/mldsa.h>

namespace xrpl {

Buffer
pqSign(Slice secretKey, Slice msg)
{
    // mldsa::sign takes (msg, secretKey); the cross is intentional, the API
    // order matches xrpl::sign rather than the underlying primitive.
    return mldsa::sign(msg, secretKey);
}

bool
pqVerify(Slice publicKey, Slice msg, Slice sig) noexcept
{
    // mldsa::verify takes (sig, msg, publicKey); the cross is intentional,
    // the API order matches xrpl::verify rather than the underlying primitive.
    return mldsa::verify(sig, msg, publicKey);
}

}  // namespace xrpl
