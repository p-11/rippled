#include <pq_custody_mock.h>

#include <xrpl/basics/StringUtilities.h>
#include <xrpl/basics/strHex.h>
#include <xrpl/json/json_reader.h>
#include <xrpl/json/json_value.h>
#include <xrpl/json/json_writer.h>
#include <xrpl/protocol/PQSign.h>

#include <cstdlib>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>

namespace pqwallet::custody {

namespace {

constexpr char const* kAlgorithmField = "algorithm";
constexpr char const* kAlgorithmValue = "ML-DSA-44";
constexpr char const* kPqSeedField = "pq_seed_hex";
constexpr char const* kPublicKeyField = "public_key_hex";

bool
pqFailRequested()
{
    char const* env = std::getenv("PQ_CUSTODY_FAIL");
    return env != nullptr && std::string_view{env} == "1";
}

}  // namespace

PqCustodyMock::PqCustodyMock(std::filesystem::path stateFile) : stateFile_(std::move(stateFile))
{
}

bool
PqCustodyMock::exists() const
{
    return std::filesystem::exists(stateFile_);
}

void
PqCustodyMock::initializeFromPqSeed(xrpl::Slice seed)
{
    if (seed.size() != xrpl::kPQSeedSize)
        throw std::runtime_error(
            "PQ seed must be exactly " + std::to_string(xrpl::kPQSeedSize) + " bytes");

    auto [pub, sec] = xrpl::pqKeypair(seed);
    pqSeed_ = xrpl::Buffer(seed.data(), seed.size());
    publicKey_ = std::move(pub);
    secretKey_ = std::move(sec);
    persist();
}

void
PqCustodyMock::load()
{
    std::ifstream in(stateFile_);
    if (!in)
    {
        std::ostringstream msg;
        msg << "PQ custody state file '" << stateFile_.string()
            << "' is missing. Run 'keygen' first.";
        throw std::runtime_error(msg.str());
    }

    std::stringstream ss;
    ss << in.rdbuf();
    json::Value root;
    json::Reader reader;
    if (!reader.parse(ss.str(), root))
        throw std::runtime_error(
            "PQ custody state file is not valid JSON: " + reader.getFormattedErrorMessages());

    auto const algo = root.get(kAlgorithmField, "").asString();
    if (algo != kAlgorithmValue)
        throw std::runtime_error("PQ custody has unsupported algorithm: '" + algo + "'");

    auto const seedHex = root.get(kPqSeedField, "").asString();
    if (seedHex.empty())
        throw std::runtime_error("PQ custody is missing pq_seed_hex");

    auto const seedBytes = xrpl::strUnHex(seedHex);
    if (!seedBytes)
        throw std::runtime_error("PQ custody pq_seed_hex is malformed");
    initializeFromPqSeed(xrpl::Slice(seedBytes->data(), seedBytes->size()));
}

xrpl::Buffer
PqCustodyMock::publicKey() const
{
    if (!publicKey_)
        throw std::runtime_error(
            "PQ custody is uninitialized; call load() or "
            "initializeFromPqSeed() first");
    return *publicKey_;
}

xrpl::Buffer
PqCustodyMock::signWithPq(xrpl::Slice payload) const
{
    if (!secretKey_)
        throw std::runtime_error(
            "PQ custody is uninitialized; call load() or "
            "initializeFromPqSeed() first");

    auto sig = xrpl::pqSign(xrpl::Slice(secretKey_->data(), secretKey_->size()), payload);

    if (pqFailRequested())
    {
        sig.data()[0] ^= 0xFFu;
        return sig;
    }
    return sig;
}

std::filesystem::path const&
PqCustodyMock::stateFile() const noexcept
{
    return stateFile_;
}

void
PqCustodyMock::persist() const
{
    if (!pqSeed_ || !publicKey_)
        throw std::runtime_error("PqCustodyMock::persist called before keys were initialized");

    json::Value root(json::ValueType::Object);
    root[kAlgorithmField] = kAlgorithmValue;
    root[kPqSeedField] = xrpl::strHex(*pqSeed_);
    root[kPublicKeyField] = xrpl::strHex(*publicKey_);

    auto const dir = stateFile_.parent_path();
    if (!dir.empty())
        std::filesystem::create_directories(dir);

    std::ofstream out(stateFile_);
    if (!out)
        throw std::runtime_error(
            "Cannot open PQ custody state file for writing: " + stateFile_.string());

    json::StyledWriter writer;
    out << writer.write(root);
    if (!out)
        throw std::runtime_error("Failed to write PQ custody state file: " + stateFile_.string());
}

}  // namespace pqwallet::custody
