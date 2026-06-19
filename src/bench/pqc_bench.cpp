// Crypto microbench for the hybrid PQC PoC.
//
// Reports sign / verify cost for the three signature algorithms on the
// hybrid path (secp256k1, Ed25519, ML-DSA-44). ML-DSA-44 runs under whichever
// backend the binary was built with: rebuild with `-o mldsa_avx2=True` for the
// AVX2 column.
//
// Before the timed cases it prints a deterministic, byte-exact size table
// (signature / public-key sizes and the per-signer hybrid serialization delta)
// to stdout as JSON, so metrics 1-3 and 8 of the benchmark plan land in the
// same artifact as the timing numbers.

#include <xrpl/basics/Buffer.h>
#include <xrpl/basics/Slice.h>
#include <xrpl/protocol/KeyType.h>
#include <xrpl/protocol/PQSign.h>
#include <xrpl/protocol/PublicKey.h>
#include <xrpl/protocol/SField.h>
#include <xrpl/protocol/STObject.h>
#include <xrpl/protocol/SecretKey.h>
#include <xrpl/protocol/Serializer.h>
#include <xrpl/protocol/digest.h>

#include <benchmark/benchmark.h>

#include <array>
#include <cstdint>
#include <iostream>
#include <vector>

using namespace xrpl;

namespace {

std::vector<std::uint8_t>
sampleMessage()
{
    // A fixed 64-byte message stands in for a transaction signing payload.
    std::vector<std::uint8_t> msg(64);
    for (std::size_t i = 0; i < msg.size(); ++i)
        msg[i] = static_cast<std::uint8_t>(i * 7 + 3);
    return msg;
}

Buffer
fixedPqSeed()
{
    Buffer seed(kPQSeedSize);
    for (std::size_t i = 0; i < kPQSeedSize; ++i)
        reinterpret_cast<std::uint8_t*>(seed.data())[i] = static_cast<std::uint8_t>(i + 1);
    return seed;
}

}  // namespace

static void
BM_secp256k1_sign(benchmark::State& state)
{
    auto const [pk, sk] = randomKeyPair(KeyType::Secp256k1);
    auto const msg = sampleMessage();
    uint256 const digest = sha512Half(makeSlice(msg));
    for (auto _ : state)
    {
        auto sig = signDigest(pk, sk, digest);
        benchmark::DoNotOptimize(sig.data());
    }
}
BENCHMARK(BM_secp256k1_sign);

static void
BM_secp256k1_verify(benchmark::State& state)
{
    auto const [pk, sk] = randomKeyPair(KeyType::Secp256k1);
    auto const msg = sampleMessage();
    uint256 const digest = sha512Half(makeSlice(msg));
    auto const sig = signDigest(pk, sk, digest);
    for (auto _ : state)
    {
        bool ok = verifyDigest(pk, digest, Slice(sig));
        benchmark::DoNotOptimize(ok);
    }
}
BENCHMARK(BM_secp256k1_verify);

static void
BM_ed25519_sign(benchmark::State& state)
{
    auto const [pk, sk] = randomKeyPair(KeyType::Ed25519);
    auto const msg = sampleMessage();
    for (auto _ : state)
    {
        auto sig = sign(pk, sk, makeSlice(msg));
        benchmark::DoNotOptimize(sig.data());
    }
}
BENCHMARK(BM_ed25519_sign);

static void
BM_ed25519_verify(benchmark::State& state)
{
    auto const [pk, sk] = randomKeyPair(KeyType::Ed25519);
    auto const msg = sampleMessage();
    auto const sig = sign(pk, sk, makeSlice(msg));
    for (auto _ : state)
    {
        bool ok = verify(pk, makeSlice(msg), Slice(sig));
        benchmark::DoNotOptimize(ok);
    }
}
BENCHMARK(BM_ed25519_verify);

static void
BM_mldsa44_sign(benchmark::State& state)
{
    auto const seed = fixedPqSeed();
    auto const [pub, sec] = pqKeypair(Slice(seed));
    auto const msg = sampleMessage();
    for (auto _ : state)
    {
        auto sig = pqSign(Slice(sec), Slice(msg.data(), msg.size()));
        benchmark::DoNotOptimize(sig.data());
    }
}
BENCHMARK(BM_mldsa44_sign);

static void
BM_mldsa44_verify(benchmark::State& state)
{
    auto const seed = fixedPqSeed();
    auto const [pub, sec] = pqKeypair(Slice(seed));
    auto const msg = sampleMessage();
    auto const sig = pqSign(Slice(sec), Slice(msg.data(), msg.size()));
    for (auto _ : state)
    {
        bool ok = pqVerify(Slice(pub), Slice(msg.data(), msg.size()), Slice(sig));
        benchmark::DoNotOptimize(ok);
    }
}
BENCHMARK(BM_mldsa44_verify);

namespace {

// Exact serialized byte cost of the two per-signer PQ fields. An empty
// STObject(sfGeneric) serializes to zero bytes, so this is precisely the
// overhead the hybrid path adds per signer (field id + length prefix + payload
// for sfQuantumPubKey and sfQuantumSignature).
std::size_t
perSignerHybridDelta()
{
    auto const seed = fixedPqSeed();
    auto const [pub, sec] = pqKeypair(Slice(seed));
    auto const msg = sampleMessage();
    auto const sig = pqSign(Slice(sec), Slice(msg.data(), msg.size()));

    STObject st(sfGeneric);
    st.setFieldVL(sfQuantumPubKey, Slice(pub));
    st.setFieldVL(sfQuantumSignature, Slice(sig));
    Serializer s;
    st.add(s);
    return s.size();
}

void
printSizeTable()
{
    auto const [secpPk, secpSk] = randomKeyPair(KeyType::Secp256k1);
    auto const [edPk, edSk] = randomKeyPair(KeyType::Ed25519);
    auto const msg = sampleMessage();
    uint256 const digest = sha512Half(makeSlice(msg));
    auto const secpSig = signDigest(secpPk, secpSk, digest);
    auto const edSig = sign(edPk, edSk, makeSlice(msg));

    std::size_t const perSigner = perSignerHybridDelta();

    std::cout << "{\n";
    std::cout << "  \"size_table\": {\n";
    std::cout << "    \"secp256k1\": {\"pubkey_bytes\": " << secpPk.size()
              << ", \"signature_bytes\": " << secpSig.size() << "},\n";
    std::cout << "    \"ed25519\": {\"pubkey_bytes\": " << edPk.size()
              << ", \"signature_bytes\": " << edSig.size() << "},\n";
    std::cout << "    \"mldsa44\": {\"pubkey_bytes\": " << kPQPublicKeySize
              << ", \"signature_bytes\": " << kPQSignatureSize
              << ", \"secret_bytes\": " << kPQSecretKeySize << ", \"seed_bytes\": " << kPQSeedSize
              << "},\n";
    std::cout << "    \"single_sign_payload_delta_bytes\": " << perSigner << ",\n";
    std::cout << "    \"multi_sign_per_signer_delta_bytes\": " << perSigner << ",\n";
    std::cout << "    \"multi_sign_payload_delta_bytes\": {";
    bool first = true;
    for (int n : {1, 4, 8, 16, 32})
    {
        std::cout << (first ? "" : ", ") << "\"" << n << "\": " << perSigner * n;
        first = false;
    }
    std::cout << "}\n";
    std::cout << "  }\n";
    std::cout << "}\n";
    std::cout.flush();
}

}  // namespace

int
main(int argc, char** argv)
{
    printSizeTable();
    benchmark::Initialize(&argc, argv);
    benchmark::RunSpecifiedBenchmarks();
    benchmark::Shutdown();
    return 0;
}
