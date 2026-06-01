#include <xrpl/protocol/detail/mldsa.h>

#include <xrpl/basics/Buffer.h>
#include <xrpl/basics/Slice.h>
#include <xrpl/basics/contract.h>

#include <openssl/rand.h>

#include <mldsa_native.h>

#include <cstddef>
#include <cstdint>
#include <utility>

extern "C" int
randombytes(std::uint8_t* out, std::size_t outlen)
{
    // OpenSSL's RAND_bytes returns 1 on success, 0 or -1 on failure; mldsa-native's
    // randombytes contract is the inverse: 0 on success, non-zero on failure. Translate.
    if (RAND_bytes(reinterpret_cast<unsigned char*>(out), static_cast<int>(outlen)) != 1)
        return -1;
    return 0;
}

namespace xrpl::mldsa {

std::pair<Buffer, Buffer>
keypair()
{
    Buffer pk(publicKeySize);
    Buffer sk(secretKeySize);
    if (MLD_API_NAMESPACE(keypair)(pk.data(), sk.data()) != 0)
        logicError("mldsa::keypair: PQCP_MLDSA_NATIVE_MLDSA44_keypair failed");
    return {std::move(pk), std::move(sk)};
}

Buffer
sign(Slice msg, Slice secretKey)
{
    return sign(msg, secretKey, Slice{});
}

Buffer
sign(Slice msg, Slice secretKey, Slice context)
{
    if (secretKey.size() != secretKeySize)
        logicError("mldsa::sign: secret key has wrong size");

    Buffer sig(signatureSize);
    std::size_t siglen = 0;
    if (MLD_API_NAMESPACE(signature)(
            sig.data(),
            &siglen,
            reinterpret_cast<std::uint8_t const*>(msg.data()),
            msg.size(),
            reinterpret_cast<std::uint8_t const*>(context.data()),
            context.size(),
            reinterpret_cast<std::uint8_t const*>(secretKey.data())) != 0)
        logicError("mldsa::sign: PQCP_MLDSA_NATIVE_MLDSA44_signature failed");

    if (siglen != signatureSize)
        logicError("mldsa::sign: unexpected signature length");

    return sig;
}

bool
verify(Slice sig, Slice msg, Slice publicKey)
{
    return verify(sig, msg, publicKey, Slice{});
}

bool
verify(Slice sig, Slice msg, Slice publicKey, Slice context)
{
    if (sig.size() != signatureSize || publicKey.size() != publicKeySize)
        return false;

    auto const result = MLD_API_NAMESPACE(verify)(
        reinterpret_cast<std::uint8_t const*>(sig.data()),
        sig.size(),
        reinterpret_cast<std::uint8_t const*>(msg.data()),
        msg.size(),
        reinterpret_cast<std::uint8_t const*>(context.data()),
        context.size(),
        reinterpret_cast<std::uint8_t const*>(publicKey.data()));

    return result == 0;
}

}  // namespace xrpl::mldsa
