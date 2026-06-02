#include <xrpl/protocol/detail/mldsa.h>

#include <xrpl/basics/Blob.h>
#include <xrpl/basics/Buffer.h>
#include <xrpl/basics/Slice.h>
#include <xrpl/basics/StringUtilities.h>
#include <xrpl/json/json_reader.h>
#include <xrpl/json/json_value.h>

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <fstream>
#include <sstream>
#include <string>

namespace {

xrpl::Blob
unhex(std::string const& hex)
{
    auto const decoded = xrpl::strUnHex(hex);
    if (!decoded)
        throw std::runtime_error("invalid hex in ACVP fixture: " + hex.substr(0, 32) + "...");
    return *decoded;
}

json::Value
loadFixture()
{
    std::ifstream in(XRPL_MLDSA_ACVP_FIXTURE_PATH);
    if (!in)
        throw std::runtime_error("cannot open ACVP fixture at " XRPL_MLDSA_ACVP_FIXTURE_PATH);

    std::stringstream buf;
    buf << in.rdbuf();

    json::Reader reader;
    json::Value root;
    if (!reader.parse(buf.str(), root))
        throw std::runtime_error(
            "failed to parse ACVP fixture: " + reader.getFormattedErrorMessages());

    return root;
}

}  // namespace

TEST(mldsa, SigVer_ACVP_KAT)
{
    auto const root = loadFixture();
    ASSERT_EQ(root["algorithm"].asString(), "ML-DSA");
    ASSERT_EQ(root["mode"].asString(), "sigVer");
    ASSERT_EQ(root["parameterSet"].asString(), "ML-DSA-44");
    ASSERT_EQ(root["signatureInterface"].asString(), "external");
    ASSERT_EQ(root["preHash"].asString(), "pure");

    auto const& tests = root["tests"];
    ASSERT_TRUE(tests.isArray());
    ASSERT_GT(tests.size(), 0u);

    std::size_t positive = 0;
    std::size_t negative = 0;

    for (auto const& tc : tests)
    {
        auto const tcId = tc["tcId"].asInt();
        auto const pk = unhex(tc["pk"].asString());
        auto const msg = unhex(tc["message"].asString());
        auto const ctx = unhex(tc["context"].asString());
        auto const sig = unhex(tc["signature"].asString());
        auto const expected = tc["testPassed"].asBool();

        auto const actual = xrpl::mldsa::verify(
            xrpl::makeSlice(sig), xrpl::makeSlice(msg), xrpl::makeSlice(pk), xrpl::makeSlice(ctx));

        EXPECT_EQ(actual, expected)
            << "ACVP tcId " << tcId << ": expected verify=" << expected << ", got " << actual;

        if (expected)
            ++positive;
        else
            ++negative;
    }

    EXPECT_GT(positive, 0u) << "fixture has no positive vectors";
    EXPECT_GT(negative, 0u) << "fixture has no negative vectors";
}

TEST(mldsa, Roundtrip)
{
    auto const [pk, sk] = xrpl::mldsa::keypair();
    ASSERT_EQ(pk.size(), xrpl::mldsa::kPublicKeySize);
    ASSERT_EQ(sk.size(), xrpl::mldsa::kSecretKeySize);

    std::string const msg = "xrpld hybrid post-quantum signing smoke test";
    auto const sig = xrpl::mldsa::sign(xrpl::makeSlice(msg), sk);
    ASSERT_EQ(sig.size(), xrpl::mldsa::kSignatureSize);

    EXPECT_TRUE(xrpl::mldsa::verify(sig, xrpl::makeSlice(msg), pk));
}

TEST(mldsa, RoundtripWithContext)
{
    auto const [pk, sk] = xrpl::mldsa::keypair();
    std::string const msg = "xrpld hybrid post-quantum signing smoke test";
    std::string const ctx = "xrpl-mldsa-test-context";

    auto const sig = xrpl::mldsa::sign(xrpl::makeSlice(msg), sk, xrpl::makeSlice(ctx));
    ASSERT_EQ(sig.size(), xrpl::mldsa::kSignatureSize);

    EXPECT_TRUE(xrpl::mldsa::verify(sig, xrpl::makeSlice(msg), pk, xrpl::makeSlice(ctx)));

    std::string const otherCtx = "xrpl-mldsa-other-context";
    EXPECT_FALSE(xrpl::mldsa::verify(sig, xrpl::makeSlice(msg), pk, xrpl::makeSlice(otherCtx)))
        << "verify must reject when context differs from the one used at sign";

    EXPECT_FALSE(xrpl::mldsa::verify(sig, xrpl::makeSlice(msg), pk))
        << "verify must reject when no context is provided against a signature "
           "that was signed with a context";
}
