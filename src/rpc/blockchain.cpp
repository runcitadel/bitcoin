// Copyright (c) 2010 Satoshi Nakamoto
// Copyright (c) 2009-2020 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <rpc/blockchain.h>

#include <amount.h>
#include <blockfilter.h>
#include <chain.h>
#include <chainparams.h>
#include <coins.h>
#include <consensus/params.h>
#include <consensus/validation.h>
#include <core_io.h>
#include <deploymentinfo.h>
#include <deploymentstatus.h>
#include <hash.h>
#include <index/blockfilterindex.h>
#include <index/coinstatsindex.h>
#include <net.h>
#include <net_processing.h>
#include <node/blockstorage.h>
#include <node/coinstats.h>
#include <node/context.h>
#include <node/utxo_snapshot.h>
#include <policy/feerate.h>
#include <policy/fees.h>
#include <policy/policy.h>
#include <policy/rbf.h>
#include <primitives/transaction.h>
#include <rpc/net.h>
#include <rpc/server.h>
#include <rpc/util.h>
#include <script/descriptor.h>
#include <streams.h>
#include <sync.h>
#include <txdb.h>
#include <txmempool.h>
#include <undo.h>
#include <util/strencodings.h>
#include <util/string.h>
#include <util/system.h>
#include <util/translation.h>
#include <validation.h>
#include <validationinterface.h>
#include <versionbits.h>
#include <warnings.h>

#include <stdint.h>

#include <univalue.h>

#include <condition_variable>
#include <memory>
#include <mutex>
#include <optional>

struct CUpdatedBlock
{
    uint256 hash;
    int height;
};

static Mutex cs_blockchange;
static std::condition_variable cond_blockchange;
static CUpdatedBlock latestblock GUARDED_BY(cs_blockchange);

NodeContext& EnsureAnyNodeContext(const std::any& context)
{
    auto node_context = util::AnyPtr<NodeContext>(context);
    if (!node_context) {
        throw JSONRPCError(RPC_INTERNAL_ERROR, "Node context not found");
    }
    return *node_context;
}

CTxMemPool& EnsureMemPool(const NodeContext& node)
{
    if (!node.mempool) {
        throw JSONRPCError(RPC_CLIENT_MEMPOOL_DISABLED, "Mempool disabled or instance not found");
    }
    return *node.mempool;
}

CTxMemPool& EnsureAnyMemPool(const std::any& context)
{
    return EnsureMemPool(EnsureAnyNodeContext(context));
}

ChainstateManager& EnsureChainman(const NodeContext& node)
{
    if (!node.chainman) {
        throw JSONRPCError(RPC_INTERNAL_ERROR, "Node chainman not found");
    }
    return *node.chainman;
}

ChainstateManager& EnsureAnyChainman(const std::any& context)
{
    return EnsureChainman(EnsureAnyNodeContext(context));
}

CBlockPolicyEstimator& EnsureFeeEstimator(const NodeContext& node)
{
    if (!node.fee_estimator) {
        throw JSONRPCError(RPC_INTERNAL_ERROR, "Fee estimation disabled");
    }
    return *node.fee_estimator;
}

CBlockPolicyEstimator& EnsureAnyFeeEstimator(const std::any& context)
{
    return EnsureFeeEstimator(EnsureAnyNodeContext(context));
}

/* Calculate the difficulty for a given block index.
 */
double GetDifficulty(const CBlockIndex* blockindex)
{
    CHECK_NONFATAL(blockindex);

    int nShift = (blockindex->nBits >> 24) & 0xff;
    double dDiff =
        (double)0x0000ffff / (double)(blockindex->nBits & 0x00ffffff);

    while (nShift < 29)
    {
        dDiff *= 256.0;
        nShift++;
    }
    while (nShift > 29)
    {
        dDiff /= 256.0;
        nShift--;
    }

    return dDiff;
}

static int ComputeNextBlockAndDepth(const CBlockIndex* tip, const CBlockIndex* blockindex, const CBlockIndex*& next)
{
    next = tip->GetAncestor(blockindex->nHeight + 1);
    if (next && next->pprev == blockindex) {
        return tip->nHeight - blockindex->nHeight + 1;
    }
    next = nullptr;
    return blockindex == tip ? 1 : -1;
}

CBlockIndex* ParseHashOrHeight(const UniValue& param, ChainstateManager& chainman) {
    LOCK(::cs_main);
    CChain& active_chain = chainman.ActiveChain();

    if (param.isNum()) {
        const int height{param.get_int()};
        if (height < 0) {
            throw JSONRPCError(RPC_INVALID_PARAMETER, strprintf("Target block height %d is negative", height));
        }
        const int current_tip{active_chain.Height()};
        if (height > current_tip) {
            throw JSONRPCError(RPC_INVALID_PARAMETER, strprintf("Target block height %d after current tip %d", height, current_tip));
        }

        return active_chain[height];
    } else {
        const uint256 hash{ParseHashV(param, "hash_or_height")};
        CBlockIndex* pindex = chainman.m_blockman.LookupBlockIndex(hash);

        if (!pindex) {
            throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Block not found");
        }

        return pindex;
    }
}

UniValue blockheaderToJSON(const CBlockIndex* tip, const CBlockIndex* blockindex)
{
    // Serialize passed information without accessing chain state of the active chain!
    AssertLockNotHeld(cs_main); // For performance reasons

    UniValue result(UniValue::VOBJ);
    result.pushKV("hash", blockindex->GetBlockHash().GetHex());
    const CBlockIndex* pnext;
    int confirmations = ComputeNextBlockAndDepth(tip, blockindex, pnext);
    result.pushKV("confirmations", confirmations);
    result.pushKV("height", blockindex->nHeight);
    result.pushKV("version", blockindex->nVersion);
    result.pushKV("versionHex", strprintf("%08x", blockindex->nVersion));
    result.pushKV("merkleroot", blockindex->hashMerkleRoot.GetHex());
    result.pushKV("time", (int64_t)blockindex->nTime);
    result.pushKV("mediantime", (int64_t)blockindex->GetMedianTimePast());
    result.pushKV("nonce", (uint64_t)blockindex->nNonce);
    result.pushKV("bits", strprintf("%08x", blockindex->nBits));
    result.pushKV("difficulty", GetDifficulty(blockindex));
    result.pushKV("chainwork", blockindex->nChainWork.GetHex());
    result.pushKV("nTx", (uint64_t)blockindex->nTx);

    if (blockindex->pprev)
        result.pushKV("previousblockhash", blockindex->pprev->GetBlockHash().GetHex());
    if (pnext)
        result.pushKV("nextblockhash", pnext->GetBlockHash().GetHex());
    return result;
}

UniValue blockToJSON(const CBlock& block, const CBlockIndex* tip, const CBlockIndex* blockindex, TxVerbosity verbosity)
{
    UniValue result = blockheaderToJSON(tip, blockindex);

    result.pushKV("strippedsize", (int)::GetSerializeSize(block, PROTOCOL_VERSION | SERIALIZE_TRANSACTION_NO_WITNESS));
    result.pushKV("size", (int)::GetSerializeSize(block, PROTOCOL_VERSION));
    result.pushKV("weight", (int)::GetBlockWeight(block));
    UniValue txs(UniValue::VARR);

    switch (verbosity) {
        case TxVerbosity::SHOW_TXID:
            for (const CTransactionRef& tx : block.vtx) {
                txs.push_back(tx->GetHash().GetHex());
            }
            break;

        case TxVerbosity::SHOW_DETAILS:
        case TxVerbosity::SHOW_DETAILS_AND_PREVOUT:
            CBlockUndo blockUndo;
            const bool have_undo = !IsBlockPruned(blockindex) && UndoReadFromDisk(blockUndo, blockindex);

            for (size_t i = 0; i < block.vtx.size(); ++i) {
                const CTransactionRef& tx = block.vtx.at(i);
                // coinbase transaction (i.e. i == 0) doesn't have undo data
                const CTxUndo* txundo = (have_undo && i > 0) ? &blockUndo.vtxundo.at(i - 1) : nullptr;
                UniValue objTx(UniValue::VOBJ);
                TxToUniv(*tx, uint256(), objTx, true, RPCSerializationFlags(), txundo, verbosity);
                txs.push_back(objTx);
            }
    }

    result.pushKV("tx", txs);

    return result;
}

static RPCHelpMan getblockcount()
{
    return RPCHelpMan{"getblockcount",
                "\nReturns the height of the most-work fully-validated chain.\n"
                "The genesis block has height 0.\n",
                {},
                RPCResult{
                    RPCResult::Type::NUM, "", "The current block count"},
                RPCExamples{
                    HelpExampleCli("getblockcount", "")
            + HelpExampleRpc("getblockcount", "")
                },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    ChainstateManager& chainman = EnsureAnyChainman(request.context);
    LOCK(cs_main);
    return chainman.ActiveChain().Height();
},
    };
}

static RPCHelpMan getbestblockhash()
{
    return RPCHelpMan{"getbestblockhash",
                "\nReturns the hash of the best (tip) block in the most-work fully-validated chain.\n",
                {},
                RPCResult{
                    RPCResult::Type::STR_HEX, "", "the block hash, hex-encoded"},
                RPCExamples{
                    HelpExampleCli("getbestblockhash", "")
            + HelpExampleRpc("getbestblockhash", "")
                },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    ChainstateManager& chainman = EnsureAnyChainman(request.context);
    LOCK(cs_main);
    return chainman.ActiveChain().Tip()->GetBlockHash().GetHex();
},
    };
}

void RPCNotifyBlockChange(const CBlockIndex* pindex)
{
    if(pindex) {
        LOCK(cs_blockchange);
        latestblock.hash = pindex->GetBlockHash();
        latestblock.height = pindex->nHeight;
    }
    cond_blockchange.notify_all();
}

static RPCHelpMan waitfornewblock()
{
    return RPCHelpMan{"waitfornewblock",
                "\nWaits for a specific new block and returns useful info about it.\n"
                "\nReturns the current block on timeout or exit.\n",
                {
                    {"timeout", RPCArg::Type::NUM, RPCArg::Default{0}, "Time in milliseconds to wait for a response. 0 indicates no timeout."},
                },
                RPCResult{
                    RPCResult::Type::OBJ, "", "",
                    {
                        {RPCResult::Type::STR_HEX, "hash", "The blockhash"},
                        {RPCResult::Type::NUM, "height", "Block height"},
                    }},
                RPCExamples{
                    HelpExampleCli("waitfornewblock", "1000")
            + HelpExampleRpc("waitfornewblock", "1000")
                },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    int timeout = 0;
    if (!request.params[0].isNull())
        timeout = request.params[0].get_int();

    CUpdatedBlock block;
    {
        WAIT_LOCK(cs_blockchange, lock);
        block = latestblock;
        if(timeout)
            cond_blockchange.wait_for(lock, std::chrono::milliseconds(timeout), [&block]() EXCLUSIVE_LOCKS_REQUIRED(cs_blockchange) {return latestblock.height != block.height || latestblock.hash != block.hash || !IsRPCRunning(); });
        else
            cond_blockchange.wait(lock, [&block]() EXCLUSIVE_LOCKS_REQUIRED(cs_blockchange) {return latestblock.height != block.height || latestblock.hash != block.hash || !IsRPCRunning(); });
        block = latestblock;
    }
    UniValue ret(UniValue::VOBJ);
    ret.pushKV("hash", block.hash.GetHex());
    ret.pushKV("height", block.height);
    return ret;
},
    };
}

static RPCHelpMan waitforblock()
{
    return RPCHelpMan{"waitforblock",
                "\nWaits for a specific new block and returns useful info about it.\n"
                "\nReturns the current block on timeout or exit.\n",
                {
                    {"blockhash", RPCArg::Type::STR_HEX, RPCArg::Optional::NO, "Block hash to wait for."},
                    {"timeout", RPCArg::Type::NUM, RPCArg::Default{0}, "Time in milliseconds to wait for a response. 0 indicates no timeout."},
                },
                RPCResult{
                    RPCResult::Type::OBJ, "", "",
                    {
                        {RPCResult::Type::STR_HEX, "hash", "The blockhash"},
                        {RPCResult::Type::NUM, "height", "Block height"},
                    }},
                RPCExamples{
                    HelpExampleCli("waitforblock", "\"0000000000079f8ef3d2c688c244eb7a4570b24c9ed7b4a8c619eb02596f8862\" 1000")
            + HelpExampleRpc("waitforblock", "\"0000000000079f8ef3d2c688c244eb7a4570b24c9ed7b4a8c619eb02596f8862\", 1000")
                },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    int timeout = 0;

    uint256 hash(ParseHashV(request.params[0], "blockhash"));

    if (!request.params[1].isNull())
        timeout = request.params[1].get_int();

    CUpdatedBlock block;
    {
        WAIT_LOCK(cs_blockchange, lock);
        if(timeout)
            cond_blockchange.wait_for(lock, std::chrono::milliseconds(timeout), [&hash]() EXCLUSIVE_LOCKS_REQUIRED(cs_blockchange) {return latestblock.hash == hash || !IsRPCRunning();});
        else
            cond_blockchange.wait(lock, [&hash]() EXCLUSIVE_LOCKS_REQUIRED(cs_blockchange) {return latestblock.hash == hash || !IsRPCRunning(); });
        block = latestblock;
    }

    UniValue ret(UniValue::VOBJ);
    ret.pushKV("hash", block.hash.GetHex());
    ret.pushKV("height", block.height);
    return ret;
},
    };
}

static RPCHelpMan waitforblockheight()
{
    return RPCHelpMan{"waitforblockheight",
                "\nWaits for (at least) block height and returns the height and hash\n"
                "of the current tip.\n"
                "\nReturns the current block on timeout or exit.\n",
                {
                    {"height", RPCArg::Type::NUM, RPCArg::Optional::NO, "Block height to wait for."},
                    {"timeout", RPCArg::Type::NUM, RPCArg::Default{0}, "Time in milliseconds to wait for a response. 0 indicates no timeout."},
                },
                RPCResult{
                    RPCResult::Type::OBJ, "", "",
                    {
                        {RPCResult::Type::STR_HEX, "hash", "The blockhash"},
                        {RPCResult::Type::NUM, "height", "Block height"},
                    }},
                RPCExamples{
                    HelpExampleCli("waitforblockheight", "100 1000")
            + HelpExampleRpc("waitforblockheight", "100, 1000")
                },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    int timeout = 0;

    int height = request.params[0].get_int();

    if (!request.params[1].isNull())
        timeout = request.params[1].get_int();

    CUpdatedBlock block;
    {
        WAIT_LOCK(cs_blockchange, lock);
        if(timeout)
            cond_blockchange.wait_for(lock, std::chrono::milliseconds(timeout), [&height]() EXCLUSIVE_LOCKS_REQUIRED(cs_blockchange) {return latestblock.height >= height || !IsRPCRunning();});
        else
            cond_blockchange.wait(lock, [&height]() EXCLUSIVE_LOCKS_REQUIRED(cs_blockchange) {return latestblock.height >= height || !IsRPCRunning(); });
        block = latestblock;
    }
    UniValue ret(UniValue::VOBJ);
    ret.pushKV("hash", block.hash.GetHex());
    ret.pushKV("height", block.height);
    return ret;
},
    };
}

static RPCHelpMan syncwithvalidationinterfacequeue()
{
    return RPCHelpMan{"syncwithvalidationinterfacequeue",
                "\nWaits for the validation interface queue to catch up on everything that was there when we entered this function.\n",
                {},
                RPCResult{RPCResult::Type::NONE, "", ""},
                RPCExamples{
                    HelpExampleCli("syncwithvalidationinterfacequeue","")
            + HelpExampleRpc("syncwithvalidationinterfacequeue","")
                },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    SyncWithValidationInterfaceQueue();
    return NullUniValue;
},
    };
}

static RPCHelpMan getdifficulty()
{
    return RPCHelpMan{"getdifficulty",
                "\nReturns the proof-of-work difficulty as a multiple of the minimum difficulty.\n",
                {},
                RPCResult{
                    RPCResult::Type::NUM, "", "the proof-of-work difficulty as a multiple of the minimum difficulty."},
                RPCExamples{
                    HelpExampleCli("getdifficulty", "")
            + HelpExampleRpc("getdifficulty", "")
                },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    ChainstateManager& chainman = EnsureAnyChainman(request.context);
    LOCK(cs_main);
    return GetDifficulty(chainman.ActiveChain().Tip());
},
    };
}

static std::vector<RPCResult> MempoolEntryDescription() { return {
    RPCResult{RPCResult::Type::NUM, "vsize", "virtual transaction size as defined in BIP 141. This is different from actual serialized size for witness transactions as witness data is discounted."},
    RPCResult{RPCResult::Type::NUM, "weight", "transaction weight as defined in BIP 141."},
    RPCResult{RPCResult::Type::STR_AMOUNT, "fee", "transaction fee in " + CURRENCY_UNIT + " (DEPRECATED)"},
    RPCResult{RPCResult::Type::STR_AMOUNT, "modifiedfee", "transaction fee with fee deltas used for mining priority (DEPRECATED)"},
    RPCResult{RPCResult::Type::NUM_TIME, "time", "local time transaction entered pool in seconds since 1 Jan 1970 GMT"},
    RPCResult{RPCResult::Type::NUM, "height", "block height when transaction entered pool"},
    RPCResult{RPCResult::Type::NUM, "startingpriority", "Priority when transaction entered pool"},
    RPCResult{RPCResult::Type::NUM, "currentpriority", "Transaction priority now"},
    RPCResult{RPCResult::Type::NUM, "descendantcount", "number of in-mempool descendant transactions (including this one)"},
    RPCResult{RPCResult::Type::NUM, "descendantsize", "virtual transaction size of in-mempool descendants (including this one)"},
    RPCResult{RPCResult::Type::STR_AMOUNT, "descendantfees", "modified fees (see above) of in-mempool descendants (including this one) (DEPRECATED)"},
    RPCResult{RPCResult::Type::NUM, "ancestorcount", "number of in-mempool ancestor transactions (including this one)"},
    RPCResult{RPCResult::Type::NUM, "ancestorsize", "virtual transaction size of in-mempool ancestors (including this one)"},
    RPCResult{RPCResult::Type::STR_AMOUNT, "ancestorfees", "modified fees (see above) of in-mempool ancestors (including this one) (DEPRECATED)"},
    RPCResult{RPCResult::Type::STR_HEX, "hash", "hash of entire serialized transaction"},
    RPCResult{RPCResult::Type::STR_HEX, "wtxid", "hash of serialized transaction, including witness data"},
    RPCResult{RPCResult::Type::OBJ, "fees", "",
        {
            RPCResult{RPCResult::Type::STR_AMOUNT, "base", "transaction fee in " + CURRENCY_UNIT},
            RPCResult{RPCResult::Type::STR_AMOUNT, "modified", "transaction fee with fee deltas used for mining priority in " + CURRENCY_UNIT},
            RPCResult{RPCResult::Type::STR_AMOUNT, "ancestor", "modified fees (see above) of in-mempool ancestors (including this one) in " + CURRENCY_UNIT},
            RPCResult{RPCResult::Type::STR_AMOUNT, "descendant", "modified fees (see above) of in-mempool descendants (including this one) in " + CURRENCY_UNIT},
        }},
    RPCResult{RPCResult::Type::ARR, "depends", "unconfirmed transactions used as inputs for this transaction",
        {RPCResult{RPCResult::Type::STR_HEX, "transactionid", "parent transaction id"}}},
    RPCResult{RPCResult::Type::ARR, "spentby", "unconfirmed transactions spending outputs from this transaction",
        {RPCResult{RPCResult::Type::STR_HEX, "transactionid", "child transaction id"}}},
    RPCResult{RPCResult::Type::BOOL, "bip125-replaceable", "Whether this transaction could be replaced due to BIP125 (replace-by-fee)"},
    RPCResult{RPCResult::Type::BOOL, "unbroadcast", "Whether this transaction is currently unbroadcast (initial broadcast not yet acknowledged by any peers)"},
};}

static void entryToJSON(const CTxMemPool& pool, UniValue& info, const CTxMemPoolEntry& e, const int next_block_height) EXCLUSIVE_LOCKS_REQUIRED(pool.cs)
{
    AssertLockHeld(pool.cs);

    UniValue fees(UniValue::VOBJ);
    fees.pushKV("base", ValueFromAmount(e.GetFee()));
    fees.pushKV("modified", ValueFromAmount(e.GetModifiedFee()));
    fees.pushKV("ancestor", ValueFromAmount(e.GetModFeesWithAncestors()));
    fees.pushKV("descendant", ValueFromAmount(e.GetModFeesWithDescendants()));
    info.pushKV("fees", fees);

    info.pushKV("vsize", (int)e.GetTxSize());
    info.pushKV("weight", (int)e.GetTxWeight());
    info.pushKV("fee", ValueFromAmount(e.GetFee()));
    info.pushKV("modifiedfee", ValueFromAmount(e.GetModifiedFee()));
    info.pushKV("time", count_seconds(e.GetTime()));
    info.pushKV("height", (int)e.GetHeight());
    info.pushKV("startingpriority", e.GetStartingPriority());
    info.pushKV("currentpriority", e.GetPriority(next_block_height));
    info.pushKV("descendantcount", e.GetCountWithDescendants());
    info.pushKV("descendantsize", e.GetSizeWithDescendants());
    info.pushKV("descendantfees", e.GetModFeesWithDescendants());
    info.pushKV("ancestorcount", e.GetCountWithAncestors());
    info.pushKV("ancestorsize", e.GetSizeWithAncestors());
    info.pushKV("ancestorfees", e.GetModFeesWithAncestors());
    info.pushKV("wtxid", pool.vTxHashes[e.vTxHashesIdx].first.ToString());
    info.pushKV("hash", info["wtxid"]);
    const CTransaction& tx = e.GetTx();
    std::set<std::string> setDepends;
    for (const CTxIn& txin : tx.vin)
    {
        if (pool.exists(txin.prevout.hash))
            setDepends.insert(txin.prevout.hash.ToString());
    }

    UniValue depends(UniValue::VARR);
    for (const std::string& dep : setDepends)
    {
        depends.push_back(dep);
    }

    info.pushKV("depends", depends);

    UniValue spent(UniValue::VARR);
    const CTxMemPool::txiter& it = pool.mapTx.find(tx.GetHash());
    const CTxMemPoolEntry::Children& children = it->GetMemPoolChildrenConst();
    for (const CTxMemPoolEntry& child : children) {
        spent.push_back(child.GetTx().GetHash().ToString());
    }

    info.pushKV("spentby", spent);

    // Add opt-in RBF status
    bool rbfStatus = false;
    RBFTransactionState rbfState = IsRBFOptIn(tx, pool);
    if (rbfState == RBFTransactionState::UNKNOWN) {
        throw JSONRPCError(RPC_MISC_ERROR, "Transaction is not in mempool");
    } else if (rbfState == RBFTransactionState::REPLACEABLE_BIP125) {
        rbfStatus = true;
    }

    info.pushKV("bip125-replaceable", rbfStatus);
    info.pushKV("unbroadcast", pool.IsUnbroadcastTx(tx.GetHash()));
}

UniValue MempoolToJSON(ChainstateManager &chainman, const CTxMemPool& pool, bool verbose, bool include_mempool_sequence)
{
    if (verbose) {
        if (include_mempool_sequence) {
            throw JSONRPCError(RPC_INVALID_PARAMETER, "Verbose results cannot contain mempool sequence values.");
        }
        LOCK(::cs_main);
        const CChain& active_chain = chainman.ActiveChain();
        const int next_block_height = active_chain.Height() + 1;
        LOCK(pool.cs);
        // TODO: Release cs_main after mempool.cs acquired

        UniValue o(UniValue::VOBJ);
        for (const CTxMemPoolEntry& e : pool.mapTx) {
            const uint256& hash = e.GetTx().GetHash();
            UniValue info(UniValue::VOBJ);
            entryToJSON(pool, info, e, next_block_height);
            // Mempool has unique entries so there is no advantage in using
            // UniValue::pushKV, which checks if the key already exists in O(N).
            // UniValue::__pushKV is used instead which currently is O(1).
            o.__pushKV(hash.ToString(), info);
        }
        return o;
    } else {
        uint64_t mempool_sequence;
        std::vector<uint256> vtxid;
        {
            LOCK(pool.cs);
            pool.queryHashes(vtxid);
            mempool_sequence = pool.GetSequence();
        }
        UniValue a(UniValue::VARR);
        for (const uint256& hash : vtxid)
            a.push_back(hash.ToString());

        if (!include_mempool_sequence) {
            return a;
        } else {
            UniValue o(UniValue::VOBJ);
            o.pushKV("txids", a);
            o.pushKV("mempool_sequence", mempool_sequence);
            return o;
        }
    }
}

static RPCHelpMan maxmempool()
{
    return RPCHelpMan{"maxmempool",
                "\nSets the allocated memory for the memory pool.\n",
                {
                    {"megabytes", RPCArg::Type::NUM, RPCArg::Optional::NO, "The memory allocated in MB"},
                },
                RPCResult{
                    RPCResult::Type::NONE, "", ""},
                RPCExamples{
                    HelpExampleCli("maxmempool", "150") + HelpExampleRpc("maxmempool", "150")
                },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    int64_t nSize = request.params[0].get_int64();
    int64_t nMempoolSizeMax = nSize * 1000000;
    int64_t nMempoolSizeMin = maxmempoolMinimum(gArgs.GetArg("-limitdescendantsize", DEFAULT_DESCENDANT_SIZE_LIMIT)) * 1000000;
    if (nMempoolSizeMax < 0 || nMempoolSizeMax < nMempoolSizeMin)
        throw JSONRPCError(RPC_INVALID_PARAMETER, strprintf("MaxMempool size %d is too small", nSize));
    gArgs.ForceSetArg("-maxmempool", strprintf("%d", nSize));

    auto node_context = util::AnyPtr<NodeContext>(request.context);
    if (node_context && node_context->mempool && node_context->chainman) {
        LOCK(cs_main);
        CChainState& active_chainstate = node_context->chainman->ActiveChainstate();
        LimitMempoolSize(*node_context->mempool, active_chainstate.CoinsTip());
    }

    return NullUniValue;
}
    };
}

static RPCHelpMan getrawmempool()
{
    return RPCHelpMan{"getrawmempool",
                "\nReturns all transaction ids in memory pool as a json array of string transaction ids.\n"
                "\nHint: use getmempoolentry to fetch a specific transaction from the mempool.\n",
                {
                    {"verbose", RPCArg::Type::BOOL, RPCArg::Default{false}, "True for a json object, false for array of transaction ids"},
                    {"mempool_sequence", RPCArg::Type::BOOL, RPCArg::Default{false}, "If verbose=false, returns a json object with transaction list and mempool sequence number attached."},
                },
                {
                    RPCResult{"for verbose = false",
                        RPCResult::Type::ARR, "", "",
                        {
                            {RPCResult::Type::STR_HEX, "", "The transaction id"},
                        }},
                    RPCResult{"for verbose = true",
                        RPCResult::Type::OBJ_DYN, "", "",
                        {
                            {RPCResult::Type::OBJ, "transactionid", "", MempoolEntryDescription()},
                        }},
                    RPCResult{"for verbose = false and mempool_sequence = true",
                        RPCResult::Type::OBJ, "", "",
                        {
                            {RPCResult::Type::ARR, "txids", "",
                            {
                                {RPCResult::Type::STR_HEX, "", "The transaction id"},
                            }},
                            {RPCResult::Type::NUM, "mempool_sequence", "The mempool sequence value."},
                        }},
                },
                RPCExamples{
                    HelpExampleCli("getrawmempool", "true")
            + HelpExampleRpc("getrawmempool", "true")
                },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    bool fVerbose = false;
    if (!request.params[0].isNull())
        fVerbose = request.params[0].get_bool();

    bool include_mempool_sequence = false;
    if (!request.params[1].isNull()) {
        include_mempool_sequence = request.params[1].get_bool();
    }

    NodeContext& node = EnsureAnyNodeContext(request.context);
    ChainstateManager& chainman = EnsureChainman(node);
    return MempoolToJSON(chainman, EnsureAnyMemPool(request.context), fVerbose, include_mempool_sequence);
},
    };
}

static RPCHelpMan getmempoolancestors()
{
    return RPCHelpMan{"getmempoolancestors",
                "\nIf txid is in the mempool, returns all in-mempool ancestors.\n",
                {
                    {"txid", RPCArg::Type::STR_HEX, RPCArg::Optional::NO, "The transaction id (must be in mempool)"},
                    {"verbose", RPCArg::Type::BOOL, RPCArg::Default{false}, "True for a json object, false for array of transaction ids"},
                },
                {
                    RPCResult{"for verbose = false",
                        RPCResult::Type::ARR, "", "",
                        {{RPCResult::Type::STR_HEX, "", "The transaction id of an in-mempool ancestor transaction"}}},
                    RPCResult{"for verbose = true",
                        RPCResult::Type::OBJ_DYN, "", "",
                        {
                            {RPCResult::Type::OBJ, "transactionid", "", MempoolEntryDescription()},
                        }},
                },
                RPCExamples{
                    HelpExampleCli("getmempoolancestors", "\"mytxid\"")
            + HelpExampleRpc("getmempoolancestors", "\"mytxid\"")
                },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    bool fVerbose = false;
    if (!request.params[1].isNull())
        fVerbose = request.params[1].get_bool();

    uint256 hash = ParseHashV(request.params[0], "parameter 1");

    const CTxMemPool& mempool = EnsureAnyMemPool(request.context);
    ChainstateManager& chainman = EnsureAnyChainman(request.context);
    LOCK(::cs_main);
    const CChain& active_chain = chainman.ActiveChain();
    const int next_block_height = active_chain.Height() + 1;
    LOCK(mempool.cs);
    // TODO: Release cs_main after mempool.cs acquired

    CTxMemPool::txiter it = mempool.mapTx.find(hash);
    if (it == mempool.mapTx.end()) {
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Transaction not in mempool");
    }

    CTxMemPool::setEntries setAncestors;
    uint64_t noLimit = std::numeric_limits<uint64_t>::max();
    std::string dummy;
    mempool.CalculateMemPoolAncestors(*it, setAncestors, noLimit, noLimit, noLimit, noLimit, dummy, false);

    if (!fVerbose) {
        UniValue o(UniValue::VARR);
        for (CTxMemPool::txiter ancestorIt : setAncestors) {
            o.push_back(ancestorIt->GetTx().GetHash().ToString());
        }
        return o;
    } else {
        UniValue o(UniValue::VOBJ);
        for (CTxMemPool::txiter ancestorIt : setAncestors) {
            const CTxMemPoolEntry &e = *ancestorIt;
            const uint256& _hash = e.GetTx().GetHash();
            UniValue info(UniValue::VOBJ);
            entryToJSON(mempool, info, e, next_block_height);
            o.pushKV(_hash.ToString(), info);
        }
        return o;
    }
},
    };
}

static RPCHelpMan getmempooldescendants()
{
    return RPCHelpMan{"getmempooldescendants",
                "\nIf txid is in the mempool, returns all in-mempool descendants.\n",
                {
                    {"txid", RPCArg::Type::STR_HEX, RPCArg::Optional::NO, "The transaction id (must be in mempool)"},
                    {"verbose", RPCArg::Type::BOOL, RPCArg::Default{false}, "True for a json object, false for array of transaction ids"},
                },
                {
                    RPCResult{"for verbose = false",
                        RPCResult::Type::ARR, "", "",
                        {{RPCResult::Type::STR_HEX, "", "The transaction id of an in-mempool descendant transaction"}}},
                    RPCResult{"for verbose = true",
                        RPCResult::Type::OBJ_DYN, "", "",
                        {
                            {RPCResult::Type::OBJ, "transactionid", "", MempoolEntryDescription()},
                        }},
                },
                RPCExamples{
                    HelpExampleCli("getmempooldescendants", "\"mytxid\"")
            + HelpExampleRpc("getmempooldescendants", "\"mytxid\"")
                },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    bool fVerbose = false;
    if (!request.params[1].isNull())
        fVerbose = request.params[1].get_bool();

    uint256 hash = ParseHashV(request.params[0], "parameter 1");

    const CTxMemPool& mempool = EnsureAnyMemPool(request.context);
    ChainstateManager& chainman = EnsureAnyChainman(request.context);
    LOCK(::cs_main);
    const CChain& active_chain = chainman.ActiveChain();
    const int next_block_height = active_chain.Height() + 1;
    LOCK(mempool.cs);
    // TODO: Release cs_main after mempool.cs acquired

    CTxMemPool::txiter it = mempool.mapTx.find(hash);
    if (it == mempool.mapTx.end()) {
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Transaction not in mempool");
    }

    CTxMemPool::setEntries setDescendants;
    mempool.CalculateDescendants(it, setDescendants);
    // CTxMemPool::CalculateDescendants will include the given tx
    setDescendants.erase(it);

    if (!fVerbose) {
        UniValue o(UniValue::VARR);
        for (CTxMemPool::txiter descendantIt : setDescendants) {
            o.push_back(descendantIt->GetTx().GetHash().ToString());
        }

        return o;
    } else {
        UniValue o(UniValue::VOBJ);
        for (CTxMemPool::txiter descendantIt : setDescendants) {
            const CTxMemPoolEntry &e = *descendantIt;
            const uint256& _hash = e.GetTx().GetHash();
            UniValue info(UniValue::VOBJ);
            entryToJSON(mempool, info, e, next_block_height);
            o.pushKV(_hash.ToString(), info);
        }
        return o;
    }
},
    };
}

static RPCHelpMan getmempoolentry()
{
    return RPCHelpMan{"getmempoolentry",
                "\nReturns mempool data for given transaction\n",
                {
                    {"txid", RPCArg::Type::STR_HEX, RPCArg::Optional::NO, "The transaction id (must be in mempool)"},
                },
                RPCResult{
                    RPCResult::Type::OBJ, "", "", MempoolEntryDescription()},
                RPCExamples{
                    HelpExampleCli("getmempoolentry", "\"mytxid\"")
            + HelpExampleRpc("getmempoolentry", "\"mytxid\"")
                },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    uint256 hash = ParseHashV(request.params[0], "parameter 1");

    const CTxMemPool& mempool = EnsureAnyMemPool(request.context);
    ChainstateManager& chainman = EnsureAnyChainman(request.context);
    LOCK(::cs_main);
    const CChain& active_chain = chainman.ActiveChain();
    const int next_block_height = active_chain.Height() + 1;
    LOCK(mempool.cs);
    // TODO: Release cs_main after mempool.cs acquired

    CTxMemPool::txiter it = mempool.mapTx.find(hash);
    if (it == mempool.mapTx.end()) {
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Transaction not in mempool");
    }

    const CTxMemPoolEntry &e = *it;
    UniValue info(UniValue::VOBJ);
    entryToJSON(mempool, info, e, next_block_height);
    return info;
},
    };
}

static RPCHelpMan getblockfrompeer()
{
    return RPCHelpMan{"getblockfrompeer",
                "\nAttempt to fetch block from a given peer.\n"
                "\nReturns {} if a block-request was successfully scheduled\n",
                {
                    {"blockhash", RPCArg::Type::STR_HEX, RPCArg::Optional::NO, "The block hash"},
                    {"nodeid", RPCArg::Type::NUM, RPCArg::Optional::NO, "The node ID (see getpeerinfo for node IDs)"},
                },
                RPCResult{RPCResult::Type::OBJ, "", "",
                {
                    {RPCResult::Type::STR, "warnings", "any warnings"}
                }},
                RPCExamples{
                    HelpExampleCli("getblockfrompeer", "\"00000000c937983704a73af28acdec37b049d214adbda81d7e2a3dd146f6ed09\" 0")
                + HelpExampleRpc("getblockfrompeer", "\"00000000c937983704a73af28acdec37b049d214adbda81d7e2a3dd146f6ed09\" 0")
                },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    const NodeContext& node = EnsureAnyNodeContext(request.context);
    ChainstateManager& chainman = EnsureChainman(node);
    PeerManager& peerman = EnsurePeerman(node);

    uint256 hash(ParseHashV(request.params[0], "hash"));

    const NodeId nodeid = static_cast<NodeId>(request.params[1].get_int64());

    const CBlockIndex* const index = WITH_LOCK(cs_main, return chainman.m_blockman.LookupBlockIndex(hash););

    UniValue result = UniValue::VOBJ;

    if (index && index->nStatus & BLOCK_HAVE_DATA) {
        result.pushKV("warnings", "Block already downloaded");
    } else if (!peerman.FetchBlock(nodeid, hash, index)) {
        throw JSONRPCError(RPC_MISC_ERROR, "Failed to fetch block from peer");
    }
    return result;
},
    };
}

static RPCHelpMan getblockhash()
{
    return RPCHelpMan{"getblockhash",
                "\nReturns hash of block in best-block-chain at height provided.\n",
                {
                    {"height", RPCArg::Type::NUM, RPCArg::Optional::NO, "The height index"},
                },
                RPCResult{
                    RPCResult::Type::STR_HEX, "", "The block hash"},
                RPCExamples{
                    HelpExampleCli("getblockhash", "1000")
            + HelpExampleRpc("getblockhash", "1000")
                },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    ChainstateManager& chainman = EnsureAnyChainman(request.context);
    LOCK(cs_main);
    const CChain& active_chain = chainman.ActiveChain();

    int nHeight = request.params[0].get_int();
    if (nHeight < 0 || nHeight > active_chain.Height())
        throw JSONRPCError(RPC_INVALID_PARAMETER, "Block height out of range");

    CBlockIndex* pblockindex = active_chain[nHeight];
    return pblockindex->GetBlockHash().GetHex();
},
    };
}

static RPCHelpMan getblockheader()
{
    return RPCHelpMan{"getblockheader",
                "\nIf verbose is false, returns a string that is serialized, hex-encoded data for blockheader 'hash'.\n"
                "If verbose is true, returns an Object with information about blockheader <hash>.\n",
                {
                    {"blockhash", RPCArg::Type::STR_HEX, RPCArg::Optional::NO, "The block hash"},
                    {"verbose", RPCArg::Type::BOOL, RPCArg::Default{true}, "true for a json object, false for the hex-encoded data"},
                },
                {
                    RPCResult{"for verbose = true",
                        RPCResult::Type::OBJ, "", "",
                        {
                            {RPCResult::Type::STR_HEX, "hash", "the block hash (same as provided)"},
                            {RPCResult::Type::NUM, "confirmations", "The number of confirmations, or -1 if the block is not on the main chain"},
                            {RPCResult::Type::NUM, "height", "The block height or index"},
                            {RPCResult::Type::NUM, "version", "The block version"},
                            {RPCResult::Type::STR_HEX, "versionHex", "The block version formatted in hexadecimal"},
                            {RPCResult::Type::STR_HEX, "merkleroot", "The merkle root"},
                            {RPCResult::Type::NUM_TIME, "time", "The block time expressed in " + UNIX_EPOCH_TIME},
                            {RPCResult::Type::NUM_TIME, "mediantime", "The median block time expressed in " + UNIX_EPOCH_TIME},
                            {RPCResult::Type::NUM, "nonce", "The nonce"},
                            {RPCResult::Type::STR_HEX, "bits", "The bits"},
                            {RPCResult::Type::NUM, "difficulty", "The difficulty"},
                            {RPCResult::Type::STR_HEX, "chainwork", "Expected number of hashes required to produce the current chain"},
                            {RPCResult::Type::NUM, "nTx", "The number of transactions in the block"},
                            {RPCResult::Type::STR_HEX, "previousblockhash", /* optional */ true, "The hash of the previous block (if available)"},
                            {RPCResult::Type::STR_HEX, "nextblockhash", /* optional */ true, "The hash of the next block (if available)"},
                        }},
                    RPCResult{"for verbose=false",
                        RPCResult::Type::STR_HEX, "", "A string that is serialized, hex-encoded data for block 'hash'"},
                },
                RPCExamples{
                    HelpExampleCli("getblockheader", "\"00000000c937983704a73af28acdec37b049d214adbda81d7e2a3dd146f6ed09\"")
            + HelpExampleRpc("getblockheader", "\"00000000c937983704a73af28acdec37b049d214adbda81d7e2a3dd146f6ed09\"")
                },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    uint256 hash(ParseHashV(request.params[0], "hash"));

    bool fVerbose = true;
    if (!request.params[1].isNull())
        fVerbose = request.params[1].get_bool();

    const CBlockIndex* pblockindex;
    const CBlockIndex* tip;
    {
        ChainstateManager& chainman = EnsureAnyChainman(request.context);
        LOCK(cs_main);
        pblockindex = chainman.m_blockman.LookupBlockIndex(hash);
        tip = chainman.ActiveChain().Tip();
    }

    if (!pblockindex) {
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Block not found");
    }

    if (!fVerbose)
    {
        CDataStream ssBlock(SER_NETWORK, PROTOCOL_VERSION);
        ssBlock << pblockindex->GetBlockHeader();
        std::string strHex = HexStr(ssBlock);
        return strHex;
    }

    return blockheaderToJSON(tip, pblockindex);
},
    };
}

static CBlock GetBlockChecked(const CBlockIndex* pblockindex)
{
    CBlock block;
    if (IsBlockPruned(pblockindex)) {
        throw JSONRPCError(RPC_MISC_ERROR, "Block not available (pruned data)");
    }

    if (!ReadBlockFromDisk(block, pblockindex, Params().GetConsensus())) {
        // Block not found on disk. This could be because we have the block
        // header in our index but not yet have the block or did not accept the
        // block.
        throw JSONRPCError(RPC_MISC_ERROR, "Block not found on disk");
    }

    return block;
}

static CBlockUndo GetUndoChecked(const CBlockIndex* pblockindex)
{
    CBlockUndo blockUndo;

    // The Genesis block does not have undo data
    if (pblockindex->nHeight == 0) return blockUndo;

    if (IsBlockPruned(pblockindex)) {
        throw JSONRPCError(RPC_MISC_ERROR, "Undo data not available (pruned data)");
    }

    if (!UndoReadFromDisk(blockUndo, pblockindex)) {
        throw JSONRPCError(RPC_MISC_ERROR, "Can't read undo data from disk");
    }

    return blockUndo;
}

/// Requires cs_main
static void ThrowIfPrunedBlock(const CBlockIndex *pblockindex) EXCLUSIVE_LOCKS_REQUIRED(cs_main) {
    AssertLockHeld(::cs_main);
    if (IsBlockPruned(pblockindex)) {
        throw JSONRPCError(RPC_MISC_ERROR, "Block not available (pruned data)");
    }
}

/// Lock-free -- will throw if block not found or was pruned, etc. Guaranteed to return a valid block or fail.
static CBlock ReadBlockChecked(const Config &config, const CBlockIndex *pblockindex) {
    CBlock block;
    if (!ReadBlockFromDisk(block, pblockindex,
                           config.GetChainParams().GetConsensus())) {
        // Block not found on disk. This could be because we have the block
        // header in our index but don't have the block (for example if a
        // non-whitelisted node sends us an unrequested long chain of valid
        // blocks, we add the headers to our index, but don't accept the block).
        throw JSONRPCError(RPC_MISC_ERROR, "Block not found on disk");
    }

    return block;
}

static RPCHelpMan getblock()
{
    return RPCHelpMan{"getblock",
                "\nIf verbosity is 0, returns a string that is serialized, hex-encoded data for block 'hash'.\n"
                "If verbosity is 1, returns an Object with information about block <hash>.\n"
                "If verbosity is 2, returns an Object with information about block <hash> and information about each transaction.\n"
                "If verbosity is 3, returns an Object with information about block <hash> and information about each transaction, including prevout information for inputs (only for unpruned blocks in the current best chain).\n",
                {
                    {"blockhash", RPCArg::Type::STR_HEX, RPCArg::Optional::NO, "The block hash"},
                    {"verbosity|verbose", RPCArg::Type::NUM, RPCArg::Default{1}, "0 for hex-encoded data, 1 for a json object, and 2 for json object with transaction data"},
                },
                {
                    RPCResult{"for verbosity = 0",
                RPCResult::Type::STR_HEX, "", "A string that is serialized, hex-encoded data for block 'hash'"},
                    RPCResult{"for verbosity = 1",
                RPCResult::Type::OBJ, "", "",
                {
                    {RPCResult::Type::STR_HEX, "hash", "the block hash (same as provided)"},
                    {RPCResult::Type::NUM, "confirmations", "The number of confirmations, or -1 if the block is not on the main chain"},
                    {RPCResult::Type::NUM, "size", "The block size"},
                    {RPCResult::Type::NUM, "strippedsize", "The block size excluding witness data"},
                    {RPCResult::Type::NUM, "weight", "The block weight as defined in BIP 141"},
                    {RPCResult::Type::NUM, "height", "The block height or index"},
                    {RPCResult::Type::NUM, "version", "The block version"},
                    {RPCResult::Type::STR_HEX, "versionHex", "The block version formatted in hexadecimal"},
                    {RPCResult::Type::STR_HEX, "merkleroot", "The merkle root"},
                    {RPCResult::Type::ARR, "tx", "The transaction ids",
                        {{RPCResult::Type::STR_HEX, "", "The transaction id"}}},
                    {RPCResult::Type::NUM_TIME, "time",       "The block time expressed in " + UNIX_EPOCH_TIME},
                    {RPCResult::Type::NUM_TIME, "mediantime", "The median block time expressed in " + UNIX_EPOCH_TIME},
                    {RPCResult::Type::NUM, "nonce", "The nonce"},
                    {RPCResult::Type::STR_HEX, "bits", "The bits"},
                    {RPCResult::Type::NUM, "difficulty", "The difficulty"},
                    {RPCResult::Type::STR_HEX, "chainwork", "Expected number of hashes required to produce the chain up to this block (in hex)"},
                    {RPCResult::Type::NUM, "nTx", "The number of transactions in the block"},
                    {RPCResult::Type::STR_HEX, "previousblockhash", /* optional */ true, "The hash of the previous block (if available)"},
                    {RPCResult::Type::STR_HEX, "nextblockhash", /* optional */ true, "The hash of the next block (if available)"},
                }},
                    RPCResult{"for verbosity = 2",
                RPCResult::Type::OBJ, "", "",
                {
                    {RPCResult::Type::ELISION, "", "Same output as verbosity = 1"},
                    {RPCResult::Type::ARR, "tx", "",
                    {
                        {RPCResult::Type::OBJ, "", "",
                        {
                            {RPCResult::Type::ELISION, "", "The transactions in the format of the getrawtransaction RPC. Different from verbosity = 1 \"tx\" result"},
                            {RPCResult::Type::NUM, "fee", "The transaction fee in " + CURRENCY_UNIT + ", omitted if block undo data is not available"},
                        }},
                    }},
                }},
                    RPCResult{"for verbosity = 3",
                RPCResult::Type::OBJ, "", "",
                {
                    {RPCResult::Type::ELISION, "", "Same output as verbosity = 2"},
                    {RPCResult::Type::ARR, "tx", "",
                    {
                        {RPCResult::Type::OBJ, "", "",
                        {
                            {RPCResult::Type::ARR, "vin", "",
                            {
                                {RPCResult::Type::OBJ, "", "",
                                {
                                    {RPCResult::Type::ELISION, "", "The same output as verbosity = 2"},
                                    {RPCResult::Type::OBJ, "prevout", "",
                                    {
                                        {RPCResult::Type::BOOL, "generated", "Coinbase or not"},
                                        {RPCResult::Type::NUM, "height", "The height of the prevout"},
                                        {RPCResult::Type::NUM, "value", "The value in " + CURRENCY_UNIT},
                                        {RPCResult::Type::OBJ, "scriptPubKey", "",
                                        {
                                            {RPCResult::Type::STR, "asm", "The asm"},
                                            {RPCResult::Type::STR, "hex", "The hex"},
                                            {RPCResult::Type::STR, "address", /* optional */ true, "The Bitcoin address (only if a well-defined address exists)"},
                                            {RPCResult::Type::STR, "type", "The type, eg 'pubkeyhash'"},
                                        }},
                                    }},
                                }},
                            }},
                        }},
                    }},
                }},
        },
                RPCExamples{
                    HelpExampleCli("getblock", "\"00000000c937983704a73af28acdec37b049d214adbda81d7e2a3dd146f6ed09\"")
            + HelpExampleRpc("getblock", "\"00000000c937983704a73af28acdec37b049d214adbda81d7e2a3dd146f6ed09\"")
                },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    uint256 hash(ParseHashV(request.params[0], "blockhash"));

    int verbosity = 1;
    if (!request.params[1].isNull()) {
        if (request.params[1].isBool()) {
            verbosity = request.params[1].get_bool() ? 1 : 0;
        } else {
            verbosity = request.params[1].get_int();
        }
    }

    CBlock block;
    std::vector<uint8_t> raw_block;
    const CBlockIndex* pblockindex;
    const CBlockIndex* tip;
    {
        ChainstateManager& chainman = EnsureAnyChainman(request.context);
        LOCK(cs_main);
        pblockindex = chainman.m_blockman.LookupBlockIndex(hash);
        tip = chainman.ActiveChain().Tip();

        if (!pblockindex) {
            throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Block not found");
        }

        ThrowIfPrunedBlock(pblockindex);
    }


    if (verbosity <= 0 && !RPCSerializationFlags()) {
        // This one case doesn't need to parse the block at all
        if (!ReadRawBlockFromDisk(raw_block, pblockindex, Params().MessageStart())) {
            throw JSONRPCError(RPC_MISC_ERROR, "Block not found on disk");
        }
    } else {
        block = ReadBlockChecked(pblockindex);
    }

    if (verbosity <= 0)
    {
        std::string strHex;
        if (raw_block.empty()) {
            CDataStream ssBlock(SER_NETWORK, PROTOCOL_VERSION | RPCSerializationFlags());
            ssBlock << block;
            strHex = HexStr(ssBlock);
        } else {
            strHex = HexStr(raw_block);
        }
        return strHex;
    }

    TxVerbosity tx_verbosity;
    if (verbosity == 1) {
        tx_verbosity = TxVerbosity::SHOW_TXID;
    } else if (verbosity == 2) {
        tx_verbosity = TxVerbosity::SHOW_DETAILS;
    } else {
        tx_verbosity = TxVerbosity::SHOW_DETAILS_AND_PREVOUT;
    }

    return blockToJSON(block, tip, pblockindex, tx_verbosity);
},
    };
}

static RPCHelpMan listprunelocks()
{
    return RPCHelpMan{"listprunelocks",
        "\nReturns a list of pruning locks.\n",
        {},
        RPCResult{
            RPCResult::Type::OBJ, "", "",
            {
                {RPCResult::Type::ARR, "prune_locks", "",
                {
                    {RPCResult::Type::OBJ, "", "",
                    {
                        {RPCResult::Type::STR, "id", "A unique identifier for the lock"},
                        {RPCResult::Type::STR, "desc", "A description of the lock's purpose"},
                        {RPCResult::Type::ARR_FIXED, "height", "Range of blocks prevented from being pruned",
                        {
                            {RPCResult::Type::NUM, "height_first", "Height of first block that may not be pruned"},
                            {RPCResult::Type::NUM, "height_last", "Height of last block that may not be pruned (omitted if unbounded)"},
                        }},
                        {RPCResult::Type::BOOL, "temporary", "Indicates the lock will not remain after a restart of the node"},
                    }},
                }},
            }
        },
        RPCExamples{
            HelpExampleCli("listprunelocks", "")
          + HelpExampleRpc("listprunelocks", "")
        },
    [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    UniValue locks_uv(UniValue::VARR);
    {
        LOCK(g_prune_locks_mutex);
        for (const auto& prune_lock : g_prune_locks) {
            UniValue prune_lock_uv(UniValue::VOBJ);
            const auto& lockinfo = prune_lock.second;
            prune_lock_uv.pushKV("id", prune_lock.first);
            prune_lock_uv.pushKV("desc", lockinfo.m_desc);
            UniValue heights_uv(UniValue::VARR);
            heights_uv.push_back(lockinfo.m_height_first);
            if (lockinfo.m_height_last < std::numeric_limits<uint64_t>::max()) {
                heights_uv.push_back(lockinfo.m_height_last);
            }
            prune_lock_uv.pushKV("height", heights_uv);
            locks_uv.push_back(prune_lock_uv);
        }
    }

    UniValue result(UniValue::VOBJ);
    result.pushKV("prune_locks", locks_uv);
    return result;
},
    };
}

static RPCHelpMan setprunelock()
{
    return RPCHelpMan{"setprunelock",
        "\nManipulate pruning locks.\n",
        {
            {"id", RPCArg::Type::STR, RPCArg::Optional::NO, "The unique id of the manipulated prune lock (or \"*\" if deleting all)"},
            {"lock_info", RPCArg::Type::OBJ, RPCArg::Optional::NO, "An object describing the desired lock",
                {
                    {"desc", RPCArg::Type::STR, RPCArg::Optional::NO, "Description of the lock"},
                    {"height", RPCArg::Type::RANGE, RPCArg::DefaultHint("deletes the lock"), "The range of block heights to prevent pruning"},
                    {"sync", RPCArg::Type::BOOL, RPCArg::Default(false), "If true, success indicates the lock change was stored to disk (if non-temporary). If false, it is possible for a subsequent node crash to lose the lock."},
                    {"temporary", RPCArg::Type::BOOL, RPCArg::Default(false), "If true, the lock will not persist across node restart."},
                },
            },
        },
        RPCResult{
            RPCResult::Type::OBJ, "", "",
            {
                {RPCResult::Type::BOOL, "success", "Whether the change was successful"},
            }},
        RPCExamples{
            HelpExampleCli("setprunelock", "\"test\" \"{\\\"desc\\\": \\\"Just a test\\\", \\\"height\\\": [0,100]}\"")
          + HelpExampleCli("setprunelock", "\"test-2\" \"{\\\"desc\\\": \\\"Second RPC-created prunelock test\\\", \\\"height\\\": [100]}\"")
          + HelpExampleRpc("setprunelock", "\"test\", {\"desc\": \"Just a test\", \"height\": [0,100]}")
        },
    [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    RPCTypeCheck(request.params, {UniValue::VSTR, UniValue::VOBJ});
    const auto& lockinfo_json = request.params[1];
    RPCTypeCheckObj(lockinfo_json,
        {
            {"desc", UniValueType(UniValue::VSTR)},
            {"height", UniValueType()}, // will be checked below
            {"sync", UniValueType(UniValue::VBOOL)},
            {"temporary", UniValueType(UniValue::VBOOL)},
        },
        /*allow_null=*/ true, /*strict=*/ true);

    const auto& lockid = request.params[0].get_str();

    PruneLockInfo lockinfo;

    auto height_param = lockinfo_json["height"];
    if (!height_param.isArray()) {
        UniValue new_height_param(UniValue::VARR);
        new_height_param.push_back(std::move(height_param));
        height_param = std::move(new_height_param);
    }
    bool success;
    if (height_param[0].isNull() && height_param[1].isNull()) {
        // Delete
        if (lockid == "*") {
            // Delete all
            success = true;
            std::vector<std::string> all_ids;
            LOCK(g_prune_locks_mutex);
            for (const auto& prune_lock : g_prune_locks) {
                all_ids.push_back(prune_lock.first);
            }
            for (auto& lockid : all_ids) {
                success |= DeletePruneLock(lockid);
            }
        } else {
            LOCK(g_prune_locks_mutex);
            success = PruneLockExists(lockid) && DeletePruneLock(lockid);
        }
    } else {
        if (lockid == "*") throw JSONRPCError(RPC_INVALID_PARAMETER, "id \"*\" only makes sense when deleting");
        if (!height_param[0].isNum()) throw JSONRPCError(RPC_TYPE_ERROR, "Invalid start height");
        lockinfo.m_height_first = height_param[0].get_int64();
        if (!height_param[1].isNull()) {
            if (!height_param[1].isNum()) throw JSONRPCError(RPC_TYPE_ERROR, "Invalid end height");
            lockinfo.m_height_last = height_param[1].get_int64();
        }
        lockinfo.m_desc = lockinfo_json["desc"].get_str();
        if (!lockinfo_json["temporary"].isNull()) {
            lockinfo.m_temporary = lockinfo_json["temporary"].get_bool();
        }
        bool sync = false;
        if (!lockinfo_json["sync"].isNull()) {
            sync = lockinfo_json["sync"].get_bool();
        }
        LOCK(g_prune_locks_mutex);
        success = SetPruneLock(lockid, lockinfo, sync);
    }

    UniValue result(UniValue::VOBJ);
    result.pushKV("success", success);
    return result;
},
    };
}

static RPCHelpMan pruneblockchain()
{
    return RPCHelpMan{"pruneblockchain", "",
                {
                    {"height", RPCArg::Type::NUM, RPCArg::Optional::NO, "The block height to prune up to. May be set to a discrete height, or to a " + UNIX_EPOCH_TIME + "\n"
            "                  to prune blocks whose block time is at least 2 hours older than the provided timestamp."},
                },
                RPCResult{
                    RPCResult::Type::NUM, "", "Height of the last block pruned"},
                RPCExamples{
                    HelpExampleCli("pruneblockchain", "1000")
            + HelpExampleRpc("pruneblockchain", "1000")
                },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    if (!fPruneMode)
        throw JSONRPCError(RPC_MISC_ERROR, "Cannot prune blocks because node is not in prune mode.");

    ChainstateManager& chainman = EnsureAnyChainman(request.context);
    LOCK(cs_main);
    CChainState& active_chainstate = chainman.ActiveChainstate();
    CChain& active_chain = active_chainstate.m_chain;

    int heightParam = request.params[0].get_int();
    if (heightParam < 0)
        throw JSONRPCError(RPC_INVALID_PARAMETER, "Negative block height.");
    if (heightParam == 0) {
        // Nothing to do here
        return uint64_t(0);
    }

    // Height value more than a billion is too high to be a block height, and
    // too low to be a block time (corresponds to timestamp from Sep 2001).
    if (heightParam > 1000000000) {
        // Add a 2 hour buffer to include blocks which might have had old timestamps
        CBlockIndex* pindex = active_chain.FindEarliestAtLeast(heightParam - TIMESTAMP_WINDOW, 0);
        if (!pindex) {
            throw JSONRPCError(RPC_INVALID_PARAMETER, "Could not find block with at least the specified timestamp.");
        }
        heightParam = pindex->nHeight;
    }

    unsigned int height = (unsigned int) heightParam;
    unsigned int chainHeight = (unsigned int) active_chain.Height();
    if (chainHeight < Params().PruneAfterHeight())
        throw JSONRPCError(RPC_MISC_ERROR, "Blockchain is too short for pruning.");
    else if (height > chainHeight)
        throw JSONRPCError(RPC_INVALID_PARAMETER, "Blockchain is shorter than the attempted prune height.");
    else if (height > chainHeight - MIN_BLOCKS_TO_KEEP) {
        LogPrint(BCLog::RPC, "Attempt to prune blocks close to the tip.  Retaining the minimum number of blocks.\n");
        height = chainHeight - MIN_BLOCKS_TO_KEEP;
    }

    PruneBlockFilesManual(active_chainstate, height);
    const CBlockIndex* block = active_chain.Tip();
    CHECK_NONFATAL(block);
    while (block->pprev && (block->pprev->nStatus & BLOCK_HAVE_DATA)) {
        block = block->pprev;
    }
    return uint64_t(block->nHeight);
},
    };
}

CoinStatsHashType ParseHashType(const std::string& hash_type_input)
{
    if (hash_type_input == "hash_serialized_2") {
        return CoinStatsHashType::HASH_SERIALIZED;
    } else if (hash_type_input == "muhash") {
        return CoinStatsHashType::MUHASH;
    } else if (hash_type_input == "none") {
        return CoinStatsHashType::NONE;
    } else {
        throw JSONRPCError(RPC_INVALID_PARAMETER, strprintf("%s is not a valid hash_type", hash_type_input));
    }
}

static RPCHelpMan gettxoutsetinfo()
{
    return RPCHelpMan{"gettxoutsetinfo",
                "\nReturns statistics about the unspent transaction output set.\n"
                "Note this call may take some time if you are not using coinstatsindex.\n",
                {
                    {"hash_type", RPCArg::Type::STR, RPCArg::Default{"hash_serialized_2"}, "Which UTXO set hash should be calculated. Options: 'hash_serialized_2' (the legacy algorithm), 'muhash', 'none'."},
                    {"hash_or_height", RPCArg::Type::NUM, RPCArg::Optional::OMITTED_NAMED_ARG, "The block hash or height of the target height (only available with coinstatsindex).", "", {"", "string or numeric"}},
                    {"use_index", RPCArg::Type::BOOL, RPCArg::Default{true}, "Use coinstatsindex, if available."},
                },
                RPCResult{
                    RPCResult::Type::OBJ, "", "",
                    {
                        {RPCResult::Type::NUM, "height", "The block height (index) of the returned statistics"},
                        {RPCResult::Type::STR_HEX, "bestblock", "The hash of the block at which these statistics are calculated"},
                        {RPCResult::Type::NUM, "txouts", "The number of unspent transaction outputs"},
                        {RPCResult::Type::NUM, "bogosize", "Database-independent, meaningless metric indicating the UTXO set size"},
                        {RPCResult::Type::STR_HEX, "hash_serialized_2", /* optional */ true, "The serialized hash (only present if 'hash_serialized_2' hash_type is chosen)"},
                        {RPCResult::Type::STR_HEX, "muhash", /* optional */ true, "The serialized hash (only present if 'muhash' hash_type is chosen)"},
                        {RPCResult::Type::NUM, "transactions", "The number of transactions with unspent outputs (not available when coinstatsindex is used)"},
                        {RPCResult::Type::NUM, "disk_size", "The estimated size of the chainstate on disk (not available when coinstatsindex is used)"},
                        {RPCResult::Type::STR_AMOUNT, "total_amount", "The total amount of coins in the UTXO set"},
                        {RPCResult::Type::STR_AMOUNT, "total_unspendable_amount", "The total amount of coins permanently excluded from the UTXO set (only available if coinstatsindex is used)"},
                        {RPCResult::Type::OBJ, "block_info", "Info on amounts in the block at this block height (only available if coinstatsindex is used)",
                        {
                            {RPCResult::Type::STR_AMOUNT, "prevout_spent", "Total amount of all prevouts spent in this block"},
                            {RPCResult::Type::STR_AMOUNT, "coinbase", "Coinbase subsidy amount of this block"},
                            {RPCResult::Type::STR_AMOUNT, "new_outputs_ex_coinbase", "Total amount of new outputs created by this block"},
                            {RPCResult::Type::STR_AMOUNT, "unspendable", "Total amount of unspendable outputs created in this block"},
                            {RPCResult::Type::OBJ, "unspendables", "Detailed view of the unspendable categories",
                            {
                                {RPCResult::Type::STR_AMOUNT, "genesis_block", "The unspendable amount of the Genesis block subsidy"},
                                {RPCResult::Type::STR_AMOUNT, "bip30", "Transactions overridden by duplicates (no longer possible with BIP30)"},
                                {RPCResult::Type::STR_AMOUNT, "scripts", "Amounts sent to scripts that are unspendable (for example OP_RETURN outputs)"},
                                {RPCResult::Type::STR_AMOUNT, "unclaimed_rewards", "Fee rewards that miners did not claim in their coinbase transaction"},
                            }}
                        }},
                    }},
                RPCExamples{
                    HelpExampleCli("gettxoutsetinfo", "") +
                    HelpExampleCli("gettxoutsetinfo", R"("none")") +
                    HelpExampleCli("gettxoutsetinfo", R"("none" 1000)") +
                    HelpExampleCli("gettxoutsetinfo", R"("none" '"00000000c937983704a73af28acdec37b049d214adbda81d7e2a3dd146f6ed09"')") +
                    HelpExampleRpc("gettxoutsetinfo", "") +
                    HelpExampleRpc("gettxoutsetinfo", R"("none")") +
                    HelpExampleRpc("gettxoutsetinfo", R"("none", 1000)") +
                    HelpExampleRpc("gettxoutsetinfo", R"("none", "00000000c937983704a73af28acdec37b049d214adbda81d7e2a3dd146f6ed09")")
                },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    UniValue ret(UniValue::VOBJ);

    CBlockIndex* pindex{nullptr};
    const CoinStatsHashType hash_type{request.params[0].isNull() ? CoinStatsHashType::HASH_SERIALIZED : ParseHashType(request.params[0].get_str())};
    CCoinsStats stats{hash_type};
    stats.index_requested = request.params[2].isNull() || request.params[2].get_bool();

    NodeContext& node = EnsureAnyNodeContext(request.context);
    ChainstateManager& chainman = EnsureChainman(node);
    CChainState& active_chainstate = chainman.ActiveChainstate();
    active_chainstate.ForceFlushStateToDisk();

    CCoinsView* coins_view;
    BlockManager* blockman;
    {
        LOCK(::cs_main);
        coins_view = &active_chainstate.CoinsDB();
        blockman = &active_chainstate.m_blockman;
        pindex = blockman->LookupBlockIndex(coins_view->GetBestBlock());
    }

    if (!request.params[1].isNull()) {
        if (!g_coin_stats_index) {
            throw JSONRPCError(RPC_INVALID_PARAMETER, "Querying specific block heights requires coinstatsindex");
        }

        if (stats.m_hash_type == CoinStatsHashType::HASH_SERIALIZED) {
            throw JSONRPCError(RPC_INVALID_PARAMETER, "hash_serialized_2 hash type cannot be queried for a specific block");
        }

        pindex = ParseHashOrHeight(request.params[1], chainman);
    }

    if (stats.index_requested && g_coin_stats_index) {
        if (!g_coin_stats_index->BlockUntilSyncedToCurrentChain()) {
            const IndexSummary summary{g_coin_stats_index->GetSummary()};

            // If a specific block was requested and the index has already synced past that height, we can return the
            // data already even though the index is not fully synced yet.
            if (pindex->nHeight > summary.best_block_height) {
                throw JSONRPCError(RPC_INTERNAL_ERROR, strprintf("Unable to get data because coinstatsindex is still syncing. Current height: %d", summary.best_block_height));
            }
        }
    }

    if (GetUTXOStats(coins_view, *blockman, stats, node.rpc_interruption_point, pindex)) {
        ret.pushKV("height", (int64_t)stats.nHeight);
        ret.pushKV("bestblock", stats.hashBlock.GetHex());
        ret.pushKV("txouts", (int64_t)stats.nTransactionOutputs);
        ret.pushKV("bogosize", (int64_t)stats.nBogoSize);
        if (hash_type == CoinStatsHashType::HASH_SERIALIZED) {
            ret.pushKV("hash_serialized_2", stats.hashSerialized.GetHex());
        }
        if (hash_type == CoinStatsHashType::MUHASH) {
              ret.pushKV("muhash", stats.hashSerialized.GetHex());
        }
        ret.pushKV("total_amount", ValueFromAmount(stats.nTotalAmount));
        if (!stats.index_used) {
            ret.pushKV("transactions", static_cast<int64_t>(stats.nTransactions));
            ret.pushKV("disk_size", stats.nDiskSize);
        } else {
            ret.pushKV("total_unspendable_amount", ValueFromAmount(stats.block_unspendable_amount));

            CCoinsStats prev_stats{hash_type};

            if (pindex->nHeight > 0) {
                GetUTXOStats(coins_view, *blockman, prev_stats, node.rpc_interruption_point, pindex->pprev);
            }

            UniValue block_info(UniValue::VOBJ);
            block_info.pushKV("prevout_spent", ValueFromAmount(stats.block_prevout_spent_amount - prev_stats.block_prevout_spent_amount));
            block_info.pushKV("coinbase", ValueFromAmount(stats.block_coinbase_amount - prev_stats.block_coinbase_amount));
            block_info.pushKV("new_outputs_ex_coinbase", ValueFromAmount(stats.block_new_outputs_ex_coinbase_amount - prev_stats.block_new_outputs_ex_coinbase_amount));
            block_info.pushKV("unspendable", ValueFromAmount(stats.block_unspendable_amount - prev_stats.block_unspendable_amount));

            UniValue unspendables(UniValue::VOBJ);
            unspendables.pushKV("genesis_block", ValueFromAmount(stats.unspendables_genesis_block - prev_stats.unspendables_genesis_block));
            unspendables.pushKV("bip30", ValueFromAmount(stats.unspendables_bip30 - prev_stats.unspendables_bip30));
            unspendables.pushKV("scripts", ValueFromAmount(stats.unspendables_scripts - prev_stats.unspendables_scripts));
            unspendables.pushKV("unclaimed_rewards", ValueFromAmount(stats.unspendables_unclaimed_rewards - prev_stats.unspendables_unclaimed_rewards));
            block_info.pushKV("unspendables", unspendables);

            ret.pushKV("block_info", block_info);
        }
    } else {
        throw JSONRPCError(RPC_INTERNAL_ERROR, "Unable to read UTXO set");
    }
    return ret;
},
    };
}

static RPCHelpMan gettxout()
{
    return RPCHelpMan{"gettxout",
        "\nReturns details about an unspent transaction output.\n",
        {
            {"txid", RPCArg::Type::STR, RPCArg::Optional::NO, "The transaction id"},
            {"n", RPCArg::Type::NUM, RPCArg::Optional::NO, "vout number"},
            {"include_mempool", RPCArg::Type::BOOL, RPCArg::Default{true}, "Whether to include the mempool. Note that an unspent output that is spent in the mempool won't appear."},
        },
        {
            RPCResult{"If the UTXO was not found", RPCResult::Type::NONE, "", ""},
            RPCResult{"Otherwise", RPCResult::Type::OBJ, "", "", {
                {RPCResult::Type::STR_HEX, "bestblock", "The hash of the block at the tip of the chain"},
                {RPCResult::Type::NUM, "confirmations", "The number of confirmations"},
                {RPCResult::Type::STR_AMOUNT, "value", "The transaction value in " + CURRENCY_UNIT},
                {RPCResult::Type::OBJ, "scriptPubKey", "", {
                    {RPCResult::Type::STR, "asm", ""},
                    {RPCResult::Type::STR, "desc", "Inferred descriptor for the output\n"},
                    {RPCResult::Type::STR_HEX, "hex", ""},
                    {RPCResult::Type::NUM, "reqSigs", /* optional */ true, "(DEPRECATED, returned only if config option -deprecatedrpc=addresses is passed) Number of required signatures"},
                    {RPCResult::Type::STR, "type", "The type, eg pubkeyhash"},
                    {RPCResult::Type::STR, "address", /* optional */ true, "bitcoin address (only if a well-defined address exists)"},
                    {RPCResult::Type::ARR, "addresses", /* optional */ true, "(DEPRECATED, returned only if config option -deprecatedrpc=addresses is passed) Array of bitcoin addresses",
                        {{RPCResult::Type::STR, "address", "bitcoin address"}}},
                }},
                {RPCResult::Type::BOOL, "coinbase", "Coinbase or not"},
            }},
        },
        RPCExamples{
            "\nGet unspent transactions\n"
            + HelpExampleCli("listunspent", "") +
            "\nView the details\n"
            + HelpExampleCli("gettxout", "\"txid\" 1") +
            "\nAs a JSON-RPC call\n"
            + HelpExampleRpc("gettxout", "\"txid\", 1")
                },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    NodeContext& node = EnsureAnyNodeContext(request.context);
    ChainstateManager& chainman = EnsureChainman(node);
    LOCK(cs_main);

    UniValue ret(UniValue::VOBJ);

    uint256 hash(ParseHashV(request.params[0], "txid"));
    int n = request.params[1].get_int();
    COutPoint out(hash, n);
    bool fMempool = true;
    if (!request.params[2].isNull())
        fMempool = request.params[2].get_bool();

    Coin coin;
    CChainState& active_chainstate = chainman.ActiveChainstate();
    CCoinsViewCache* coins_view = &active_chainstate.CoinsTip();

    if (fMempool) {
        const CTxMemPool& mempool = EnsureMemPool(node);
        LOCK(mempool.cs);
        CCoinsViewMemPool view(coins_view, mempool);
        if (!view.GetCoin(out, coin) || mempool.isSpent(out)) {
            return NullUniValue;
        }
    } else {
        if (!coins_view->GetCoin(out, coin)) {
            return NullUniValue;
        }
    }

    const CBlockIndex* pindex = active_chainstate.m_blockman.LookupBlockIndex(coins_view->GetBestBlock());
    ret.pushKV("bestblock", pindex->GetBlockHash().GetHex());
    if (coin.nHeight == MEMPOOL_HEIGHT) {
        ret.pushKV("confirmations", 0);
    } else {
        ret.pushKV("confirmations", (int64_t)(pindex->nHeight - coin.nHeight + 1));
    }
    ret.pushKV("value", ValueFromAmount(coin.out.nValue));
    UniValue o(UniValue::VOBJ);
    ScriptPubKeyToUniv(coin.out.scriptPubKey, o, true);
    ret.pushKV("scriptPubKey", o);
    ret.pushKV("coinbase", (bool)coin.fCoinBase);

    return ret;
},
    };
}

static RPCHelpMan verifychain()
{
    return RPCHelpMan{"verifychain",
                "\nVerifies blockchain database.\n",
                {
                    {"checklevel", RPCArg::Type::NUM, RPCArg::DefaultHint{strprintf("%d, range=0-4", DEFAULT_CHECKLEVEL)},
                        strprintf("How thorough the block verification is:\n - %s", Join(CHECKLEVEL_DOC, "\n- "))},
                    {"nblocks", RPCArg::Type::NUM, RPCArg::DefaultHint{strprintf("%d, 0=all", DEFAULT_CHECKBLOCKS)}, "The number of blocks to check."},
                },
                RPCResult{
                    RPCResult::Type::BOOL, "", "Verified or not"},
                RPCExamples{
                    HelpExampleCli("verifychain", "")
            + HelpExampleRpc("verifychain", "")
                },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    const int check_level(request.params[0].isNull() ? DEFAULT_CHECKLEVEL : request.params[0].get_int());
    const int check_depth{request.params[1].isNull() ? DEFAULT_CHECKBLOCKS : request.params[1].get_int()};

    ChainstateManager& chainman = EnsureAnyChainman(request.context);
    LOCK(cs_main);

    CChainState& active_chainstate = chainman.ActiveChainstate();
    return CVerifyDB().VerifyDB(
        active_chainstate, Params(), active_chainstate.CoinsTip(), check_level, check_depth);
},
    };
}

static RPCHelpMan scriptthreadsinfo()
{
    return RPCHelpMan{"scriptthreadsinfo",
                "\nShow information about the script verification threads.\n",
                {},
                RPCResult{
                    RPCResult::Type::OBJ, "", "",
                    {
                        {RPCResult::Type::BOOL, "enabled", "true is script verification threads are enabled (see setscriptthreadsenabled)."},
                        {RPCResult::Type::NUM, "num_script_check_threads", "The total number of script verification threads."},
                    },
                },
                RPCExamples{
                    HelpExampleCli("scriptthreadsinfo", "")
            + HelpExampleRpc("scriptthreadsinfo", "")
                },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    UniValue ret(UniValue::VOBJ);
    ret.pushKV("enabled", g_parallel_script_checks);
    ret.pushKV("num_script_check_threads", nScriptCheckThreads);
    return ret;
},
    };
}

static RPCHelpMan setscriptthreadsenabled()
{
    return RPCHelpMan{"setscriptthreadsenabled",
                "\nDisable/enable script verification threads and therefore reducing CPU usage on multicore systems.\n"
                "Disabling script verification threads may result in a significant slow-down during synchronisation.\n"
                "Has no effect on single core machines or if started with -par=<-<numcores>\n",
                {
                    {"state", RPCArg::Type::BOOL, RPCArg::Optional::NO, "false if script verification threads should be disabled (true for re-enabling)"},
                },
                RPCResult{RPCResult::Type::NONE, "", ""},
                RPCExamples{
                    HelpExampleCli("setscriptthreadsenabled", "false")
            + HelpExampleRpc("setscriptthreadsenabled", "false")
                },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{

    if (nScriptCheckThreads == 0) {
        throw JSONRPCError(RPC_MISC_ERROR, "Script verification threads are disabled (single core machine or -par=<-<numcores>)");
    }

    LOCK(cs_main);
    g_parallel_script_checks = request.params[0].get_bool();

    return NullUniValue;
},
    };
}

static void SoftForkDescPushBack(const CBlockIndex* active_chain_tip, UniValue& softforks, const Consensus::Params& params, Consensus::BuriedDeployment dep)
{
    // For buried deployments.

    if (!DeploymentEnabled(params, dep)) return;

    UniValue rv(UniValue::VOBJ);
    rv.pushKV("type", "buried");
    // getblockchaininfo reports the softfork as active from when the chain height is
    // one below the activation height
    rv.pushKV("active", DeploymentActiveAfter(active_chain_tip, params, dep));
    rv.pushKV("height", params.DeploymentHeight(dep));
    softforks.pushKV(DeploymentName(dep), rv);
}

static void SoftForkDescPushBack(const CBlockIndex* active_chain_tip, UniValue& softforks, const Consensus::Params& consensusParams, Consensus::DeploymentPos id)
{
    // For BIP9 deployments.

    if (!DeploymentEnabled(consensusParams, id)) return;

    UniValue bip9(UniValue::VOBJ);
    const ThresholdState thresholdState = g_versionbitscache.State(active_chain_tip, consensusParams, id);
    switch (thresholdState) {
    case ThresholdState::DEFINED: bip9.pushKV("status", "defined"); break;
    case ThresholdState::STARTED: bip9.pushKV("status", "started"); break;
    case ThresholdState::LOCKED_IN: bip9.pushKV("status", "locked_in"); break;
    case ThresholdState::ACTIVE: bip9.pushKV("status", "active"); break;
    case ThresholdState::FAILED: bip9.pushKV("status", "failed"); break;
    }
    const bool has_signal = (ThresholdState::STARTED == thresholdState || ThresholdState::LOCKED_IN == thresholdState);
    if (has_signal) {
        bip9.pushKV("bit", consensusParams.vDeployments[id].bit);
    }
    bip9.pushKV("start_time", consensusParams.vDeployments[id].nStartTime);
    bip9.pushKV("timeout", consensusParams.vDeployments[id].nTimeout);
    int64_t since_height = g_versionbitscache.StateSinceHeight(active_chain_tip, consensusParams, id);
    bip9.pushKV("since", since_height);
    if (has_signal) {
        UniValue statsUV(UniValue::VOBJ);
        BIP9Stats statsStruct = g_versionbitscache.Statistics(active_chain_tip, consensusParams, id);
        statsUV.pushKV("period", statsStruct.period);
        statsUV.pushKV("period_start", active_chain_tip->nHeight + 1 - statsStruct.elapsed);
        statsUV.pushKV("elapsed", statsStruct.elapsed);
        statsUV.pushKV("count", statsStruct.count);
        if (ThresholdState::LOCKED_IN != thresholdState) {
            statsUV.pushKV("threshold", statsStruct.threshold);
            statsUV.pushKV("possible", statsStruct.possible);
        }
        bip9.pushKV("statistics", statsUV);
    }
    bip9.pushKV("min_activation_height", consensusParams.vDeployments[id].min_activation_height);

    UniValue rv(UniValue::VOBJ);
    rv.pushKV("type", "bip9");
    rv.pushKV("bip9", bip9);
    if (ThresholdState::ACTIVE == thresholdState) {
        rv.pushKV("height", since_height);
    }
    rv.pushKV("active", ThresholdState::ACTIVE == thresholdState);

    softforks.pushKV(DeploymentName(id), rv);
}

RPCHelpMan getblockchaininfo()
{
    return RPCHelpMan{"getblockchaininfo",
                "Returns an object containing various state info regarding blockchain processing.\n",
                {},
                RPCResult{
                    RPCResult::Type::OBJ, "", "",
                    {
                        {RPCResult::Type::STR, "chain", "current network name (main, test, signet, regtest)"},
                        {RPCResult::Type::NUM, "blocks", "the height of the most-work fully-validated chain. The genesis block has height 0"},
                        {RPCResult::Type::NUM, "headers", "the current number of headers we have validated"},
                        {RPCResult::Type::STR, "bestblockhash", "the hash of the currently best block"},
                        {RPCResult::Type::NUM, "difficulty", "the current difficulty"},
                        {RPCResult::Type::NUM_TIME, "time", "The block time expressed in " + UNIX_EPOCH_TIME},
                        {RPCResult::Type::NUM_TIME, "mediantime", "The median block time expressed in " + UNIX_EPOCH_TIME},
                        {RPCResult::Type::NUM, "verificationprogress", "estimate of verification progress [0..1]"},
                        {RPCResult::Type::BOOL, "initialblockdownload", "(debug information) estimate of whether this node is in Initial Block Download mode"},
                        {RPCResult::Type::STR_HEX, "chainwork", "total amount of work in active chain, in hexadecimal"},
                        {RPCResult::Type::NUM, "size_on_disk", "the estimated size of the block and undo files on disk"},
                        {RPCResult::Type::BOOL, "pruned", "if the blocks are subject to pruning"},
                        {RPCResult::Type::NUM, "pruneheight", "lowest-height complete block stored (only present if pruning is enabled)"},
                        {RPCResult::Type::BOOL, "automatic_pruning", "whether automatic pruning is enabled (only present if pruning is enabled)"},
                        {RPCResult::Type::NUM, "prune_target_size", "the target size used by pruning (only present if automatic pruning is enabled)"},
                        {RPCResult::Type::OBJ_DYN, "softforks", "status of softforks",
                        {
                            {RPCResult::Type::OBJ, "xxxx", "name of the softfork",
                            {
                                {RPCResult::Type::STR, "type", "one of \"buried\", \"bip9\""},
                                {RPCResult::Type::OBJ, "bip9", "status of bip9 softforks (only for \"bip9\" type)",
                                {
                                    {RPCResult::Type::STR, "status", "one of \"defined\", \"started\", \"locked_in\", \"active\", \"failed\""},
                                    {RPCResult::Type::NUM, "bit", "the bit (0-28) in the block version field used to signal this softfork (only for \"started\" and \"locked_in\" status)"},
                                    {RPCResult::Type::NUM_TIME, "start_time", "the minimum median time past of a block at which the bit gains its meaning"},
                                    {RPCResult::Type::NUM_TIME, "timeout", "the median time past of a block at which the deployment is considered failed if not yet locked in"},
                                    {RPCResult::Type::NUM, "since", "height of the first block to which the status applies"},
                                    {RPCResult::Type::NUM, "min_activation_height", "minimum height of blocks for which the rules may be enforced"},
                                    {RPCResult::Type::OBJ, "statistics", "numeric statistics about signalling for a softfork (only for \"started\" and \"locked_in\" status)",
                                    {
                                        {RPCResult::Type::NUM, "period", "the length in blocks of the signalling period"},
                                        {RPCResult::Type::NUM, "period_start", "height of the first block of this signalling period"},
                                        {RPCResult::Type::NUM, "threshold", "the number of blocks with the version bit set required to activate the feature (only for \"started\" status)"},
                                        {RPCResult::Type::NUM, "elapsed", "the number of blocks elapsed since the beginning of the current period"},
                                        {RPCResult::Type::NUM, "count", "the number of blocks with the version bit set in the current period"},
                                        {RPCResult::Type::BOOL, "possible", "returns false if there are not enough blocks left in this period to pass activation threshold (only for \"started\" status)"},
                                    }},
                                }},
                                {RPCResult::Type::NUM, "height", "height of the first block which the rules are or will be enforced (only for \"buried\" type, or \"bip9\" type with \"active\" status)"},
                                {RPCResult::Type::BOOL, "active", "true if the rules are enforced for the mempool and the next block"},
                            }},
                        }},
                        {RPCResult::Type::STR, "warnings", "any network and blockchain warnings"},
                    }},
                RPCExamples{
                    HelpExampleCli("getblockchaininfo", "")
            + HelpExampleRpc("getblockchaininfo", "")
                },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    ChainstateManager& chainman = EnsureAnyChainman(request.context);
    LOCK(cs_main);
    CChainState& active_chainstate = chainman.ActiveChainstate();

    const CBlockIndex* tip = active_chainstate.m_chain.Tip();
    CHECK_NONFATAL(tip);
    const int height = tip->nHeight;
    UniValue obj(UniValue::VOBJ);
    obj.pushKV("chain",                 Params().NetworkIDString());
    obj.pushKV("blocks",                height);
    obj.pushKV("headers",               pindexBestHeader ? pindexBestHeader->nHeight : -1);
    obj.pushKV("bestblockhash",         tip->GetBlockHash().GetHex());
    obj.pushKV("difficulty",            (double)GetDifficulty(tip));
    obj.pushKV("time",                  (int64_t)tip->nTime);
    obj.pushKV("mediantime",            (int64_t)tip->GetMedianTimePast());
    obj.pushKV("verificationprogress",  GuessVerificationProgress(Params().TxData(), tip));
    obj.pushKV("initialblockdownload",  active_chainstate.IsInitialBlockDownload());
    obj.pushKV("chainwork",             tip->nChainWork.GetHex());
    obj.pushKV("size_on_disk",          CalculateCurrentUsage());
    obj.pushKV("pruned",                fPruneMode);
    if (fPruneMode) {
        const CBlockIndex* block = tip;
        CHECK_NONFATAL(block);
        while (block->pprev && (block->pprev->nStatus & BLOCK_HAVE_DATA)) {
            block = block->pprev;
        }

        obj.pushKV("pruneheight",        block->nHeight);

        // if 0, execution bypasses the whole if block.
        bool automatic_pruning = (gArgs.GetArg("-prune", 0) != 1);
        obj.pushKV("automatic_pruning",  automatic_pruning);
        if (automatic_pruning) {
            obj.pushKV("prune_target_size",  nPruneTarget);
        }
    }

    const Consensus::Params& consensusParams = Params().GetConsensus();
    UniValue softforks(UniValue::VOBJ);
    SoftForkDescPushBack(tip, softforks, consensusParams, Consensus::DEPLOYMENT_HEIGHTINCB);
    SoftForkDescPushBack(tip, softforks, consensusParams, Consensus::DEPLOYMENT_DERSIG);
    SoftForkDescPushBack(tip, softforks, consensusParams, Consensus::DEPLOYMENT_CLTV);
    SoftForkDescPushBack(tip, softforks, consensusParams, Consensus::DEPLOYMENT_CSV);
    SoftForkDescPushBack(tip, softforks, consensusParams, Consensus::DEPLOYMENT_SEGWIT);
    SoftForkDescPushBack(tip, softforks, consensusParams, Consensus::DEPLOYMENT_TESTDUMMY);
    SoftForkDescPushBack(tip, softforks, consensusParams, Consensus::DEPLOYMENT_TAPROOT);
    obj.pushKV("softforks", softforks);

    obj.pushKV("warnings", GetWarnings(false).original);
    return obj;
},
    };
}

/** Comparison function for sorting the getchaintips heads.  */
struct CompareBlocksByHeight
{
    bool operator()(const CBlockIndex* a, const CBlockIndex* b) const
    {
        /* Make sure that unequal blocks with the same height do not compare
           equal. Use the pointers themselves to make a distinction. */

        if (a->nHeight != b->nHeight)
          return (a->nHeight > b->nHeight);

        return a < b;
    }
};

static RPCHelpMan getchaintips()
{
    return RPCHelpMan{"getchaintips",
                "Return information about all known tips in the block tree,"
                " including the main chain as well as orphaned branches.\n",
                {},
                RPCResult{
                    RPCResult::Type::ARR, "", "",
                    {{RPCResult::Type::OBJ, "", "",
                        {
                            {RPCResult::Type::NUM, "height", "height of the chain tip"},
                            {RPCResult::Type::STR_HEX, "hash", "block hash of the tip"},
                            {RPCResult::Type::NUM, "branchlen", "zero for main chain, otherwise length of branch connecting the tip to the main chain"},
                            {RPCResult::Type::STR, "status", "status of the chain, \"active\" for the main chain\n"
            "Possible values for status:\n"
            "1.  \"invalid\"               This branch contains at least one invalid block\n"
            "2.  \"headers-only\"          Not all blocks for this branch are available, but the headers are valid\n"
            "3.  \"valid-headers\"         All blocks are available for this branch, but they were never fully validated\n"
            "4.  \"valid-fork\"            This branch is not part of the active chain, but is fully validated\n"
            "5.  \"active\"                This is the tip of the active main chain, which is certainly valid"},
                        }}}},
                RPCExamples{
                    HelpExampleCli("getchaintips", "")
            + HelpExampleRpc("getchaintips", "")
                },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    ChainstateManager& chainman = EnsureAnyChainman(request.context);
    LOCK(cs_main);
    CChain& active_chain = chainman.ActiveChain();

    /*
     * Idea: The set of chain tips is the active chain tip, plus orphan blocks which do not have another orphan building off of them.
     * Algorithm:
     *  - Make one pass through BlockIndex(), picking out the orphan blocks, and also storing a set of the orphan block's pprev pointers.
     *  - Iterate through the orphan blocks. If the block isn't pointed to by another orphan, it is a chain tip.
     *  - Add the active chain tip
     */
    std::set<const CBlockIndex*, CompareBlocksByHeight> setTips;
    std::set<const CBlockIndex*> setOrphans;
    std::set<const CBlockIndex*> setPrevs;

    for (const std::pair<const uint256, CBlockIndex*>& item : chainman.BlockIndex()) {
        if (!active_chain.Contains(item.second)) {
            setOrphans.insert(item.second);
            setPrevs.insert(item.second->pprev);
        }
    }

    for (std::set<const CBlockIndex*>::iterator it = setOrphans.begin(); it != setOrphans.end(); ++it) {
        if (setPrevs.erase(*it) == 0) {
            setTips.insert(*it);
        }
    }

    // Always report the currently active tip.
    setTips.insert(active_chain.Tip());

    /* Construct the output array.  */
    UniValue res(UniValue::VARR);
    for (const CBlockIndex* block : setTips) {
        UniValue obj(UniValue::VOBJ);
        obj.pushKV("height", block->nHeight);
        obj.pushKV("hash", block->phashBlock->GetHex());

        const int branchLen = block->nHeight - active_chain.FindFork(block)->nHeight;
        obj.pushKV("branchlen", branchLen);

        std::string status;
        if (active_chain.Contains(block)) {
            // This block is part of the currently active chain.
            status = "active";
        } else if (block->nStatus & BLOCK_FAILED_MASK) {
            // This block or one of its ancestors is invalid.
            status = "invalid";
        } else if (!block->HaveTxsDownloaded()) {
            // This block cannot be connected because full block data for it or one of its parents is missing.
            status = "headers-only";
        } else if (block->IsValid(BLOCK_VALID_SCRIPTS)) {
            // This block is fully validated, but no longer part of the active chain. It was probably the active block once, but was reorganized.
            status = "valid-fork";
        } else if (block->IsValid(BLOCK_VALID_TREE)) {
            // The headers for this block are valid, but it has not been validated. It was probably never part of the most-work chain.
            status = "valid-headers";
        } else {
            // No clue.
            status = "unknown";
        }
        obj.pushKV("status", status);

        res.push_back(obj);
    }

    return res;
},
    };
}

UniValue MempoolInfoToJSON(const CTxMemPool& pool, const std::optional<MempoolHistogramFeeRates> feeLimits)
{
    // Make sure this call is atomic in the pool.
    LOCK(pool.cs);
    UniValue ret(UniValue::VOBJ);
    ret.pushKV("loaded", pool.IsLoaded());
    ret.pushKV("size", (int64_t)pool.size());
    ret.pushKV("bytes", (int64_t)pool.GetTotalTxSize());
    ret.pushKV("usage", (int64_t)pool.DynamicMemoryUsage());
    ret.pushKV("total_fee", ValueFromAmount(pool.GetTotalFee()));
    size_t maxmempool = gArgs.GetArg("-maxmempool", DEFAULT_MAX_MEMPOOL_SIZE) * 1000000;
    ret.pushKV("maxmempool", (int64_t) maxmempool);
    ret.pushKV("mempoolminfee", ValueFromAmount(std::max(pool.GetMinFee(maxmempool), ::minRelayTxFee).GetFeePerK()));
    ret.pushKV("minrelaytxfee", ValueFromAmount(::minRelayTxFee.GetFeePerK()));
    ret.pushKV("unbroadcastcount", uint64_t{pool.GetUnbroadcastTxs().size()});

    if (feeLimits) {
        const MempoolHistogramFeeRates& limits{feeLimits.value()};

        /* Keep histogram per...
         * ... cumulated tx sizes
         * ... txns (count)
         * ... cumulated fees */
        std::vector<uint64_t> sizes(limits.size(), 0);
        std::vector<uint64_t> count(limits.size(), 0);
        std::vector<CAmount> fees(limits.size(), 0);

        for (const CTxMemPoolEntry& e : pool.mapTx) {
            const CAmount fee{e.GetFee()}, afees{e.GetModFeesWithAncestors()}, dfees{e.GetModFeesWithDescendants()};
            const uint32_t size{(uint32_t)e.GetTxSize()}, asize{(uint32_t)e.GetSizeWithAncestors()}, dsize{(uint32_t)e.GetSizeWithDescendants()};

            const CAmount fpb{CFeeRate{fee, size}.GetFee(1)};     // Fee rate per byte
            const CAmount afpb{CFeeRate{afees, asize}.GetFee(1)}; // Fee rate per byte including ancestors
            const CAmount dfpb{CFeeRate{dfees, dsize}.GetFee(1)}; // Fee rate per byte including descendants

            // Fee rate per byte including ancestors & descendants
            const CAmount tfpb{CFeeRate{afees + dfees - fee, asize + dsize - size}.GetFee(1)};
            const CAmount fee_rate_per_byte{std::max(std::min(dfpb, tfpb), std::min(fpb, afpb))};

            // Distribute fee rates into fee limits
            for (size_t i = limits.size(); i-- > 0; ) {
                if (fee_rate_per_byte >= limits[i]) {
                    sizes[i] += size;
                    ++count[i];
                    fees[i] += fee;
                    break;
                }
            }
        }

        // Track total amount of available fees in fee rate groups
        CAmount total_fees = 0;
        UniValue info(UniValue::VOBJ);
        for (size_t i = 0; i < limits.size(); ++i) {
            UniValue info_sub(UniValue::VOBJ);
            info_sub.pushKV("sizes", sizes[i]);
            info_sub.pushKV("count", count.at(i));
            info_sub.pushKV("fees", fees.at(i));
            info_sub.pushKV("from_feerate", limits[i]);
            info_sub.pushKV("to_feerate", i == limits.size() - 1 ? std::numeric_limits<int64_t>::max() : limits[i + 1]);
            total_fees += fees.at(i);
            info.pushKV(ToString(limits[i]), info_sub);
        }
        info.pushKV("total_fees", total_fees);
        ret.pushKV("fee_histogram", info);
    }

    return ret;
}

static RPCHelpMan getmempoolinfo()
{
    return RPCHelpMan{"getmempoolinfo",
                "\nReturns details on the active state of the TX memory pool.\n",
                {
                    {"fee_histogram|with_fee_histogram", RPCArg::Type::ARR, RPCArg::Optional::OMITTED_NAMED_ARG, "Fee statistics grouped by fee rate ranges",
                        {
                            {"fee_rate", RPCArg::Type::NUM, RPCArg::Optional::NO, "Fee rate (in " + CURRENCY_ATOM + "/vB) to group the fees by"},
                        },
                    },
                },
                RPCResult{
                    RPCResult::Type::OBJ, "", "",
                    {
                        {RPCResult::Type::BOOL, "loaded", "True if the mempool is fully loaded"},
                        {RPCResult::Type::NUM, "size", "Current tx count"},
                        {RPCResult::Type::NUM, "bytes", "Sum of all virtual transaction sizes as defined in BIP 141. Differs from actual serialized size because witness data is discounted"},
                        {RPCResult::Type::NUM, "usage", "Total memory usage for the mempool"},
                        {RPCResult::Type::STR_AMOUNT, "total_fee", "Total fees for the mempool in " + CURRENCY_UNIT + "/kvB, ignoring modified fees through prioritizetransaction"},
                        {RPCResult::Type::NUM, "maxmempool", "Maximum memory usage for the mempool"},
                        {RPCResult::Type::STR_AMOUNT, "mempoolminfee", "Minimum fee rate in " + CURRENCY_UNIT + "/kvB for tx to be accepted. Is the maximum of minrelaytxfee and minimum mempool fee"},
                        {RPCResult::Type::STR_AMOUNT, "minrelaytxfee", "Current minimum relay fee for transactions"},
                        {RPCResult::Type::NUM, "unbroadcastcount", "Current number of transactions that haven't passed initial broadcast yet"},
                        {RPCResult::Type::OBJ, "fee_histogram", "",
                            {
                                {RPCResult::Type::OBJ, "<fee_rate_group>", "Fee rate group named by its lower bound (in " + CURRENCY_ATOM + "/vB), identical to the \"from_feerate\" field below",
                                    {
                                        {RPCResult::Type::NUM, "sizes", "Cumulative size of all transactions in the fee rate group"},
                                        {RPCResult::Type::NUM, "count", "Number of transactions in the fee rate group"},
                                        {RPCResult::Type::NUM, "fees", "Cumulative fees of all transactions in the fee rate group (in " + CURRENCY_ATOM + "/vB)"},
                                        {RPCResult::Type::NUM, "from_feerate", "Group contains transactions with fee rates equal or greater than this value (in " + CURRENCY_ATOM + "/vB)"},
                                        {RPCResult::Type::NUM, "to_feerate", "Group contains transactions with fee rates equal or less than this value (in " + CURRENCY_ATOM + "/vB)"},
                                    }},
                                {RPCResult::Type::ELISION, "", ""},
                                {RPCResult::Type::NUM, "total_fees", "Total available fees in mempool (in " + CURRENCY_ATOM + "/vB)"},
                            }},
                    }},
                RPCExamples{
                    HelpExampleCli("getmempoolinfo", "") +
                    HelpExampleCli("getmempoolinfo", R"("[0, 1, 2, 3, 4, 5, 6, 7, 8, 10, 12, 14, 17, 20, 25, 30, 40, 50, 60, 70, 80, 100, 120, 140, 170, 200]")") +
                    HelpExampleRpc("getmempoolinfo", "") +
                    HelpExampleRpc("getmempoolinfo", R"([0, 1, 2, 3, 4, 5, 6, 7, 8, 10, 12, 14, 17, 20, 25, 30, 40, 50, 60, 70, 80, 100, 120, 140, 170, 200])")
                },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    MempoolHistogramFeeRates feelimits;
    std::optional<MempoolHistogramFeeRates> feelimits_opt = std::nullopt;

    if (request.params[0].isTrue()) {
        feelimits_opt = MempoolInfoToJSON_const_limits;
    } else if (!(request.params[0].isNull() || request.params[0].isFalse())) {
        const UniValue feelimits_univalue = request.params[0].get_array();

        if (feelimits_univalue.size() == 0 || feelimits_univalue.size() > 30) {
            throw JSONRPCError(RPC_INVALID_PARAMETER, "Invalid number of parameters");
        }

        for (size_t i = 0; i < feelimits_univalue.size(); ++i) {
            int64_t value = feelimits_univalue[i].get_int64();

            if (value < 0) {
                throw JSONRPCError(RPC_INVALID_PARAMETER, "Non-negative values are expected");
            } else if (i > 0 && feelimits.back() >= value) {
                throw JSONRPCError(RPC_INVALID_PARAMETER, "Strictly increasing values are expected");
            }

            feelimits.push_back(value);
        }
        feelimits_opt = std::optional<MempoolHistogramFeeRates>(feelimits);
    }

    return MempoolInfoToJSON(EnsureAnyMemPool(request.context), feelimits_opt);
},
    };
}

static RPCHelpMan preciousblock()
{
    return RPCHelpMan{"preciousblock",
                "\nTreats a block as if it were received before others with the same work.\n"
                "\nA later preciousblock call can override the effect of an earlier one.\n"
                "\nThe effects of preciousblock are not retained across restarts.\n",
                {
                    {"blockhash", RPCArg::Type::STR_HEX, RPCArg::Optional::NO, "the hash of the block to mark as precious"},
                },
                RPCResult{RPCResult::Type::NONE, "", ""},
                RPCExamples{
                    HelpExampleCli("preciousblock", "\"blockhash\"")
            + HelpExampleRpc("preciousblock", "\"blockhash\"")
                },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    uint256 hash(ParseHashV(request.params[0], "blockhash"));
    CBlockIndex* pblockindex;

    ChainstateManager& chainman = EnsureAnyChainman(request.context);
    {
        LOCK(cs_main);
        pblockindex = chainman.m_blockman.LookupBlockIndex(hash);
        if (!pblockindex) {
            throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Block not found");
        }
    }

    BlockValidationState state;
    chainman.ActiveChainstate().PreciousBlock(state, pblockindex);

    if (!state.IsValid()) {
        throw JSONRPCError(RPC_DATABASE_ERROR, state.ToString());
    }

    return NullUniValue;
},
    };
}

static RPCHelpMan invalidateblock()
{
    return RPCHelpMan{"invalidateblock",
                "\nPermanently marks a block as invalid, as if it violated a consensus rule.\n",
                {
                    {"blockhash", RPCArg::Type::STR_HEX, RPCArg::Optional::NO, "the hash of the block to mark as invalid"},
                },
                RPCResult{RPCResult::Type::NONE, "", ""},
                RPCExamples{
                    HelpExampleCli("invalidateblock", "\"blockhash\"")
            + HelpExampleRpc("invalidateblock", "\"blockhash\"")
                },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    uint256 hash(ParseHashV(request.params[0], "blockhash"));
    BlockValidationState state;

    ChainstateManager& chainman = EnsureAnyChainman(request.context);
    CBlockIndex* pblockindex;
    {
        LOCK(cs_main);
        pblockindex = chainman.m_blockman.LookupBlockIndex(hash);
        if (!pblockindex) {
            throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Block not found");
        }
    }
    chainman.ActiveChainstate().InvalidateBlock(state, pblockindex);

    if (state.IsValid()) {
        chainman.ActiveChainstate().ActivateBestChain(state);
    }

    if (!state.IsValid()) {
        throw JSONRPCError(RPC_DATABASE_ERROR, state.ToString());
    }

    return NullUniValue;
},
    };
}

static RPCHelpMan reconsiderblock()
{
    return RPCHelpMan{"reconsiderblock",
                "\nRemoves invalidity status of a block, its ancestors and its descendants, reconsider them for activation.\n"
                "This can be used to undo the effects of invalidateblock.\n",
                {
                    {"blockhash", RPCArg::Type::STR_HEX, RPCArg::Optional::NO, "the hash of the block to reconsider"},
                },
                RPCResult{RPCResult::Type::NONE, "", ""},
                RPCExamples{
                    HelpExampleCli("reconsiderblock", "\"blockhash\"")
            + HelpExampleRpc("reconsiderblock", "\"blockhash\"")
                },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    ChainstateManager& chainman = EnsureAnyChainman(request.context);
    uint256 hash(ParseHashV(request.params[0], "blockhash"));

    {
        LOCK(cs_main);
        CBlockIndex* pblockindex = chainman.m_blockman.LookupBlockIndex(hash);
        if (!pblockindex) {
            throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Block not found");
        }

        chainman.ActiveChainstate().ResetBlockFailureFlags(pblockindex);
    }

    BlockValidationState state;
    chainman.ActiveChainstate().ActivateBestChain(state);

    if (!state.IsValid()) {
        throw JSONRPCError(RPC_DATABASE_ERROR, state.ToString());
    }

    return NullUniValue;
},
    };
}

static RPCHelpMan getchaintxstats()
{
    return RPCHelpMan{"getchaintxstats",
                "\nCompute statistics about the total number and rate of transactions in the chain.\n",
                {
                    {"nblocks", RPCArg::Type::NUM, RPCArg::DefaultHint{"one month"}, "Size of the window in number of blocks"},
                    {"blockhash", RPCArg::Type::STR_HEX, RPCArg::DefaultHint{"chain tip"}, "The hash of the block that ends the window."},
                },
                RPCResult{
                    RPCResult::Type::OBJ, "", "",
                    {
                        {RPCResult::Type::NUM_TIME, "time", "The timestamp for the final block in the window, expressed in " + UNIX_EPOCH_TIME},
                        {RPCResult::Type::NUM, "txcount", "The total number of transactions in the chain up to that point"},
                        {RPCResult::Type::STR_HEX, "window_final_block_hash", "The hash of the final block in the window"},
                        {RPCResult::Type::NUM, "window_final_block_height", "The height of the final block in the window."},
                        {RPCResult::Type::NUM, "window_block_count", "Size of the window in number of blocks"},
                        {RPCResult::Type::NUM, "window_tx_count", /* optional */ true, "The number of transactions in the window. Only returned if \"window_block_count\" is > 0"},
                        {RPCResult::Type::NUM, "window_interval", /* optional */ true, "The elapsed time in the window in seconds. Only returned if \"window_block_count\" is > 0"},
                        {RPCResult::Type::NUM, "txrate", /* optional */ true, "The average rate of transactions per second in the window. Only returned if \"window_interval\" is > 0"},
                    }},
                RPCExamples{
                    HelpExampleCli("getchaintxstats", "")
            + HelpExampleRpc("getchaintxstats", "2016")
                },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    ChainstateManager& chainman = EnsureAnyChainman(request.context);
    const CBlockIndex* pindex;
    int blockcount = 30 * 24 * 60 * 60 / Params().GetConsensus().nPowTargetSpacing; // By default: 1 month

    if (request.params[1].isNull()) {
        LOCK(cs_main);
        pindex = chainman.ActiveChain().Tip();
    } else {
        uint256 hash(ParseHashV(request.params[1], "blockhash"));
        LOCK(cs_main);
        pindex = chainman.m_blockman.LookupBlockIndex(hash);
        if (!pindex) {
            throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Block not found");
        }
        if (!chainman.ActiveChain().Contains(pindex)) {
            throw JSONRPCError(RPC_INVALID_PARAMETER, "Block is not in main chain");
        }
    }

    CHECK_NONFATAL(pindex != nullptr);

    if (request.params[0].isNull()) {
        blockcount = std::max(0, std::min(blockcount, pindex->nHeight - 1));
    } else {
        blockcount = request.params[0].get_int();

        if (blockcount < 0 || (blockcount > 0 && blockcount >= pindex->nHeight)) {
            throw JSONRPCError(RPC_INVALID_PARAMETER, "Invalid block count: should be between 0 and the block's height - 1");
        }
    }

    const CBlockIndex* pindexPast = pindex->GetAncestor(pindex->nHeight - blockcount);
    int nTimeDiff = pindex->GetMedianTimePast() - pindexPast->GetMedianTimePast();
    int nTxDiff = pindex->nChainTx - pindexPast->nChainTx;

    UniValue ret(UniValue::VOBJ);
    ret.pushKV("time", (int64_t)pindex->nTime);
    ret.pushKV("txcount", (int64_t)pindex->nChainTx);
    ret.pushKV("window_final_block_hash", pindex->GetBlockHash().GetHex());
    ret.pushKV("window_final_block_height", pindex->nHeight);
    ret.pushKV("window_block_count", blockcount);
    if (blockcount > 0) {
        ret.pushKV("window_tx_count", nTxDiff);
        ret.pushKV("window_interval", nTimeDiff);
        if (nTimeDiff > 0) {
            ret.pushKV("txrate", ((double)nTxDiff) / nTimeDiff);
        }
    }

    return ret;
},
    };
}

template<typename T>
static T CalculateTruncatedMedian(std::vector<T>& scores)
{
    size_t size = scores.size();
    if (size == 0) {
        return 0;
    }

    std::sort(scores.begin(), scores.end());
    if (size % 2 == 0) {
        return (scores[size / 2 - 1] + scores[size / 2]) / 2;
    } else {
        return scores[size / 2];
    }
}

void CalculatePercentilesByWeight(CAmount result[NUM_GETBLOCKSTATS_PERCENTILES], std::vector<std::pair<CAmount, int64_t>>& scores, int64_t total_weight)
{
    if (scores.empty()) {
        return;
    }

    std::sort(scores.begin(), scores.end());

    // 10th, 25th, 50th, 75th, and 90th percentile weight units.
    const double weights[NUM_GETBLOCKSTATS_PERCENTILES] = {
        total_weight / 10.0, total_weight / 4.0, total_weight / 2.0, (total_weight * 3.0) / 4.0, (total_weight * 9.0) / 10.0
    };

    int64_t next_percentile_index = 0;
    int64_t cumulative_weight = 0;
    for (const auto& element : scores) {
        cumulative_weight += element.second;
        while (next_percentile_index < NUM_GETBLOCKSTATS_PERCENTILES && cumulative_weight >= weights[next_percentile_index]) {
            result[next_percentile_index] = element.first;
            ++next_percentile_index;
        }
    }

    // Fill any remaining percentiles with the last value.
    for (int64_t i = next_percentile_index; i < NUM_GETBLOCKSTATS_PERCENTILES; i++) {
        result[i] = scores.back().first;
    }
}

void ScriptPubKeyToUniv(const CScript& scriptPubKey, UniValue& out, bool fIncludeHex)
{
    ScriptPubKeyToUniv(scriptPubKey, out, fIncludeHex, IsDeprecatedRPCEnabled("addresses"));
}

void TxToUniv(const CTransaction& tx, const uint256& hashBlock, UniValue& entry, bool include_hex, int serialize_flags, const CTxUndo* txundo, TxVerbosity verbosity)
{
    TxToUniv(tx, hashBlock, IsDeprecatedRPCEnabled("addresses"), entry, include_hex, serialize_flags, txundo, verbosity);
}

template<typename T>
static inline bool SetHasKeys(const std::set<T>& set) {return false;}
template<typename T, typename Tk, typename... Args>
static inline bool SetHasKeys(const std::set<T>& set, const Tk& key, const Args&... args)
{
    return (set.count(key) != 0) || SetHasKeys(set, args...);
}

// outpoint (needed for the utxo index) + nHeight + fCoinBase
static constexpr size_t PER_UTXO_OVERHEAD = sizeof(COutPoint) + sizeof(uint32_t) + sizeof(bool);

static RPCHelpMan getblockstats()
{
    return RPCHelpMan{"getblockstats",
                "\nCompute per block statistics for a given window. All amounts are in satoshis.\n"
                "It won't work for some heights with pruning.\n",
                {
                    {"hash_or_height", RPCArg::Type::NUM, RPCArg::Optional::NO, "The block hash or height of the target block", "", {"", "string or numeric"}},
                    {"stats", RPCArg::Type::ARR, RPCArg::DefaultHint{"all values"}, "Values to plot (see result below)",
                        {
                            {"height", RPCArg::Type::STR, RPCArg::Optional::OMITTED, "Selected statistic"},
                            {"time", RPCArg::Type::STR, RPCArg::Optional::OMITTED, "Selected statistic"},
                        },
                        "stats"},
                },
                RPCResult{
            RPCResult::Type::OBJ, "", "",
            {
                {RPCResult::Type::NUM, "avgfee", "Average fee in the block"},
                {RPCResult::Type::NUM, "avgfeerate", "Average feerate (in satoshis per virtual byte)"},
                {RPCResult::Type::NUM, "avgtxsize", "Average transaction size"},
                {RPCResult::Type::STR_HEX, "blockhash", "The block hash (to check for potential reorgs)"},
                {RPCResult::Type::ARR_FIXED, "feerate_percentiles", "Feerates at the 10th, 25th, 50th, 75th, and 90th percentile weight unit (in satoshis per virtual byte)",
                {
                    {RPCResult::Type::NUM, "10th_percentile_feerate", "The 10th percentile feerate"},
                    {RPCResult::Type::NUM, "25th_percentile_feerate", "The 25th percentile feerate"},
                    {RPCResult::Type::NUM, "50th_percentile_feerate", "The 50th percentile feerate"},
                    {RPCResult::Type::NUM, "75th_percentile_feerate", "The 75th percentile feerate"},
                    {RPCResult::Type::NUM, "90th_percentile_feerate", "The 90th percentile feerate"},
                }},
                {RPCResult::Type::NUM, "height", "The height of the block"},
                {RPCResult::Type::NUM, "ins", "The number of inputs (excluding coinbase)"},
                {RPCResult::Type::NUM, "maxfee", "Maximum fee in the block"},
                {RPCResult::Type::NUM, "maxfeerate", "Maximum feerate (in satoshis per virtual byte)"},
                {RPCResult::Type::NUM, "maxtxsize", "Maximum transaction size"},
                {RPCResult::Type::NUM, "medianfee", "Truncated median fee in the block"},
                {RPCResult::Type::NUM, "mediantime", "The block median time past"},
                {RPCResult::Type::NUM, "mediantxsize", "Truncated median transaction size"},
                {RPCResult::Type::NUM, "minfee", "Minimum fee in the block"},
                {RPCResult::Type::NUM, "minfeerate", "Minimum feerate (in satoshis per virtual byte)"},
                {RPCResult::Type::NUM, "mintxsize", "Minimum transaction size"},
                {RPCResult::Type::NUM, "outs", "The number of outputs"},
                {RPCResult::Type::NUM, "subsidy", "The block subsidy"},
                {RPCResult::Type::NUM, "swtotal_size", "Total size of all segwit transactions"},
                {RPCResult::Type::NUM, "swtotal_weight", "Total weight of all segwit transactions"},
                {RPCResult::Type::NUM, "swtxs", "The number of segwit transactions"},
                {RPCResult::Type::NUM, "time", "The block time"},
                {RPCResult::Type::NUM, "total_out", "Total amount in all outputs (excluding coinbase and thus reward [ie subsidy + totalfee])"},
                {RPCResult::Type::NUM, "total_size", "Total size of all non-coinbase transactions"},
                {RPCResult::Type::NUM, "total_weight", "Total weight of all non-coinbase transactions"},
                {RPCResult::Type::NUM, "totalfee", "The fee total"},
                {RPCResult::Type::NUM, "txs", "The number of transactions (including coinbase)"},
                {RPCResult::Type::NUM, "utxo_increase", "The increase/decrease in the number of unspent outputs (not discounting op_return and similar)"},
                {RPCResult::Type::NUM, "utxo_size_inc", "The increase/decrease in size for the utxo index (not discounting op_return and similar)"},
                {RPCResult::Type::NUM, "utxo_increase_actual", "The increase/decrease in the number of unspent outputs, not counting unspendables"},
                {RPCResult::Type::NUM, "utxo_size_inc_actual", "The increase/decrease in size for the utxo index, not counting unspendables"},
            }},
                RPCExamples{
                    HelpExampleCli("getblockstats", R"('"00000000c937983704a73af28acdec37b049d214adbda81d7e2a3dd146f6ed09"' '["minfeerate","avgfeerate"]')") +
                    HelpExampleCli("getblockstats", R"(1000 '["minfeerate","avgfeerate"]')") +
                    HelpExampleRpc("getblockstats", R"("00000000c937983704a73af28acdec37b049d214adbda81d7e2a3dd146f6ed09", ["minfeerate","avgfeerate"])") +
                    HelpExampleRpc("getblockstats", R"(1000, ["minfeerate","avgfeerate"])")
                },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    ChainstateManager& chainman = EnsureAnyChainman(request.context);
    LOCK(cs_main);
    CBlockIndex* pindex{ParseHashOrHeight(request.params[0], chainman)};
    CHECK_NONFATAL(pindex != nullptr);

    std::set<std::string> stats;
    if (!request.params[1].isNull()) {
        const UniValue stats_univalue = request.params[1].get_array();
        for (unsigned int i = 0; i < stats_univalue.size(); i++) {
            const std::string stat = stats_univalue[i].get_str();
            stats.insert(stat);
        }
    }

    const CBlock block = GetBlockChecked(pindex);
    const CBlockUndo blockUndo = GetUndoChecked(pindex);

    const bool do_all = stats.size() == 0; // Calculate everything if nothing selected (default)
    const bool do_mediantxsize = do_all || stats.count("mediantxsize") != 0;
    const bool do_medianfee = do_all || stats.count("medianfee") != 0;
    const bool do_feerate_percentiles = do_all || stats.count("feerate_percentiles") != 0;
    const bool loop_inputs = do_all || do_medianfee || do_feerate_percentiles ||
        SetHasKeys(stats, "utxo_increase", "utxo_increase_actual", "utxo_size_inc", "utxo_size_inc_actual", "totalfee", "avgfee", "avgfeerate", "minfee", "maxfee", "minfeerate", "maxfeerate");
    const bool loop_outputs = do_all || loop_inputs || stats.count("total_out");
    const bool do_calculate_size = do_mediantxsize ||
        SetHasKeys(stats, "total_size", "avgtxsize", "mintxsize", "maxtxsize", "swtotal_size");
    const bool do_calculate_weight = do_all || SetHasKeys(stats, "total_weight", "avgfeerate", "swtotal_weight", "avgfeerate", "feerate_percentiles", "minfeerate", "maxfeerate");
    const bool do_calculate_sw = do_all || SetHasKeys(stats, "swtxs", "swtotal_size", "swtotal_weight");

    CAmount maxfee = 0;
    CAmount maxfeerate = 0;
    CAmount minfee = MAX_MONEY;
    CAmount minfeerate = MAX_MONEY;
    CAmount total_out = 0;
    CAmount totalfee = 0;
    int64_t inputs = 0;
    int64_t maxtxsize = 0;
    int64_t mintxsize = MAX_BLOCK_SERIALIZED_SIZE;
    int64_t outputs = 0;
    int64_t swtotal_size = 0;
    int64_t swtotal_weight = 0;
    int64_t swtxs = 0;
    int64_t total_size = 0;
    int64_t total_weight = 0;
    int64_t utxos = 0;
    int64_t utxo_size_inc = 0;
    int64_t utxo_size_inc_actual = 0;
    std::vector<CAmount> fee_array;
    std::vector<std::pair<CAmount, int64_t>> feerate_array;
    std::vector<int64_t> txsize_array;

    for (size_t i = 0; i < block.vtx.size(); ++i) {
        const auto& tx = block.vtx.at(i);
        outputs += tx->vout.size();

        CAmount tx_total_out = 0;
        if (loop_outputs) {
            for (const CTxOut& out : tx->vout) {
                tx_total_out += out.nValue;

                size_t out_size = GetSerializeSize(out, PROTOCOL_VERSION) + PER_UTXO_OVERHEAD;
                utxo_size_inc += out_size;

                // The Genesis block and the repeated BIP30 block coinbases don't change the UTXO
                // set counts, so they have to be excluded from the statistics
                if (pindex->nHeight == 0 || (IsBIP30Repeat(*pindex) && tx->IsCoinBase())) continue;
                // Skip unspendable outputs since they are not included in the UTXO set
                if (out.scriptPubKey.IsUnspendable()) continue;

                ++utxos;
                utxo_size_inc_actual += out_size;
            }
        }

        if (tx->IsCoinBase()) {
            continue;
        }

        inputs += tx->vin.size(); // Don't count coinbase's fake input
        total_out += tx_total_out; // Don't count coinbase reward

        int64_t tx_size = 0;
        if (do_calculate_size) {

            tx_size = tx->GetTotalSize();
            if (do_mediantxsize) {
                txsize_array.push_back(tx_size);
            }
            maxtxsize = std::max(maxtxsize, tx_size);
            mintxsize = std::min(mintxsize, tx_size);
            total_size += tx_size;
        }

        int64_t weight = 0;
        if (do_calculate_weight) {
            weight = GetTransactionWeight(*tx);
            total_weight += weight;
        }

        if (do_calculate_sw && tx->HasWitness()) {
            ++swtxs;
            swtotal_size += tx_size;
            swtotal_weight += weight;
        }

        if (loop_inputs) {
            CAmount tx_total_in = 0;
            const auto& txundo = blockUndo.vtxundo.at(i - 1);
            for (const Coin& coin: txundo.vprevout) {
                const CTxOut& prevoutput = coin.out;

                tx_total_in += prevoutput.nValue;
                size_t prevout_size = GetSerializeSize(prevoutput, PROTOCOL_VERSION) + PER_UTXO_OVERHEAD;
                utxo_size_inc -= prevout_size;
                utxo_size_inc_actual -= prevout_size;
            }

            CAmount txfee = tx_total_in - tx_total_out;
            CHECK_NONFATAL(MoneyRange(txfee));
            if (do_medianfee) {
                fee_array.push_back(txfee);
            }
            maxfee = std::max(maxfee, txfee);
            minfee = std::min(minfee, txfee);
            totalfee += txfee;

            // New feerate uses satoshis per virtual byte instead of per serialized byte
            CAmount feerate = weight ? (txfee * WITNESS_SCALE_FACTOR) / weight : 0;
            if (do_feerate_percentiles) {
                feerate_array.emplace_back(std::make_pair(feerate, weight));
            }
            maxfeerate = std::max(maxfeerate, feerate);
            minfeerate = std::min(minfeerate, feerate);
        }
    }

    CAmount feerate_percentiles[NUM_GETBLOCKSTATS_PERCENTILES] = { 0 };
    CalculatePercentilesByWeight(feerate_percentiles, feerate_array, total_weight);

    UniValue feerates_res(UniValue::VARR);
    for (int64_t i = 0; i < NUM_GETBLOCKSTATS_PERCENTILES; i++) {
        feerates_res.push_back(feerate_percentiles[i]);
    }

    UniValue ret_all(UniValue::VOBJ);
    ret_all.pushKV("avgfee", (block.vtx.size() > 1) ? totalfee / (block.vtx.size() - 1) : 0);
    ret_all.pushKV("avgfeerate", total_weight ? (totalfee * WITNESS_SCALE_FACTOR) / total_weight : 0); // Unit: sat/vbyte
    ret_all.pushKV("avgtxsize", (block.vtx.size() > 1) ? total_size / (block.vtx.size() - 1) : 0);
    ret_all.pushKV("blockhash", pindex->GetBlockHash().GetHex());
    ret_all.pushKV("feerate_percentiles", feerates_res);
    ret_all.pushKV("height", (int64_t)pindex->nHeight);
    ret_all.pushKV("ins", inputs);
    ret_all.pushKV("maxfee", maxfee);
    ret_all.pushKV("maxfeerate", maxfeerate);
    ret_all.pushKV("maxtxsize", maxtxsize);
    ret_all.pushKV("medianfee", CalculateTruncatedMedian(fee_array));
    ret_all.pushKV("mediantime", pindex->GetMedianTimePast());
    ret_all.pushKV("mediantxsize", CalculateTruncatedMedian(txsize_array));
    ret_all.pushKV("minfee", (minfee == MAX_MONEY) ? 0 : minfee);
    ret_all.pushKV("minfeerate", (minfeerate == MAX_MONEY) ? 0 : minfeerate);
    ret_all.pushKV("mintxsize", mintxsize == MAX_BLOCK_SERIALIZED_SIZE ? 0 : mintxsize);
    ret_all.pushKV("outs", outputs);
    ret_all.pushKV("subsidy", GetBlockSubsidy(pindex->nHeight, Params().GetConsensus()));
    ret_all.pushKV("swtotal_size", swtotal_size);
    ret_all.pushKV("swtotal_weight", swtotal_weight);
    ret_all.pushKV("swtxs", swtxs);
    ret_all.pushKV("time", pindex->GetBlockTime());
    ret_all.pushKV("total_out", total_out);
    ret_all.pushKV("total_size", total_size);
    ret_all.pushKV("total_weight", total_weight);
    ret_all.pushKV("totalfee", totalfee);
    ret_all.pushKV("txs", (int64_t)block.vtx.size());
    ret_all.pushKV("utxo_increase", outputs - inputs);
    ret_all.pushKV("utxo_size_inc", utxo_size_inc);
    ret_all.pushKV("utxo_increase_actual", utxos - inputs);
    ret_all.pushKV("utxo_size_inc_actual", utxo_size_inc_actual);

    if (do_all) {
        return ret_all;
    }

    UniValue ret(UniValue::VOBJ);
    for (const std::string& stat : stats) {
        const UniValue& value = ret_all[stat];
        if (value.isNull()) {
            throw JSONRPCError(RPC_INVALID_PARAMETER, strprintf("Invalid selected statistic %s", stat));
        }
        ret.pushKV(stat, value);
    }
    return ret;
},
    };
}

static RPCHelpMan savemempool()
{
    return RPCHelpMan{"savemempool",
                "\nDumps the mempool to disk. It will fail until the previous dump is fully loaded.\n",
                {},
                RPCResult{
                    RPCResult::Type::OBJ, "", "",
                    {
                        {RPCResult::Type::STR, "filename", "the directory and file where the mempool was saved"},
                    }},
                RPCExamples{
                    HelpExampleCli("savemempool", "")
            + HelpExampleRpc("savemempool", "")
                },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    const CTxMemPool& mempool = EnsureAnyMemPool(request.context);

    const NodeContext& node = EnsureAnyNodeContext(request.context);

    if (!mempool.IsLoaded()) {
        throw JSONRPCError(RPC_MISC_ERROR, "The mempool was not loaded yet");
    }

    if (!DumpMempool(mempool)) {
        throw JSONRPCError(RPC_MISC_ERROR, "Unable to dump mempool to disk");
    }

    UniValue ret(UniValue::VOBJ);
    ret.pushKV("filename", (node.args->GetDataDirNet() / "mempool.dat").string());

    return ret;
},
    };
}

//! Search for a given set of pubkey scripts
bool FindScriptPubKey(std::atomic<int>& scan_progress, const std::atomic<bool>& should_abort, int64_t& count, CCoinsViewCursor* cursor, const std::set<CScript>& needles, std::map<COutPoint, Coin>& out_results, std::function<void()>& interruption_point)
{
    scan_progress = 0;
    count = 0;
    while (cursor->Valid()) {
        COutPoint key;
        Coin coin;
        if (!cursor->GetKey(key) || !cursor->GetValue(coin)) return false;
        if (++count % 8192 == 0) {
            interruption_point();
            if (should_abort) {
                // allow to abort the scan via the abort reference
                return false;
            }
        }
        if (count % 256 == 0) {
            // update progress reference every 256 item
            uint32_t high = 0x100 * *key.hash.begin() + *(key.hash.begin() + 1);
            scan_progress = (int)(high * 100.0 / 65536.0 + 0.5);
        }
        if (needles.count(coin.out.scriptPubKey)) {
            out_results.emplace(key, coin);
        }
        cursor->Next();
    }
    scan_progress = 100;
    return true;
}

/** RAII object to prevent concurrency issue when scanning the txout set */
static std::atomic<int> g_scan_progress;
static std::atomic<bool> g_scan_in_progress;
static std::atomic<bool> g_should_abort_scan;
class CoinsViewScanReserver
{
private:
    bool m_could_reserve;
public:
    explicit CoinsViewScanReserver() : m_could_reserve(false) {}

    bool reserve() {
        CHECK_NONFATAL(!m_could_reserve);
        if (g_scan_in_progress.exchange(true)) {
            return false;
        }
        CHECK_NONFATAL(g_scan_progress == 0);
        m_could_reserve = true;
        return true;
    }

    ~CoinsViewScanReserver() {
        if (m_could_reserve) {
            g_scan_in_progress = false;
            g_scan_progress = 0;
        }
    }
};

static RPCHelpMan scantxoutset()
{
    return RPCHelpMan{"scantxoutset",
        "\nScans the unspent transaction output set for entries that match certain output descriptors.\n"
        "Examples of output descriptors are:\n"
        "    addr(<address>)                      Outputs whose scriptPubKey corresponds to the specified address (does not include P2PK)\n"
        "    raw(<hex script>)                    Outputs whose scriptPubKey equals the specified hex scripts\n"
        "    combo(<pubkey>)                      P2PK, P2PKH, P2WPKH, and P2SH-P2WPKH outputs for the given pubkey\n"
        "    pkh(<pubkey>)                        P2PKH outputs for the given pubkey\n"
        "    sh(multi(<n>,<pubkey>,<pubkey>,...)) P2SH-multisig outputs for the given threshold and pubkeys\n"
        "\nIn the above, <pubkey> either refers to a fixed public key in hexadecimal notation, or to an xpub/xprv optionally followed by one\n"
        "or more path elements separated by \"/\", and optionally ending in \"/*\" (unhardened), or \"/*'\" or \"/*h\" (hardened) to specify all\n"
        "unhardened or hardened child keys.\n"
        "In the latter case, a range needs to be specified by below if different from 1000.\n"
        "For more information on output descriptors, see the documentation in the doc/descriptors.md file.\n",
        {
            {"action", RPCArg::Type::STR, RPCArg::Optional::NO, "The action to execute\n"
                "\"start\" for starting a scan\n"
                "\"abort\" for aborting the current scan (returns true when abort was successful)\n"
                "\"status\" for progress report (in %) of the current scan"},
            {"scanobjects", RPCArg::Type::ARR, RPCArg::Optional::OMITTED, "Array of scan objects. Required for \"start\" action\n"
                "Every scan object is either a string descriptor or an object:",
            {
                {"descriptor", RPCArg::Type::STR, RPCArg::Optional::OMITTED, "An output descriptor"},
                {"", RPCArg::Type::OBJ, RPCArg::Optional::OMITTED, "An object with output descriptor and metadata",
                {
                    {"desc", RPCArg::Type::STR, RPCArg::Optional::NO, "An output descriptor"},
                    {"range", RPCArg::Type::RANGE, RPCArg::Default{1000}, "The range of HD chain indexes to explore (either end or [begin,end])"},
                }},
            },
                        "[scanobjects,...]"},
        },
        {
            RPCResult{"When action=='abort'", RPCResult::Type::BOOL, "", ""},
            RPCResult{"When action=='status' and no scan is in progress", RPCResult::Type::NONE, "", ""},
            RPCResult{"When action=='status' and scan is in progress", RPCResult::Type::OBJ, "", "",
            {
                {RPCResult::Type::NUM, "progress", "The scan progress"},
            }},
            RPCResult{"When action=='start'", RPCResult::Type::OBJ, "", "", {
                {RPCResult::Type::BOOL, "success", "Whether the scan was completed"},
                {RPCResult::Type::NUM, "txouts", "The number of unspent transaction outputs scanned"},
                {RPCResult::Type::NUM, "height", "The current block height (index)"},
                {RPCResult::Type::STR_HEX, "bestblock", "The hash of the block at the tip of the chain"},
                {RPCResult::Type::ARR, "unspents", "",
                {
                    {RPCResult::Type::OBJ, "", "",
                    {
                        {RPCResult::Type::STR_HEX, "txid", "The transaction id"},
                        {RPCResult::Type::NUM, "vout", "The vout value"},
                        {RPCResult::Type::STR_HEX, "scriptPubKey", "The script key"},
                        {RPCResult::Type::STR, "desc", "A specialized descriptor for the matched scriptPubKey"},
                        {RPCResult::Type::STR_AMOUNT, "amount", "The total amount in " + CURRENCY_UNIT + " of the unspent output"},
                        {RPCResult::Type::NUM, "height", "Height of the unspent transaction output"},
                    }},
                }},
                {RPCResult::Type::STR_AMOUNT, "total_amount", "The total amount of all found unspent outputs in " + CURRENCY_UNIT},
            }},
        },
        RPCExamples{""},
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    RPCTypeCheck(request.params, {UniValue::VSTR, UniValue::VARR});

    UniValue result(UniValue::VOBJ);
    if (request.params[0].get_str() == "status") {
        CoinsViewScanReserver reserver;
        if (reserver.reserve()) {
            // no scan in progress
            return NullUniValue;
        }
        result.pushKV("progress", g_scan_progress);
        return result;
    } else if (request.params[0].get_str() == "abort") {
        CoinsViewScanReserver reserver;
        if (reserver.reserve()) {
            // reserve was possible which means no scan was running
            return false;
        }
        // set the abort flag
        g_should_abort_scan = true;
        return true;
    } else if (request.params[0].get_str() == "start") {
        CoinsViewScanReserver reserver;
        if (!reserver.reserve()) {
            throw JSONRPCError(RPC_INVALID_PARAMETER, "Scan already in progress, use action \"abort\" or \"status\"");
        }

        if (request.params.size() < 2) {
            throw JSONRPCError(RPC_MISC_ERROR, "scanobjects argument is required for the start action");
        }

        std::set<CScript> needles;
        std::map<CScript, std::string> descriptors;
        CAmount total_in = 0;

        // loop through the scan objects
        for (const UniValue& scanobject : request.params[1].get_array().getValues()) {
            FlatSigningProvider provider;
            auto scripts = EvalDescriptorStringOrObject(scanobject, provider);
            for (const auto& script : scripts) {
                std::string inferred = InferDescriptor(script, provider)->ToString();
                needles.emplace(script);
                descriptors.emplace(std::move(script), std::move(inferred));
            }
        }

        // Scan the unspent transaction output set for inputs
        UniValue unspents(UniValue::VARR);
        std::vector<CTxOut> input_txos;
        std::map<COutPoint, Coin> coins;
        g_should_abort_scan = false;
        int64_t count = 0;
        std::unique_ptr<CCoinsViewCursor> pcursor;
        CBlockIndex* tip;
        NodeContext& node = EnsureAnyNodeContext(request.context);
        {
            ChainstateManager& chainman = EnsureChainman(node);
            LOCK(cs_main);
            CChainState& active_chainstate = chainman.ActiveChainstate();
            active_chainstate.ForceFlushStateToDisk();
            pcursor = active_chainstate.CoinsDB().Cursor();
            CHECK_NONFATAL(pcursor);
            tip = active_chainstate.m_chain.Tip();
            CHECK_NONFATAL(tip);
        }
        bool res = FindScriptPubKey(g_scan_progress, g_should_abort_scan, count, pcursor.get(), needles, coins, node.rpc_interruption_point);
        result.pushKV("success", res);
        result.pushKV("txouts", count);
        result.pushKV("height", tip->nHeight);
        result.pushKV("bestblock", tip->GetBlockHash().GetHex());

        for (const auto& it : coins) {
            const COutPoint& outpoint = it.first;
            const Coin& coin = it.second;
            const CTxOut& txo = coin.out;
            input_txos.push_back(txo);
            total_in += txo.nValue;

            UniValue unspent(UniValue::VOBJ);
            unspent.pushKV("txid", outpoint.hash.GetHex());
            unspent.pushKV("vout", (int32_t)outpoint.n);
            unspent.pushKV("scriptPubKey", HexStr(txo.scriptPubKey));
            unspent.pushKV("desc", descriptors[txo.scriptPubKey]);
            unspent.pushKV("amount", ValueFromAmount(txo.nValue));
            unspent.pushKV("height", (int32_t)coin.nHeight);

            unspents.push_back(unspent);
        }
        result.pushKV("unspents", unspents);
        result.pushKV("total_amount", ValueFromAmount(total_in));
    } else {
        throw JSONRPCError(RPC_INVALID_PARAMETER, "Invalid action argument");
    }
    return result;
},
    };
}

/** RAII object to prevent concurrency issue when scanning blockfilters */
static std::atomic<int> g_scanfilter_progress;
static std::atomic<int> g_scanfilter_progress_height;
static std::atomic<bool> g_scanfilter_in_progress;
static std::atomic<bool> g_scanfilter_should_abort_scan;
class BlockFiltersScanReserver
{
private:
    bool m_could_reserve;
public:
    explicit BlockFiltersScanReserver() : m_could_reserve(false) {}

    bool reserve() {
        CHECK_NONFATAL(!m_could_reserve);
        if (g_scanfilter_in_progress.exchange(true)) {
            return false;
        }
        m_could_reserve = true;
        return true;
    }

    ~BlockFiltersScanReserver() {
        if (m_could_reserve) {
            g_scanfilter_in_progress = false;
        }
    }
};

static RPCHelpMan scanblocks()
{
    return RPCHelpMan{"scanblocks",
                "\nReturn relevant blockhashes for given descriptors.\n"
                "This call may take serval minutes. Make sure to use no RPC timeout (bitcoin-cli -rpcclienttimeout=0)",
                {
                    {"action", RPCArg::Type::STR, RPCArg::Optional::NO, "The action to execute\n"
            "                                      \"start\" for starting a scan\n"
            "                                      \"abort\" for aborting the current scan (returns true when abort was successful)\n"
            "                                      \"status\" for progress report (in %) of the current scan (returns Null if there is no ongoing scan)"},
                    {"scanobjects", RPCArg::Type::ARR, RPCArg::Optional::OMITTED, "Array of scan objects.\n"
            "                                  Every scan object is either a string descriptor or an object:",
                        {
                            {"descriptor", RPCArg::Type::STR, RPCArg::Optional::OMITTED, "An output descriptor"},
                            {"", RPCArg::Type::OBJ, RPCArg::Optional::OMITTED, "An object with output descriptor and metadata",
                                {
                                    {"desc", RPCArg::Type::STR, RPCArg::Optional::NO, "An output descriptor"},
                                    {"range", RPCArg::Type::RANGE, RPCArg::Default{1000}, "The range of HD chain indexes to explore (either end or [begin,end])"},
                                },
                            },
                        },
                        "[scanobjects,...]"},
                    {"start_height", RPCArg::Type::NUM, RPCArg::Default{0}, "height to start to filter from"},
                    {"stop_height", RPCArg::Type::NUM, RPCArg::DefaultHint{"<tip>"}, "height to stop to scan"},
                    {"filtertype", RPCArg::Type::STR, RPCArg::Default{"basic"}, "The type name of the filter"}
                },
                {

                    RPCResult{"if action is start (only returns after scan completes)",
                        RPCResult::Type::OBJ, "", "",
                        {
                            {RPCResult::Type::NUM, "from_height", "The height of the first block scanned."},
                            {RPCResult::Type::NUM, "to_height", "The height of the last block scanned."},
                            {RPCResult::Type::ARR, "relevant_blocks", "Blocks that may have matched a scanobject.", {
                                {RPCResult::Type::STR_HEX, "hash", "The blockhash."},
                            }},
                        },
                    },
                    RPCResult{"if action is abort", RPCResult::Type::BOOL, "success", "True if scan will be aborted (not necessarily before this RPC call returns), or false if there is no scan to abort."},
                    RPCResult{"if action is status, and a scan is currently in progress",
                        RPCResult::Type::OBJ, "", "",
                        {
                            {RPCResult::Type::NUM, "progress", "Approximate percent complete."},
                            {RPCResult::Type::NUM, "current_height", "Height of the block currently being scanned."}
                        },
                    },
                    RPCResult{"if action is status, and there is no scan running (possibly already completed)", RPCResult::Type::NONE, "", ""},
                },
                RPCExamples{
                    HelpExampleCli("scanblocks", "\"[\"addr(bcrt1q4u4nsgk6ug0sqz7r3rj9tykjxrsl0yy4d0wwte)\"]\" 300000") +
                    HelpExampleRpc("scanblocks", "\"[\"addr(bcrt1q4u4nsgk6ug0sqz7r3rj9tykjxrsl0yy4d0wwte)\"]\" 300000")
                },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    UniValue ret(UniValue::VOBJ);
    if (request.params[0].get_str() == "status") {
        BlockFiltersScanReserver reserver;
        if (reserver.reserve()) {
            // no scan in progress
            return NullUniValue;
        }
        ret.pushKV("progress", g_scanfilter_progress);
        ret.pushKV("current_height", g_scanfilter_progress_height);
        return ret;
    } else if (request.params[0].get_str() == "abort") {
        BlockFiltersScanReserver reserver;
        if (reserver.reserve()) {
            // reserve was possible which means no scan was running
            return false;
        }
        // set the abort flag
        g_scanfilter_should_abort_scan = true;
        return true;
    }
    else if (request.params[0].get_str() == "start") {
        BlockFiltersScanReserver reserver;
        if (!reserver.reserve()) {
            throw JSONRPCError(RPC_INVALID_PARAMETER, "Scan already in progress, use action \"abort\" or \"status\"");
        }
        const std::string filtertype_name{request.params[4].isNull() ? "basic" : request.params[4].get_str()};

        BlockFilterType filtertype;
        if (!BlockFilterTypeByName(filtertype_name, filtertype)) {
            throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Unknown filtertype");
        }

        BlockFilterIndex* index = GetBlockFilterIndex(filtertype);
        if (!index) {
            throw JSONRPCError(RPC_MISC_ERROR, "Index is not enabled for filtertype " + filtertype_name);
        }

        ChainstateManager& chainman = EnsureAnyChainman(request.context);

        // set the start-height
        const CBlockIndex* block = nullptr;
        const CBlockIndex* stop_block = nullptr;
        {
            LOCK(cs_main);
            block = chainman.ActiveChain().Genesis();
            stop_block = chainman.ActiveChain().Tip();
            if (!request.params[2].isNull()) {
                block = chainman.ActiveChain()[request.params[2].get_int()];
                if (!block) {
                    throw JSONRPCError(RPC_MISC_ERROR, "Invalid start_height");
                }
            }
            if (!request.params[3].isNull()) {
                stop_block = chainman.ActiveChain()[request.params[3].get_int()];
                if (!stop_block || stop_block->nHeight < block->nHeight) {
                    throw JSONRPCError(RPC_MISC_ERROR, "Invalid stop_height");
                }
            }
        }
        CHECK_NONFATAL(block);

        // loop through the scan objects, add scripts to the needle_set
        GCSFilter::ElementSet needle_set;
        for (const UniValue& scanobject : request.params[1].get_array().getValues()) {
            FlatSigningProvider provider;
            std::vector<CScript> scripts = EvalDescriptorStringOrObject(scanobject, provider);
            for (const CScript& script : scripts) {
                needle_set.emplace(script.begin(), script.end());
            }
        }
        NodeContext& node = EnsureAnyNodeContext(request.context);
        UniValue blocks(UniValue::VARR);
        const int amount_per_chunk = 10000;
        const CBlockIndex* start_index = block; // for remembering the start of a blockfilter range
        std::vector<BlockFilter> filters;
        const CBlockIndex* start_block = block; // for progress reporting
        const CBlockIndex* last_scanned_block = block;
        g_scanfilter_should_abort_scan = false;
        g_scanfilter_progress = 0;
        g_scanfilter_progress_height = start_block->nHeight;
        while (block) {
            node.rpc_interruption_point(); // allow a clean shutdown
            if (g_scanfilter_should_abort_scan) {
                break;
            }
            const CBlockIndex* next = nullptr;
            {
                LOCK(cs_main);
                next = chainman.ActiveChain().Next(block);
                if (block == stop_block) next = nullptr;
            }
            if (start_index->nHeight + amount_per_chunk == block->nHeight || next == nullptr) {
                LogPrint(BCLog::RPC, "Fetching blockfilters from height %d to height %d.\n", start_index->nHeight, block->nHeight);
                if (index->LookupFilterRange(start_index->nHeight, block, filters)) {
                    for (const BlockFilter& filter : filters) {
                        // compare the elements-set with each filter
                        if (filter.GetFilter().MatchAny(needle_set)) {
                            blocks.push_back(filter.GetBlockHash().GetHex());
                            LogPrint(BCLog::RPC, "scanblocks: found match in %s\n", filter.GetBlockHash().GetHex());
                        }
                    }
                }
                start_index = block;

                // update progress
                int blocks_processed = block->nHeight - start_block->nHeight;
                int total_blocks_to_process = stop_block->nHeight - start_block->nHeight;
                if (total_blocks_to_process > 0) { // avoid division by zero
                    g_scanfilter_progress = (int)(100.0 / total_blocks_to_process * blocks_processed);
                } else {
                    g_scanfilter_progress = 100;
                }
                g_scanfilter_progress_height = block->nHeight;
            }
            last_scanned_block = block;
            block = next;
        }
        ret.pushKV("from_height", start_block->nHeight);
        ret.pushKV("to_height", last_scanned_block->nHeight);
        ret.pushKV("relevant_blocks", blocks);
    }
    else {
        throw JSONRPCError(RPC_INVALID_PARAMETER, "Invalid action argument");
    }
    return ret;
},
    };
}

static RPCHelpMan getblockfilter()
{
    return RPCHelpMan{"getblockfilter",
                "\nRetrieve a BIP 157 content filter for a particular block.\n",
                {
                    {"blockhash", RPCArg::Type::STR_HEX, RPCArg::Optional::NO, "The hash of the block"},
                    {"filtertype", RPCArg::Type::STR, RPCArg::Default{"basic"}, "The type name of the filter, values: " + ListBlockFilterTypes()},
                },
                RPCResult{
                    RPCResult::Type::OBJ, "", "",
                    {
                        {RPCResult::Type::STR_HEX, "filter", "the hex-encoded filter data"},
                        {RPCResult::Type::STR_HEX, "header", "the hex-encoded filter header"},
                    }},
                RPCExamples{
                    HelpExampleCli("getblockfilter", "\"00000000c937983704a73af28acdec37b049d214adbda81d7e2a3dd146f6ed09\" \"basic\"") +
                    HelpExampleRpc("getblockfilter", "\"00000000c937983704a73af28acdec37b049d214adbda81d7e2a3dd146f6ed09\", \"basic\"")
                },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    uint256 block_hash = ParseHashV(request.params[0], "blockhash");
    std::string filtertype_name = "basic";
    if (!request.params[1].isNull()) {
        filtertype_name = request.params[1].get_str();
    }

    BlockFilterType filtertype;
    if (!BlockFilterTypeByName(filtertype_name, filtertype)) {
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Unknown filtertype");
    }

    BlockFilterIndex* index = GetBlockFilterIndex(filtertype);
    if (!index) {
        throw JSONRPCError(RPC_MISC_ERROR, "Index is not enabled for filtertype " + filtertype_name);
    }

    const CBlockIndex* block_index;
    bool block_was_connected;
    {
        ChainstateManager& chainman = EnsureAnyChainman(request.context);
        LOCK(cs_main);
        block_index = chainman.m_blockman.LookupBlockIndex(block_hash);
        if (!block_index) {
            throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Block not found");
        }
        block_was_connected = block_index->IsValid(BLOCK_VALID_SCRIPTS);
    }

    bool index_ready = index->BlockUntilSyncedToCurrentChain();

    BlockFilter filter;
    uint256 filter_header;
    if (!index->LookupFilter(block_index, filter) ||
        !index->LookupFilterHeader(block_index, filter_header)) {
        int err_code;
        std::string errmsg = "Filter not found.";

        if (!block_was_connected) {
            err_code = RPC_INVALID_ADDRESS_OR_KEY;
            errmsg += " Block was not connected to active chain.";
        } else if (!index_ready) {
            err_code = RPC_MISC_ERROR;
            errmsg += " Block filters are still in the process of being indexed.";
        } else {
            err_code = RPC_INTERNAL_ERROR;
            errmsg += " This error is unexpected and indicates index corruption.";
        }

        throw JSONRPCError(err_code, errmsg);
    }

    UniValue ret(UniValue::VOBJ);
    ret.pushKV("filter", HexStr(filter.GetEncodedFilter()));
    ret.pushKV("header", filter_header.GetHex());
    return ret;
},
    };
}

/**
 * Serialize the UTXO set to a file for loading elsewhere.
 *
 * @see SnapshotMetadata
 */
static RPCHelpMan dumptxoutset()
{
    static const std::vector<std::pair<std::string, coinascii_cb_t>> ascii_types{
        {"txid",         [](const COutPoint& k, const Coin& c) { return k.hash.GetHex(); }},
        {"vout",         [](const COutPoint& k, const Coin& c) { return ToString(static_cast<int32_t>(k.n)); }},
        {"value",        [](const COutPoint& k, const Coin& c) { return ToString(c.out.nValue); }},
        {"coinbase",     [](const COutPoint& k, const Coin& c) { return ToString(c.fCoinBase); }},
        {"height",       [](const COutPoint& k, const Coin& c) { return ToString(static_cast<uint32_t>(c.nHeight)); }},
        {"scriptPubKey", [](const COutPoint& k, const Coin& c) { return HexStr(c.out.scriptPubKey); }},
        // add any other desired items here
    };

    std::vector<RPCArg> ascii_args;
    std::transform(std::begin(ascii_types), std::end(ascii_types), std::back_inserter(ascii_args),
            [](const std::pair<std::string, coinascii_cb_t>& t) { return RPCArg{t.first, RPCArg::Type::STR, RPCArg::Optional::OMITTED, "Info to write for a given UTXO"}; });

    return RPCHelpMan{
        "dumptxoutset",
        "\nWrite the UTXO set to disk.\n",
        {
            {"path",
                RPCArg::Type::STR,
                RPCArg::Optional::NO,
                /* default_val */ "",
                "path to the output file. If relative, will be prefixed by datadir."},
            {"format", RPCArg::Type::ARR, RPCArg::DefaultHint{"compact serialized format"},
                                        "If no argument is provided, a compact binary serialized format is used; otherwise only requested items "
                                        "available below are written in ASCII format (if an empty array is provided, all items are written in ASCII).",
                                        ascii_args, "format"},
            {"show_header", RPCArg::Type::BOOL, RPCArg::Default{true}, "Whether to include the header line in non-serialized (ASCII) mode"},
            {"separator", RPCArg::Type::STR, RPCArg::Default{","}, "Field separator to use in non-serialized (ASCII) mode"},
        },
        RPCResult{
            RPCResult::Type::OBJ, "", "",
                {
                    {RPCResult::Type::NUM, "coins_written", "the number of coins written in the snapshot"},
                    {RPCResult::Type::STR_HEX, "base_hash", "the hash of the base of the snapshot"},
                    {RPCResult::Type::NUM, "base_height", "the height of the base of the snapshot"},
                    {RPCResult::Type::STR, "path", "the absolute path that the snapshot was written to"},
                }
        },
        RPCExamples{
            HelpExampleCli("dumptxoutset", "utxo.dat") +
            HelpExampleCli("dumptxoutset", "utxo.dat '[]'") +
            HelpExampleCli("dumptxoutset", "utxo.dat '[\"txid\", \"vout\"]' false ':'")
        },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    EnsureNotWalletRestricted(request);

    // handle optional ASCII parameters
    const bool is_compact = request.params[1].isNull();
    const bool show_header = request.params[2].isNull() || request.params[2].get_bool();
    const std::string separator = request.params[3].isNull() ? "," : request.params[3].get_str();
    std::vector<std::pair<std::string, coinascii_cb_t>> requested;
    if (!is_compact) {
        const auto& arr = request.params[1].get_array();
        const std::unordered_map<std::string, coinascii_cb_t> ascii_map(std::begin(ascii_types), std::end(ascii_types));
        for(size_t i = 0; i < arr.size(); ++i) {
            const auto it = ascii_map.find(arr[i].get_str());
            if (it == std::end(ascii_map))
                throw JSONRPCError(RPC_INVALID_PARAMETER, "unable to find item '"+arr[i].get_str()+"'");

            requested.push_back(*it);
        }

        // if nothing was found, shows everything by default
        if (requested.size() == 0)
            requested = ascii_types;
    }

    const fs::path path = fsbridge::AbsPathJoin(gArgs.GetDataDirNet(), request.params[0].get_str());
    // Write to a temporary path and then move into `path` on completion
    // to avoid confusion due to an interruption.
    const fs::path temppath = fsbridge::AbsPathJoin(gArgs.GetDataDirNet(), request.params[0].get_str() + ".incomplete");

    if (fs::exists(path)) {
        throw JSONRPCError(
            RPC_INVALID_PARAMETER,
            path.string() + " already exists. If you are sure this is what you want, "
            "move it out of the way first");
    }

    FILE* file{fsbridge::fopen(temppath, is_compact ? "wb" : "w")};
    CAutoFile afile{file, SER_DISK, CLIENT_VERSION};
    NodeContext& node = EnsureAnyNodeContext(request.context);
    UniValue result = CreateUTXOSnapshot(is_compact, show_header, separator, node, node.chainman->ActiveChainstate(), afile, requested);
    fs::rename(temppath, path);

    result.pushKV("path", path.string());
    return result;
},
    };
}

UniValue CreateUTXOSnapshot(const bool is_compact, const bool show_header, const std::string& separator, NodeContext& node, CChainState& chainstate, CAutoFile& afile, const std::vector<std::pair<std::string, coinascii_cb_t>>& requested)
{
    std::unique_ptr<CCoinsViewCursor> pcursor;
    CCoinsStats stats{CoinStatsHashType::NONE};
    CBlockIndex* tip;

    {
        // We need to lock cs_main to ensure that the coinsdb isn't written to
        // between (i) flushing coins cache to disk (coinsdb), (ii) getting stats
        // based upon the coinsdb, and (iii) constructing a cursor to the
        // coinsdb for use below this block.
        //
        // Cursors returned by leveldb iterate over snapshots, so the contents
        // of the pcursor will not be affected by simultaneous writes during
        // use below this block.
        //
        // See discussion here:
        //   https://github.com/bitcoin/bitcoin/pull/15606#discussion_r274479369
        //
        LOCK(::cs_main);

        chainstate.ForceFlushStateToDisk();

        if (!GetUTXOStats(&chainstate.CoinsDB(), chainstate.m_blockman, stats, node.rpc_interruption_point)) {
            throw JSONRPCError(RPC_INTERNAL_ERROR, "Unable to read UTXO set");
        }

        pcursor = chainstate.CoinsDB().Cursor();
        tip = chainstate.m_blockman.LookupBlockIndex(stats.hashBlock);
        CHECK_NONFATAL(tip);
    }

    if (is_compact) {
        SnapshotMetadata metadata{tip->GetBlockHash(), stats.coins_count, tip->nChainTx};
        afile << metadata;
    } else if (show_header) {
        afile.write("#(blockhash " + tip->GetBlockHash().ToString() + " ) ");
        for (auto it = std::begin(requested); it != std::end(requested); ++it) {
            if (it != std::begin(requested))
                afile.write(separator);
            afile.write(it->first);
        }
        afile.write("\n");
    }

    COutPoint key;
    Coin coin;
    unsigned int iter{0};

    while (pcursor->Valid()) {
        if (iter % 5000 == 0) node.rpc_interruption_point();
        ++iter;
        if (pcursor->GetKey(key) && pcursor->GetValue(coin)) {
            if (is_compact) {
                afile << key;
                afile << coin;
            } else {
                for (auto it = std::begin(requested); it != std::end(requested); ++it) {
                    if (it != std::begin(requested))
                        afile.write(separator);
                    afile.write(it->second(key, coin));
                }
                afile.write("\n");
            }
        }

        pcursor->Next();
    }

    afile.fclose();

    UniValue result(UniValue::VOBJ);
    result.pushKV("coins_written", stats.coins_count);
    result.pushKV("base_hash", tip->GetBlockHash().ToString());
    result.pushKV("base_height", tip->nHeight);

    return result;
}

static RPCHelpMan getblocklocations()
{
    return RPCHelpMan{"getblocklocations",
                "\nEXPERIMENTAL warning: this call may be removed or changed in future releases.\n"
                "\nReturns a JSON for the file system location of 'blockhash' block and undo data.\n"
                "\nIt is possible to return also the locations of previous blocks, by specifying 'nblocks' > 1.\n",
                {
                    {"blockhash", RPCArg::Type::STR_HEX, RPCArg::Optional::NO, "The block hash"},
                    {"nblocks", RPCArg::Type::NUM, RPCArg::Optional::NO, "Maximum number locations to return (up to genesis block)"},
                },
                {
                    RPCResult{
                        RPCResult::Type::ARR, "", "",
                        {
                            {RPCResult::Type::NUM, "file", "blk*.dat/rev*.dat file index"},
                            {RPCResult::Type::NUM, "data", "block data file offset"},
                            {RPCResult::Type::NUM, "undo", "undo data file offset (if exists)"},
                            {RPCResult::Type::STR_HEX, "prev", "previous block hash"},
                        }
                    },
                },
                RPCExamples{
                    HelpExampleCli("getblocklocation", "\"00000000c937983704a73af28acdec37b049d214adbda81d7e2a3dd146f6ed09\" 10")
                },
                [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue {

    if (fPruneMode) {
        throw JSONRPCError(RPC_MISC_ERROR, "Block locations are not available in prune mode");
    }

    uint256 hash(ParseHashV(request.params[0], "blockhash"));
    size_t nblocks = request.params[1].get_int();

    ChainstateManager& chainman = EnsureAnyChainman(request.context);
    const CBlockIndex* pblockindex = WITH_LOCK(cs_main, return chainman.m_blockman.LookupBlockIndex(hash));
    if (!pblockindex) {
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Block not found");
    }

    UniValue result(UniValue::VARR);
    do {
        UniValue location(UniValue::VOBJ);
        location.pushKV("file", (uint64_t)pblockindex->nFile);
        location.pushKV("data", (uint64_t)pblockindex->nDataPos);
        if (pblockindex->nUndoPos) {
            location.pushKV("undo", (uint64_t)pblockindex->nUndoPos);
        }
        if (pblockindex->pprev) {
            location.pushKV("prev", pblockindex->pprev->GetBlockHash().GetHex());
        } else {
            location.pushKV("prev", uint256().GetHex());
        }
        result.push_back(location);
        pblockindex = pblockindex->pprev;
    } while (result.size() < nblocks && pblockindex);
    return result;
},
    };
}

void RegisterBlockchainRPCCommands(CRPCTable &t)
{
// clang-format off
static const CRPCCommand commands[] =
{ //  category              actor (function)
  //  --------------------- ------------------------
    { "blockchain",         &getblockchaininfo,                  },
    { "blockchain",         &getchaintxstats,                    },
    { "blockchain",         &getblockstats,                      },
    { "blockchain",         &getbestblockhash,                   },
    { "blockchain",         &getblockcount,                      },
    { "blockchain",         &getblock,                           },
    { "blockchain",         &getblockfrompeer,                   },
    { "blockchain",         &getblockhash,                       },
    { "blockchain",         &getblockheader,                     },
    { "blockchain",         &getchaintips,                       },
    { "blockchain",         &getdifficulty,                      },
    { "blockchain",         &getmempoolancestors,                },
    { "blockchain",         &getmempooldescendants,              },
    { "blockchain",         &getmempoolentry,                    },
    { "blockchain",         &getmempoolinfo,                     },
    { "blockchain",         &getrawmempool,                      },
    { "blockchain",         &gettxout,                           },
    { "blockchain",         &gettxoutsetinfo,                    },
    { "blockchain",         &listprunelocks,                     },
    { "blockchain",         &setprunelock,                       },
    { "blockchain",         &pruneblockchain,                    },
    { "blockchain",         &savemempool,                        },
    { "blockchain",         &maxmempool,                         },
    { "blockchain",         &verifychain,                        },
    { "blockchain",         &scriptthreadsinfo,                  },
    { "blockchain",         &setscriptthreadsenabled,            },

    { "blockchain",         &preciousblock,                      },
    { "blockchain",         &scantxoutset,                       },
    { "blockchain",         &scanblocks,                         },
    { "blockchain",         &getblockfilter,                     },

    /* Not shown in help */
    { "hidden",              &invalidateblock,                   },
    { "hidden",              &reconsiderblock,                   },
    { "hidden",              &waitfornewblock,                   },
    { "hidden",              &waitforblock,                      },
    { "hidden",              &waitforblockheight,                },
    { "hidden",              &syncwithvalidationinterfacequeue,  },
    { "hidden",              &dumptxoutset,                      },
    { "hidden",              &getblocklocations,                 },
};
// clang-format on
    for (const auto& c : commands) {
        t.appendCommand(c.name, &c);
    }
}
