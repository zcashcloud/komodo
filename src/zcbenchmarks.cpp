#include <unistd.h>
#include <boost/filesystem.hpp>
#include <boost/thread.hpp>
#include "coins.h"
#include "util.h"
#include "init.h"
#include "primitives/transaction.h"
#include "base58.h"
#include "crypto/equihash.h"
#include "chainparams.h"
#include "consensus/validation.h"
#include "main.h"
#include "miner.h"
#include "pow.h"
#include "script/sign.h"
#include "sodium.h"
#include "streams.h"
#include "wallet/wallet.h"

#include "zcbenchmarks.h"

#include "zcash/Zcash.h"
#include "zcash/IncrementalMerkleTree.hpp"

using namespace libzcash;

void timer_start(timeval &tv_start)
{
    gettimeofday(&tv_start, 0);
}

double timer_stop(timeval &tv_start)
{
    double elapsed;
    struct timeval tv_end;
    gettimeofday(&tv_end, 0);
    elapsed = double(tv_end.tv_sec-tv_start.tv_sec) +
        (tv_end.tv_usec-tv_start.tv_usec)/double(1000000);
    return elapsed;
}

double benchmark_sleep()
{
    struct timeval tv_start;
    timer_start(tv_start);
    sleep(1);
    return timer_stop(tv_start);
}

double benchmark_parameter_loading()
{
    // FIXME: this is duplicated with the actual loading code
    boost::filesystem::path pk_path = ZC_GetParamsDir() / "z9-proving.key";
    boost::filesystem::path vk_path = ZC_GetParamsDir() / "z9-verifying.key";

    struct timeval tv_start;
    timer_start(tv_start);

    auto newParams = ZCJoinSplit::Unopened();

    newParams->loadVerifyingKey(vk_path.string());
    newParams->setProvingKeyPath(pk_path.string());
    newParams->loadProvingKey();

    double ret = timer_stop(tv_start);

    delete newParams;

    return ret;
}

double benchmark_create_joinsplit()
{
    uint256 pubKeyHash;

    /* Get the anchor of an empty commitment tree. */
    uint256 anchor = ZCIncrementalMerkleTree().root();

    struct timeval tv_start;
    timer_start(tv_start);
    JSDescription jsdesc(*pzcashParams,
                         pubKeyHash,
                         anchor,
                         {JSInput(), JSInput()},
                         {JSOutput(), JSOutput()},
                         0,
                         0);
    double ret = timer_stop(tv_start);

    assert(jsdesc.Verify(*pzcashParams, pubKeyHash));
    return ret;
}

double benchmark_verify_joinsplit(const JSDescription &joinsplit)
{
    struct timeval tv_start;
    timer_start(tv_start);
    uint256 pubKeyHash;
    joinsplit.Verify(*pzcashParams, pubKeyHash);
    return timer_stop(tv_start);
}

double benchmark_solve_equihash()
{
    CBlock pblock;
    CEquihashInput I{pblock};
    CDataStream ss(SER_NETWORK, PROTOCOL_VERSION);
    ss << I;

    unsigned int n = Params(CBaseChainParams::MAIN).EquihashN();
    unsigned int k = Params(CBaseChainParams::MAIN).EquihashK();
    crypto_generichash_blake2b_state eh_state;
    EhInitialiseState(n, k, eh_state);
    crypto_generichash_blake2b_update(&eh_state, (unsigned char*)&ss[0], ss.size());

    uint256 nonce;
    randombytes_buf(nonce.begin(), 32);
    crypto_generichash_blake2b_update(&eh_state,
                                    nonce.begin(),
                                    nonce.size());

    struct timeval tv_start;
    timer_start(tv_start);
    std::set<std::vector<unsigned int>> solns;
    EhOptimisedSolveUncancellable(n, k, eh_state,
                                  [](std::vector<unsigned char> soln) { return false; });
    return timer_stop(tv_start);
}

std::vector<double> benchmark_solve_equihash_threaded(int nThreads)
{
    std::vector<double> ret;
    std::vector<boost::shared_future<double>> tasks;
    for (int i = 0; i < nThreads; i++) {
        tasks.emplace_back(boost::async(&benchmark_solve_equihash));
    }
    auto it = boost::wait_for_any(tasks.begin(),tasks.end());
    while(it != tasks.end()) {
        ret.push_back(it->get());
        tasks.erase(it);
        it = boost::wait_for_any(tasks.begin(),tasks.end());
    }
    return ret;
}

double benchmark_verify_equihash()
{
    CChainParams params = Params(CBaseChainParams::MAIN);
    CBlock genesis = Params(CBaseChainParams::MAIN).GenesisBlock();
    CBlockHeader genesis_header = genesis.GetBlockHeader();
    struct timeval tv_start;
    timer_start(tv_start);
    CheckEquihashSolution(&genesis_header, params);
    return timer_stop(tv_start);
}

double benchmark_large_tx()
{
    // Number of inputs in the spending transaction that we will simulate
    const size_t NUM_INPUTS = 11100;

    // Create priv/pub key
    CKey priv;
    priv.MakeNewKey(false);
    auto pub = priv.GetPubKey();
    CBasicKeyStore tempKeystore;
    tempKeystore.AddKey(priv);

    // The "original" transaction that the spending transaction will spend
    // from.
    CMutableTransaction m_orig_tx;
    m_orig_tx.vout.resize(1);
    m_orig_tx.vout[0].nValue = 1000000;
    CScript prevPubKey = GetScriptForDestination(pub.GetID());
    m_orig_tx.vout[0].scriptPubKey = prevPubKey;

    auto orig_tx = CTransaction(m_orig_tx);

    CMutableTransaction spending_tx;
    auto input_hash = orig_tx.GetTxid();
    // Add NUM_INPUTS inputs
    for (size_t i = 0; i < NUM_INPUTS; i++) {
        spending_tx.vin.emplace_back(input_hash, 0);
    }

    // Sign for all the inputs
    for (size_t i = 0; i < NUM_INPUTS; i++) {
        SignSignature(tempKeystore, prevPubKey, spending_tx, i, SIGHASH_ALL);
    }

    // Serialize:
    {
        CDataStream ss(SER_NETWORK, PROTOCOL_VERSION);
        ss << spending_tx;
        //std::cout << "SIZE OF SPENDING TX: " << ss.size() << std::endl;

        auto error = MAX_BLOCK_SIZE / 20; // 5% error
        assert(ss.size() < MAX_BLOCK_SIZE + error);
        assert(ss.size() > MAX_BLOCK_SIZE - error);
    }

    // Spending tx has all its inputs signed and does not need to be mutated anymore
    CTransaction final_spending_tx(spending_tx);

    // Benchmark signature verification costs:
    struct timeval tv_start;
    timer_start(tv_start);
    for (size_t i = 0; i < NUM_INPUTS; i++) {
        ScriptError serror = SCRIPT_ERR_OK;
        assert(VerifyScript(final_spending_tx.vin[i].scriptSig,
                            prevPubKey,
                            STANDARD_SCRIPT_VERIFY_FLAGS,
                            TransactionSignatureChecker(&final_spending_tx, i),
                            &serror));
    }
    return timer_stop(tv_start);
}

