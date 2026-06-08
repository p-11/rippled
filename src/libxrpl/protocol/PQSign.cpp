#include <xrpl/protocol/PQSign.h>

#include <xrpl/basics/Buffer.h>
#include <xrpl/basics/Slice.h>
#include <xrpl/protocol/HashPrefix.h>
#include <xrpl/protocol/SField.h>
#include <xrpl/protocol/STObject.h>
#include <xrpl/protocol/Serializer.h>
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

void
pqSign(STObject& st, HashPrefix const& prefix, Slice secretKey, SF_VL const& sigField)
{
    Serializer ss;
    ss.add32(prefix);
    st.addWithoutSigningFields(ss);
    auto const sig = pqSign(secretKey, ss.slice());
    st.setFieldVL(sigField, Slice(sig.data(), sig.size()));
}

bool
pqVerify(STObject const& st, HashPrefix const& prefix, Slice publicKey, SF_VL const& sigField)
{
    if (!st.isFieldPresent(sigField))
        return false;
    auto const sig = st.getFieldVL(sigField);
    Serializer ss;
    ss.add32(prefix);
    st.addWithoutSigningFields(ss);
    return pqVerify(publicKey, ss.slice(), Slice(sig.data(), sig.size()));
}

}  // namespace xrpl
