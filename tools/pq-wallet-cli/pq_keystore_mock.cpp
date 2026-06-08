#include <pq_keystore_mock.h>

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
#include <vector>

namespace pqwallet::pqstore {

namespace {

constexpr char const* kAlgorithmField = "algorithm";
constexpr char const* kAlgorithmValue = "ML-DSA-44";
constexpr char const* kPqSeedField = "pq_seed_hex";
constexpr char const* kPublicKeyField = "public_key_hex";

bool
pqFailRequested()
{
    char const* env = std::getenv("PQ_KEYSTORE_FAIL");
    return env != nullptr && std::string_view{env} == "1";
}

}  // namespace

PqKeystoreMock::PqKeystoreMock(std::filesystem::path stateFile) : stateFile_(std::move(stateFile))
{
}

bool
PqKeystoreMock::exists() const
{
    return std::filesystem::exists(stateFile_);
}

void
PqKeystoreMock::initializeFromPqSeed(xrpl::Slice seed)
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
PqKeystoreMock::load()
{
    std::ifstream in(stateFile_);
    if (!in)
    {
        std::ostringstream msg;
        msg << "PQ keystore state file '" << stateFile_.string()
            << "' is missing. Run 'keygen' first.";
        throw std::runtime_error(msg.str());
    }

    std::stringstream ss;
    ss << in.rdbuf();
    json::Value root;
    json::Reader reader;
    if (!reader.parse(ss.str(), root))
        throw std::runtime_error(
            "PQ keystore state file is not valid JSON: " + reader.getFormattedErrorMessages());

    auto const algo = root.get(kAlgorithmField, "").asString();
    if (algo != kAlgorithmValue)
        throw std::runtime_error("PQ keystore has unsupported algorithm: '" + algo + "'");

    auto const seedHex = root.get(kPqSeedField, "").asString();
    if (seedHex.empty())
        throw std::runtime_error("PQ keystore is missing pq_seed_hex");

    auto const seedBytes = xrpl::strUnHex(seedHex);
    if (!seedBytes)
        throw std::runtime_error("PQ keystore pq_seed_hex is malformed");
    initializeFromPqSeed(xrpl::Slice(seedBytes->data(), seedBytes->size()));
}

xrpl::Buffer
PqKeystoreMock::publicKey() const
{
    if (!publicKey_)
        throw std::runtime_error(
            "PQ keystore is uninitialized; call load() or "
            "initializeFromPqSeed() first");
    return *publicKey_;
}

xrpl::Buffer
PqKeystoreMock::signWithPq(xrpl::Slice payload) const
{
    if (!secretKey_)
        throw std::runtime_error(
            "PQ keystore is uninitialized; call load() or "
            "initializeFromPqSeed() first");

    auto sig = xrpl::pqSign(xrpl::Slice(secretKey_->data(), secretKey_->size()), payload);

    if (pqFailRequested())
    {
        std::vector<std::uint8_t> corrupted(sig.data(), sig.data() + sig.size());
        corrupted[0] ^= 0xFFu;
        return xrpl::Buffer(corrupted.data(), corrupted.size());
    }
    return sig;
}

std::filesystem::path const&
PqKeystoreMock::stateFile() const noexcept
{
    return stateFile_;
}

void
PqKeystoreMock::persist() const
{
    if (!pqSeed_ || !publicKey_)
        throw std::runtime_error("PqKeystoreMock::persist called before keys were initialized");

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
            "Cannot open PQ keystore state file for writing: " + stateFile_.string());

    json::StyledWriter writer;
    out << writer.write(root);
    if (!out)
        throw std::runtime_error("Failed to write PQ keystore state file: " + stateFile_.string());
}

}  // namespace pqwallet::pqstore
