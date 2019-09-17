#include "wallet.h"

#include "../wallet/wallet.h"
#include "../wallet/walletdb.h"

#include <boost/function_output_iterator.hpp>

#include <functional>

namespace exodus {

Wallet *wallet;

Wallet::Wallet(const std::string& walletFile, SigmaDatabase& sigmaDb)
    : walletFile(walletFile), mintWallet(walletFile)
{
    using std::placeholders::_1;
    using std::placeholders::_2;
    using std::placeholders::_3;
    using std::placeholders::_4;
    using std::placeholders::_5;
    using std::placeholders::_6;

    // Subscribe to events.
    {
        auto h = std::bind(&Wallet::OnMintAdded, this, _1, _2, _3, _4, _5, _6);
        eventConnections.emplace_front(sigmaDb.MintAdded.connect(h));
    }
    {
        auto h = std::bind(&Wallet::OnMintRemoved, this, _1, _2, _3);
        eventConnections.emplace_front(sigmaDb.MintRemoved.connect(h));
    }
    {
        auto h = std::bind(&Wallet::OnSpendAdded, this, _1, _2, _3, _4);
        eventConnections.emplace_front(sigmaDb.SpendAdded.connect(h));
    }
    {
        auto h = std::bind(&Wallet::OnSpendRemoved, this, _1, _2, _3);
        eventConnections.emplace_front(sigmaDb.SpendRemoved.connect(h));
    }
}

Wallet::~Wallet()
{
}

SigmaMintId Wallet::CreateSigmaMint(PropertyId property, DenominationId denomination)
{
    SigmaPrivateKey key;
    HDMint mint;
    LOCK(pwalletMain->cs_wallet);
    if (!mintWallet.GenerateMint(property, denomination, key, mint)) {
        throw std::runtime_error("fail to generate mint");
    }

    mintWallet.Record(mint);
    return mint.id;
}

void Wallet::ResetState()
{
    mintWallet.ResetCoinsState();
}

bool Wallet::HasSigmaMint(const SigmaMintId& id)
{
    LOCK(pwalletMain->cs_wallet);
    return mintWallet.HasMint(id);
}

bool Wallet::HasSigmaSpend(const secp_primitives::Scalar& serial, HDMint &mint)
{
    LOCK(pwalletMain->cs_wallet);
    return mintWallet.HasSerial(serial);
}

HDMint Wallet::GetSigmaMint(const SigmaMintId& id)
{
    HDMint mint;

    CWalletDB walletdb(walletFile);
    if (!walletdb.ReadExodusHDMint(id, mint)) {
        throw std::invalid_argument("sigma mint not found");
    }

    return mint;
}

boost::optional<HDMint>
    Wallet::GetSpendableSigmaMint(PropertyId property, DenominationId denomination)
{
    // Get all spendable mints.
    LOCK(pwalletMain->cs_wallet);
    std::vector<HDMint> spendables;
    mintWallet.ListHDMints(std::back_inserter(spendables), true, true); //GetTracker().ListHDMints(std::back_inserter(spendables), true, true);

    auto eraseFrom = std::remove_if(spendables.begin(), spendables.end(), [denomination](HDMint const &mint) -> bool {
        return denomination != mint.id.denomination;
    });
    spendables.erase(eraseFrom, spendables.end());

    if (spendables.empty()) {
        return boost::none;
    }

    // Pick the oldest mint.
    auto oldest = std::min_element(
        spendables.begin(),
        spendables.end(),
        [](const HDMint& a, const HDMint& b) -> bool {

            if (a.chainState.group == b.chainState.group) {
                return a.chainState.index < b.chainState.index;
            }

            return a.chainState.group < b.chainState.group;
        }
    );

    return *oldest;
}

SigmaPrivateKey Wallet::GetKey(const HDMint &mint)
{
    SigmaPrivateKey k;
    if (!mintWallet.RegenerateMint(mint, k)) {
        throw std::runtime_error("fail to regenerate private key");
    }

    if (mint.id.key != SigmaPublicKey(k)) {
        throw std::runtime_error("regenerated key doesn't matched with old value");
    }

    return k;
}

void Wallet::SetSigmaMintUsedTransaction(SigmaMintId const &id, uint256 const &tx)
{
    mintWallet.UpdateMintSpendTx(id, tx);
}

void Wallet::SetSigmaMintChainState(const SigmaMintId& id, const SigmaMintChainState& state)
{
    mintWallet.UpdateMintChainstate(id, state);
}

void Wallet::OnSpendAdded(
    PropertyId property,
    DenominationId denomination,
    const secp_primitives::Scalar &serial,
    const uint256 &tx)
{
    HDMint mint;
    if (!HasSigmaSpend(serial, mint)) {
        // the serial is not in wallet.
        return;
    }

    SigmaMintId id;
    try {
        id = mintWallet.GetMintId(serial);
    } catch (std::runtime_error const &e) {
        LogPrintf("%s : fail to get mint id when spend added have been triggered, %s\n", e.what());
        throw;
    }

    SetSigmaMintUsedTransaction(id, tx);
}

void Wallet::OnSpendRemoved(
    PropertyId property,
    DenominationId denomination,
    const secp_primitives::Scalar &serial)
{
    HDMint mint;
    if (!HasSigmaSpend(serial, mint)) {
        // the serial is not in wallet.
        return;
    }

    try {
        auto id = mintWallet.GetMintId(serial);
        SetSigmaMintUsedTransaction(id, uint256());
    } catch (std::runtime_error const &e) {
        LogPrintf("%s : fail to get mint id when spend removed have been triggered, %s\n", e.what());
        throw;
    }
}

void Wallet::OnMintAdded(
    PropertyId property,
    DenominationId denomination,
    MintGroupId group,
    MintGroupIndex idx,
    const SigmaPublicKey& pubKey,
    int block)
{
    LOCK(pwalletMain->cs_wallet);

    auto pubCoinHash = primitives::GetPubCoinValueHash(pubKey.GetCommitment());
    SigmaMintId id(property, denomination, pubKey);
    auto isMintTracked = HasSigmaMint(id);

    if (isMintTracked) {

        // 1. is tracked then update state
        SetSigmaMintChainState(id, SigmaMintChainState(block, group, idx));
    } else if (mintWallet.GetMintPool().count(pubCoinHash)) {

        // 2. isn't tracked but is in wallet then add it
        auto entry = mintWallet.GetMintPool().at(pubCoinHash);
        mintWallet.SetMintSeedSeen(
            {pubCoinHash, entry}, property, denomination, SigmaMintChainState(block, group, idx));
    }
}

void Wallet::OnMintRemoved(PropertyId property, DenominationId denomination, const SigmaPublicKey& pubKey)
{
    SigmaMintId id(property, denomination, pubKey);

    LOCK(pwalletMain->cs_wallet);

    if (!HasSigmaMint(id)) {
        return;
    }

    SetSigmaMintChainState(id, SigmaMintChainState());
}

}
