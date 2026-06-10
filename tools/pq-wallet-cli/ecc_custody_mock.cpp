#include <ecc_custody_mock.h>

#include <xrpl/basics/StringUtilities.h>
#include <xrpl/basics/strHex.h>
#include <xrpl/json/json_reader.h>
#include <xrpl/json/json_value.h>
#include <xrpl/json/json_writer.h>

#include <cstdlib>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace pqwallet::custody {

namespace {

constexpr char const* kKeyTypeField = "key_type";
constexpr char const* kSecretKeyField = "secret_key_hex";
constexpr char const* kPublicKeyField = "public_key_hex";

bool
custodyFailRequested()
{
    char const* env = std::getenv("RIPPLE_CUSTODY_FAIL");
    return env != nullptr && std::string_view{env} == "1";
}

}  // namespace

EccCustodyMock::EccCustodyMock(std::filesystem::path stateFile) : stateFile_(std::move(stateFile))
{
}

bool
EccCustodyMock::exists() const
{
    return std::filesystem::exists(stateFile_);
}

void
EccCustodyMock::initializeFromSeed(xrpl::Seed const& seed, xrpl::KeyType type)
{
    auto [pk, sk] = xrpl::generateKeyPair(type, seed);
    keyType_ = type;
    publicKey_ = pk;
    secretKey_ = sk;
    persist();
}

void
EccCustodyMock::load()
{
    std::ifstream in(stateFile_);
    if (!in)
    {
        std::ostringstream msg;
        msg << "ECC custody state file '" << stateFile_.string()
            << "' is missing. Run 'keygen' first.";
        throw std::runtime_error(msg.str());
    }

    std::stringstream ss;
    ss << in.rdbuf();
    json::Value root;
    json::Reader reader;
    if (!reader.parse(ss.str(), root))
    {
        throw std::runtime_error(
            "ECC custody state file is not valid JSON: " + reader.getFormattedErrorMessages());
    }

    auto const typeStr = root.get(kKeyTypeField, "").asString();
    auto const type = xrpl::keyTypeFromString(typeStr);
    if (!type)
        throw std::runtime_error("ECC custody state has unsupported key_type: '" + typeStr + "'");

    auto const secretHex = root.get(kSecretKeyField, "").asString();
    auto const publicHex = root.get(kPublicKeyField, "").asString();
    if (secretHex.empty() || publicHex.empty())
        throw std::runtime_error("ECC custody state is missing secret_key_hex or public_key_hex");

    auto const secretBytes = xrpl::strUnHex(secretHex);
    if (!secretBytes || secretBytes->size() != xrpl::SecretKey::kSize)
        throw std::runtime_error("ECC custody secret_key_hex is malformed");

    auto const publicBytes = xrpl::strUnHex(publicHex);
    if (!publicBytes)
        throw std::runtime_error("ECC custody public_key_hex is malformed");
    keyType_ = *type;
    secretKey_ = xrpl::SecretKey(xrpl::Slice(secretBytes->data(), secretBytes->size()));
    publicKey_ = xrpl::PublicKey(xrpl::Slice(publicBytes->data(), publicBytes->size()));
}

xrpl::PublicKey
EccCustodyMock::publicKey() const
{
    if (!publicKey_)
        throw std::runtime_error(
            "ECC custody mock is uninitialized; call load() or "
            "initializeFromSeed() first");
    return *publicKey_;
}

xrpl::KeyType
EccCustodyMock::keyType() const
{
    return keyType_;
}

xrpl::Buffer
EccCustodyMock::signWithECC(xrpl::Slice payload) const
{
    if (!secretKey_ || !publicKey_)
        throw std::runtime_error(
            "ECC custody mock is uninitialized; call load() or "
            "initializeFromSeed() first");

    auto sig = xrpl::sign(*publicKey_, *secretKey_, payload);

    if (custodyFailRequested())
    {
        // Corrupt the first byte so the signature is no longer valid DER;
        // run-demo.sh uses this for its negative path.
        std::vector<std::uint8_t> corrupted(sig.data(), sig.data() + sig.size());
        corrupted[0] ^= 0xFFu;
        return xrpl::Buffer(corrupted.data(), corrupted.size());
    }
    return sig;
}

std::filesystem::path const&
EccCustodyMock::stateFile() const noexcept
{
    return stateFile_;
}

void
EccCustodyMock::persist() const
{
    if (!secretKey_ || !publicKey_)
        throw std::runtime_error("EccCustodyMock::persist called before keys were initialized");

    json::Value root(json::ValueType::Object);
    root[kKeyTypeField] = xrpl::to_string(keyType_);
    root[kSecretKeyField] = xrpl::strHex(*secretKey_);
    root[kPublicKeyField] = xrpl::strHex(*publicKey_);

    auto const dir = stateFile_.parent_path();
    if (!dir.empty())
        std::filesystem::create_directories(dir);

    std::ofstream out(stateFile_);
    if (!out)
        throw std::runtime_error(
            "Cannot open ECC custody state file for writing: " + stateFile_.string());

    json::StyledWriter writer;
    out << writer.write(root);
    if (!out)
        throw std::runtime_error("Failed to write ECC custody state file: " + stateFile_.string());
}

}  // namespace pqwallet::custody
