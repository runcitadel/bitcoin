// Copyright (c) 2016-2020 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <fs.h>
#include <tinyformat.h>
#include <univalue.h>
#include <util/system.h>
#include <util/translation.h>
#include <wallet/dump.h>
#include <wallet/salvage.h>
#include <wallet/wallet.h>
#include <wallet/walletutil.h>

#include <cassert>
#include <string>

UniValue ProcessDescriptorImport(CWallet& wallet, const UniValue& data, const int64_t timestamp)
    EXCLUSIVE_LOCKS_REQUIRED(wallet.cs_wallet);

namespace WalletTool {

// The standard wallet deleter function blocks on the validation interface
// queue, which doesn't exist for the bitcoin-wallet. Define our own
// deleter here.
static void WalletToolReleaseWallet(CWallet* wallet)
{
    wallet->WalletLogPrintf("Releasing wallet\n");
    wallet->Close();
    delete wallet;
}

static void WalletCreate(CWallet* wallet_instance, uint64_t wallet_creation_flags)
{
    LOCK(wallet_instance->cs_wallet);

    wallet_instance->SetMinVersion(FEATURE_HD_SPLIT);
    wallet_instance->AddWalletFlags(wallet_creation_flags);

    if (!wallet_instance->IsWalletFlagSet(WALLET_FLAG_DESCRIPTORS)) {
        auto spk_man = wallet_instance->GetOrCreateLegacyScriptPubKeyMan();
        spk_man->SetupGeneration(false);
    } else {
        wallet_instance->SetupDescriptorScriptPubKeyMans();
    }

    tfm::format(std::cout, "Topping up keypool...\n");
    wallet_instance->TopUpKeyPool();
}

static std::shared_ptr<CWallet> MakeWallet(const std::string& name, const fs::path& path, DatabaseOptions options, CWallet::do_init_used_flag do_init_used_flag_val = CWallet::do_init_used_flag::Init)
{
    DatabaseStatus status;
    bilingual_str error;
    std::unique_ptr<WalletDatabase> database = MakeDatabase(path, options, status, error);
    if (!database) {
        tfm::format(std::cerr, "%s\n", error.original);
        return nullptr;
    }

    // dummy chain interface
    std::shared_ptr<CWallet> wallet_instance{new CWallet(nullptr /* chain */, name, std::move(database)), WalletToolReleaseWallet};
    DBErrors load_wallet_ret;
    try {
        load_wallet_ret = wallet_instance->LoadWallet(do_init_used_flag_val);
    } catch (const std::runtime_error&) {
        tfm::format(std::cerr, "Error loading %s. Is wallet being used by another process?\n", name);
        return nullptr;
    }

    if (load_wallet_ret != DBErrors::LOAD_OK) {
        wallet_instance = nullptr;
        if (load_wallet_ret == DBErrors::CORRUPT) {
            tfm::format(std::cerr, "Error loading %s: Wallet corrupted", name);
            return nullptr;
        } else if (load_wallet_ret == DBErrors::NONCRITICAL_ERROR) {
            tfm::format(std::cerr, "Error reading %s! All keys read correctly, but transaction data"
                            " or address book entries might be missing or incorrect.",
                name);
        } else if (load_wallet_ret == DBErrors::TOO_NEW) {
            tfm::format(std::cerr, "Error loading %s: Wallet requires newer version of %s",
                name, PACKAGE_NAME);
            return nullptr;
        } else if (load_wallet_ret == DBErrors::NEED_REWRITE) {
            tfm::format(std::cerr, "Wallet needed to be rewritten: restart %s to complete", PACKAGE_NAME);
            return nullptr;
        } else {
            tfm::format(std::cerr, "Error loading %s", name);
            return nullptr;
        }
    }

    if (options.require_create) WalletCreate(wallet_instance.get(), options.create_flags);

    return wallet_instance;
}

static void WalletShowInfo(CWallet* wallet_instance)
{
    LOCK(wallet_instance->cs_wallet);

    tfm::format(std::cout, "Wallet info\n===========\n");
    tfm::format(std::cout, "Name: %s\n", wallet_instance->GetName());
    tfm::format(std::cout, "Format: %s\n", wallet_instance->GetDatabase().Format());
    tfm::format(std::cout, "Descriptors: %s\n", wallet_instance->IsWalletFlagSet(WALLET_FLAG_DESCRIPTORS) ? "yes" : "no");
    tfm::format(std::cout, "Encrypted: %s\n", wallet_instance->IsCrypted() ? "yes" : "no");
    tfm::format(std::cout, "HD (hd seed available): %s\n", wallet_instance->IsHDEnabled() ? "yes" : "no");
    tfm::format(std::cout, "Keypool Size: %u\n", wallet_instance->GetKeyPoolSize());
    tfm::format(std::cout, "Transactions: %zu\n", wallet_instance->mapWallet.size());
    tfm::format(std::cout, "Address Book: %zu\n", wallet_instance->m_address_book.size());
}

static bool ReadAndParseColdcardFile(const fs::path& path, UniValue& decriptors)
{
    fsbridge::ifstream file;
    file.open(path);
    if (!file.is_open()) {
        tfm::format(std::cerr, "%s. Please check permissions.\n", path.string());
        return false;
    }

    std::string line;
    while (std::getline(file, line)) {
        if (line.substr(0, 22) == "importdescriptors \'[{\"") break;
    }

    file.close();

    decriptors.clear();
    if (!decriptors.read(line.substr(19, line.size() - 20))) {
        tfm::format(std::cerr, "Unable to parse %s\n", path.string());
        return false;
    }

    assert(decriptors.isArray());
    return true;
}

bool ExecuteWalletToolFunc(const ArgsManager& args, const std::string& command)
{
    if (args.IsArgSet("-format") && command != "createfromdump") {
        tfm::format(std::cerr, "The -format option can only be used with the \"createfromdump\" command.\n");
        return false;
    }
    if (args.IsArgSet("-dumpfile") && command != "dump" && command != "createfromdump" && command != "importfromcoldcard") {
        tfm::format(std::cerr, "The -dumpfile option can only be used with the \"dump\", \"createfromdump\" and \"importfromcoldcard\" commands.\n");
        return false;
    }
    if (args.IsArgSet("-descriptors") && command != "create") {
        tfm::format(std::cerr, "The -descriptors option can only be used with the 'create' command.\n");
        return false;
    }
    if (command == "create" && !args.IsArgSet("-wallet")) {
        tfm::format(std::cerr, "Wallet name must be provided when creating a new wallet.\n");
        return false;
    }
    const std::string name = args.GetArg("-wallet", "");
    const fs::path path = fsbridge::AbsPathJoin(GetWalletDir(), name);

    if (command == "create") {
        DatabaseOptions options;
        options.require_create = true;
        if (args.GetBoolArg("-descriptors", false)) {
            options.create_flags |= WALLET_FLAG_DESCRIPTORS;
            options.require_format = DatabaseFormat::SQLITE;
        }

        std::shared_ptr<CWallet> wallet_instance = MakeWallet(name, path, options);
        if (wallet_instance) {
            WalletShowInfo(wallet_instance.get());
            wallet_instance->Close();
        }
    } else if (command == "info") {
        DatabaseOptions options;
        options.require_existing = true;
        // NOTE: We need to skip initialisation of the m_used flag, or else the address book count might be wrong
        std::shared_ptr<CWallet> wallet_instance = MakeWallet(name, path, options, CWallet::do_init_used_flag::Skip);
        if (!wallet_instance) return false;
        WalletShowInfo(wallet_instance.get());
        wallet_instance->Close();
    } else if (command == "salvage") {
#ifdef USE_BDB
        bilingual_str error;
        std::vector<bilingual_str> warnings;
        bool ret = RecoverDatabaseFile(path, error, warnings);
        if (!ret) {
            for (const auto& warning : warnings) {
                tfm::format(std::cerr, "%s\n", warning.original);
            }
            if (!error.empty()) {
                tfm::format(std::cerr, "%s\n", error.original);
            }
        }
        return ret;
#else
        tfm::format(std::cerr, "Salvage command is not available as BDB support is not compiled");
        return false;
#endif
    } else if (command == "dump") {
        DatabaseOptions options;
        options.require_existing = true;
        std::shared_ptr<CWallet> wallet_instance = MakeWallet(name, path, options);
        if (!wallet_instance) return false;

        // Get the dumpfile
        std::string dump_filename = gArgs.GetArg("-dumpfile", "");
        if (dump_filename.empty()) {
            tfm::format(std::cerr, "No dump file provided. To use dump, -dumpfile=<filename> must be provided.\n");
            return false;
        }

        if (wallet_instance->GetDatabase().Format() == "bdb") {
            tfm::format(std::cerr, "dump: WARNING: BDB-backed wallets have a wallet id that is not currently dumped.\n");
        }

        bilingual_str error;
        bool ret = DumpWallet(*wallet_instance, error, dump_filename);
        if (!ret && !error.empty()) {
            tfm::format(std::cerr, "%s\n", error.original);
            return ret;
        }
        tfm::format(std::cerr, "The dumpfile may contain private keys. To ensure the safety of your Bitcoin, do not share the dumpfile.\n");
        return ret;
    } else if (command == "createfromdump") {
        bilingual_str error;
        std::vector<bilingual_str> warnings;
        bool ret = CreateFromDump(name, path, error, warnings);
        for (const auto& warning : warnings) {
            tfm::format(std::cerr, "%s\n", warning.original);
        }
        if (!ret && !error.empty()) {
            tfm::format(std::cerr, "%s\n", error.original);
        }
        return ret;
    } else if (command == "importfromcoldcard") {
        tfm::format(std::cerr, "WARNING: The \"importfromcoldcard\" command is experimental and will likely be removed or changed incompatibly in a future version.\n");

        std::string filename = gArgs.GetArg("-dumpfile", "");
        if (filename.empty()) {
            tfm::format(std::cerr, "To use importfromcoldcard, -dumpfile=<filename> must be provided.\n");
            return false;
        }

        const fs::path import_file_path{fs::absolute(fs::path(filename))};
        if (!fs::exists(import_file_path)) {
            tfm::format(std::cerr, "File %s does not exist.\n", import_file_path.string());
            return false;
        }

        UniValue descriptors;
        if (!ReadAndParseColdcardFile(import_file_path, descriptors)) {
            return false;
        }

        DatabaseOptions options;
        options.require_create = false;  // we do this manually
        options.create_flags |= WALLET_FLAG_DESCRIPTORS;
        options.create_flags |= WALLET_FLAG_DISABLE_PRIVATE_KEYS;
        options.create_flags |= WALLET_FLAG_BLANK_WALLET;
        options.require_format = DatabaseFormat::SQLITE;
        std::shared_ptr<CWallet> wallet_instance = MakeWallet(name, path, options);
        if (!wallet_instance) {
            return false;
        }

        LOCK(wallet_instance->cs_wallet);

        {   // This is WalletCreate, but using FEATURE_LATEST and not setting up descriptors (ie, BLANK_WALLET)
            wallet_instance->SetMinVersion(FEATURE_LATEST);
            wallet_instance->AddWalletFlags(options.create_flags);
        }

        for (const UniValue& descriptor : descriptors.getValues()) {
            const UniValue result = ::ProcessDescriptorImport(*wallet_instance, descriptor, 0);
            tfm::format(std::cerr, "%s\n", result.write(2));
        }

        WalletShowInfo(wallet_instance.get());
        wallet_instance->Close();
    } else {
        tfm::format(std::cerr, "Invalid command: %s\n", command);
        return false;
    }

    return true;
}
} // namespace WalletTool
