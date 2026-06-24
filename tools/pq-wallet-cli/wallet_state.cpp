#include <wallet_state.h>

#include <xrpl/json/json_reader.h>
#include <xrpl/json/json_value.h>
#include <xrpl/json/json_writer.h>

#include <fstream>
#include <sstream>
#include <stdexcept>

namespace pqwallet::state {

namespace {

constexpr char const* kAccountIdField = "account_id";
constexpr char const* kKeyTypeField = "key_type";
constexpr char const* kEccPublicKeyField = "ecc_public_key_hex";
constexpr char const* kAlgorithmField = "algorithm";
constexpr char const* kPqPublicKeyField = "pq_public_key_hex";

std::filesystem::path
siblingStateFile(std::filesystem::path const& walletStateFile, std::string const& suffix)
{
    auto const dir = walletStateFile.parent_path();
    auto const stem = walletStateFile.stem().string();
    auto const sibling = stem + suffix;
    return dir.empty() ? std::filesystem::path(sibling) : dir / sibling;
}

}  // namespace

std::filesystem::path
custodyStateFileFor(std::filesystem::path const& walletStateFile)
{
    return siblingStateFile(walletStateFile, ".ecc-custody.json");
}

std::filesystem::path
pqStateFileFor(std::filesystem::path const& walletStateFile)
{
    return siblingStateFile(walletStateFile, ".pq-custody.json");
}

bool
exists(std::filesystem::path const& walletStateFile)
{
    return std::filesystem::exists(walletStateFile);
}

void
save(std::filesystem::path const& walletStateFile, WalletState const& s)
{
    json::Value root(json::ValueType::Object);
    root[kAccountIdField] = s.accountId;
    root[kKeyTypeField] = s.keyType;
    root[kEccPublicKeyField] = s.eccPublicKeyHex;
    root[kAlgorithmField] = s.algorithm;
    root[kPqPublicKeyField] = s.pqPublicKeyHex;

    auto const dir = walletStateFile.parent_path();
    if (!dir.empty())
        std::filesystem::create_directories(dir);

    std::ofstream out(walletStateFile);
    if (!out)
        throw std::runtime_error(
            "Cannot open wallet state file for writing: " + walletStateFile.string());

    json::StyledWriter writer;
    out << writer.write(root);
    if (!out)
        throw std::runtime_error("Failed to write wallet state file: " + walletStateFile.string());
}

WalletState
load(std::filesystem::path const& walletStateFile)
{
    std::ifstream in(walletStateFile);
    if (!in)
    {
        std::ostringstream msg;
        msg << "Wallet state file '" << walletStateFile.string()
            << "' is missing. Run 'keygen' first.";
        throw std::runtime_error(msg.str());
    }

    std::stringstream ss;
    ss << in.rdbuf();
    json::Value root;
    json::Reader reader;
    if (!reader.parse(ss.str(), root))
        throw std::runtime_error(
            "Wallet state file is not valid JSON: " + reader.getFormattedErrorMessages());

    WalletState s;
    s.accountId = root.get(kAccountIdField, "").asString();
    s.keyType = root.get(kKeyTypeField, "").asString();
    s.eccPublicKeyHex = root.get(kEccPublicKeyField, "").asString();
    s.algorithm = root.get(kAlgorithmField, "").asString();
    s.pqPublicKeyHex = root.get(kPqPublicKeyField, "").asString();

    // The PQ fields are optional: an ECC-only wallet (keygen without --quantum)
    // leaves them empty. Only the ECC identity is required.
    if (s.accountId.empty() || s.eccPublicKeyHex.empty())
        throw std::runtime_error("Wallet state file is missing required fields");

    return s;
}

}  // namespace pqwallet::state
