// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2020 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <util/system.h>

#ifdef ENABLE_EXTERNAL_SIGNER
#if defined(WIN32) && !defined(__kernel_entry)
// A workaround for boost 1.71 incompatibility with mingw-w64 compiler.
// For details see https://github.com/bitcoin/bitcoin/pull/22348.
#define __kernel_entry
#endif
#include <boost/process.hpp>
#endif // ENABLE_EXTERNAL_SIGNER

#include <chainparamsbase.h>
#include <sync.h>
#include <util/check.h>
#include <util/getuniquepath.h>
#include <util/strencodings.h>
#include <util/string.h>
#include <util/translation.h>


#if (defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__DragonFly__))
#include <pthread.h>
#include <pthread_np.h>
#endif

#ifndef WIN32
// for posix_fallocate, in configure.ac we check if it is present after this
#ifdef __linux__

#ifdef _POSIX_C_SOURCE
#undef _POSIX_C_SOURCE
#endif

#define _POSIX_C_SOURCE 200112L

#endif // __linux__

#include <algorithm>
#include <cassert>
#include <fcntl.h>
#include <sched.h>
#include <sys/resource.h>
#include <sys/stat.h>

#else

#ifdef _MSC_VER
#pragma warning(disable:4786)
#pragma warning(disable:4804)
#pragma warning(disable:4805)
#pragma warning(disable:4717)
#endif

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <codecvt>

#include <io.h> /* for _commit */
#include <shellapi.h>
#include <shlobj.h>
#endif

#ifdef HAVE_MALLOPT_ARENA_MAX
#include <malloc.h>
#endif

#ifdef HAVE_LINUX_SYSINFO
#include <sys/sysinfo.h>
#endif

#include <boost/algorithm/string/replace.hpp>
#include <string>
#include <thread>
#include <typeinfo>
#include <univalue.h>
#include <unordered_set>

// Application startup time (used for uptime calculation)
const int64_t nStartupTime = GetTime();

const char * const BITCOIN_CONF_FILENAME = "bitcoin.conf";
const char * const BITCOIN_SETTINGS_FILENAME = "settings.json";
const char * const BITCOIN_RW_CONF_FILENAME = "bitcoin_rw.conf";

ArgsManager gArgs;

/** Mutex to protect dir_locks. */
static Mutex cs_dir_locks;
/** A map that contains all the currently held directory locks. After
 * successful locking, these will be held here until the global destructor
 * cleans them up and thus automatically unlocks them, or ReleaseDirectoryLocks
 * is called.
 */
static std::map<std::string, std::unique_ptr<fsbridge::FileLock>> dir_locks GUARDED_BY(cs_dir_locks);

bool LockDirectory(const fs::path& directory, const std::string lockfile_name, bool probe_only)
{
    LOCK(cs_dir_locks);
    fs::path pathLockFile = directory / lockfile_name;

    // If a lock for this directory already exists in the map, don't try to re-lock it
    if (dir_locks.count(pathLockFile.string())) {
        return true;
    }

    // Create empty lock file if it doesn't exist.
    FILE* file = fsbridge::fopen(pathLockFile, "a");
    if (file) fclose(file);
    auto lock = std::make_unique<fsbridge::FileLock>(pathLockFile);
    if (!lock->TryLock()) {
        return error("Error while attempting to lock directory %s: %s", directory.string(), lock->GetReason());
    }
    if (!probe_only) {
        // Lock successful and we're not just probing, put it into the map
        dir_locks.emplace(pathLockFile.string(), std::move(lock));
    }
    return true;
}

void UnlockDirectory(const fs::path& directory, const std::string& lockfile_name)
{
    LOCK(cs_dir_locks);
    dir_locks.erase((directory / lockfile_name).string());
}

void ReleaseDirectoryLocks()
{
    LOCK(cs_dir_locks);
    dir_locks.clear();
}

bool DirIsWritable(const fs::path& directory)
{
    fs::path tmpFile = GetUniquePath(directory);

    FILE* file = fsbridge::fopen(tmpFile, "a");
    if (!file) return false;

    fclose(file);
    remove(tmpFile);

    return true;
}

bool CheckDiskSpace(const fs::path& dir, uint64_t additional_bytes)
{
    constexpr uint64_t min_disk_space = 52428800; // 50 MiB

    uint64_t free_bytes_available = fs::space(dir).available;
    return free_bytes_available >= min_disk_space + additional_bytes;
}

std::streampos GetFileSize(const char* path, std::streamsize max) {
    std::ifstream file(path, std::ios::binary);
    file.ignore(max);
    return file.gcount();
}

/**
 * Interpret a string argument as a boolean.
 *
 * The definition of atoi() requires that non-numeric string values like "foo",
 * return 0. This means that if a user unintentionally supplies a non-integer
 * argument here, the return value is always false. This means that -foo=false
 * does what the user probably expects, but -foo=true is well defined but does
 * not do what they probably expected.
 *
 * The return value of atoi() is undefined when given input not representable as
 * an int. On most systems this means string value between "-2147483648" and
 * "2147483647" are well defined (this method will return true). Setting
 * -txindex=2147483648 on most systems, however, is probably undefined.
 *
 * For a more extensive discussion of this topic (and a wide range of opinions
 * on the Right Way to change this code), see PR12713.
 */
static bool InterpretBool(const std::string& strValue)
{
    if (strValue.empty())
        return true;
    return (atoi(strValue) != 0);
}

static std::string SettingName(const std::string& arg)
{
    return arg.size() > 0 && arg[0] == '-' ? arg.substr(1) : arg;
}

/**
 * Interpret -nofoo as if the user supplied -foo=0.
 *
 * This method also tracks when the -no form was supplied, and if so,
 * checks whether there was a double-negative (-nofoo=0 -> -foo=1).
 *
 * If there was not a double negative, it removes the "no" from the key
 * and returns false.
 *
 * If there was a double negative, it removes "no" from the key, and
 * returns true.
 *
 * If there was no "no", it returns the string value untouched.
 *
 * Where an option was negated can be later checked using the
 * IsArgNegated() method. One use case for this is to have a way to disable
 * options that are not normally boolean (e.g. using -nodebuglogfile to request
 * that debug log output is not sent to any file at all).
 */

static util::SettingsValue InterpretOption(std::string& section, std::string& key, const std::string& value)
{
    // Split section name from key name for keys like "testnet.foo" or "regtest.bar"
    size_t option_index = key.find('.');
    if (option_index != std::string::npos) {
        section = key.substr(0, option_index);
        key.erase(0, option_index + 1);
    }
    if (key.substr(0, 2) == "no") {
        key.erase(0, 2);
        // Double negatives like -nofoo=0 are supported (but discouraged)
        if (!InterpretBool(value)) {
            LogPrintf("Warning: parsed potentially confusing double-negative -%s=%s\n", key, value);
            return true;
        }
        return false;
    }
    return value;
}

/**
 * Check settings value validity according to flags.
 *
 * TODO: Add more meaningful error checks here in the future
 * See "here's how the flags are meant to behave" in
 * https://github.com/bitcoin/bitcoin/pull/16097#issuecomment-514627823
 */
static bool CheckValid(const std::string& key, const util::SettingsValue& val, unsigned int flags, std::string& error)
{
    if (val.isBool() && !(flags & ArgsManager::ALLOW_BOOL)) {
        error = strprintf("Negating of -%s is meaningless and therefore forbidden", key);
        return false;
    }
    return true;
}

namespace {
fs::path StripRedundantLastElementsOfPath(const fs::path& path)
{
    auto result = path;
    while (result.filename().string() == ".") {
        result = result.parent_path();
    }

    assert(fs::equivalent(result, path));
    return result;
}
} // namespace

// Define default constructor and destructor that are not inline, so code instantiating this class doesn't need to
// #include class definitions for all members.
// For example, m_settings has an internal dependency on univalue.
ArgsManager::ArgsManager() {}
ArgsManager::~ArgsManager() {}

const std::set<std::string> ArgsManager::GetUnsuitableSectionOnlyArgs() const
{
    std::set<std::string> unsuitables;

    LOCK(cs_args);

    // if there's no section selected, don't worry
    if (m_network.empty()) return std::set<std::string> {};

    // if it's okay to use the default section for this network, don't worry
    if (m_network == CBaseChainParams::MAIN) return std::set<std::string> {};

    for (const auto& arg : m_network_only_args) {
        if (OnlyHasDefaultSectionSetting(m_settings, m_network, SettingName(arg))) {
            unsuitables.insert(arg);
        }
    }
    return unsuitables;
}

const std::list<SectionInfo> ArgsManager::GetUnrecognizedSections() const
{
    // Section names to be recognized in the config file.
    static const std::set<std::string> available_sections{
        CBaseChainParams::REGTEST,
        CBaseChainParams::SIGNET,
        CBaseChainParams::TESTNET,
        CBaseChainParams::MAIN
    };

    LOCK(cs_args);
    std::list<SectionInfo> unrecognized = m_config_sections;
    unrecognized.remove_if([](const SectionInfo& appeared){ return available_sections.find(appeared.m_name) != available_sections.end(); });
    return unrecognized;
}

void ArgsManager::SelectConfigNetwork(const std::string& network)
{
    LOCK(cs_args);
    m_network = network;
}

bool ArgsManager::ParseParameters(int argc, const char* const argv[], std::string& error)
{
    LOCK(cs_args);
    m_settings.command_line_options.clear();

    for (int i = 1; i < argc; i++) {
        std::string key(argv[i]);

#ifdef MAC_OSX
        // At the first time when a user gets the "App downloaded from the
        // internet" warning, and clicks the Open button, macOS passes
        // a unique process serial number (PSN) as -psn_... command-line
        // argument, which we filter out.
        if (key.substr(0, 5) == "-psn_") continue;
#endif

        if (key == "-") break; //bitcoin-tx using stdin
        std::string val;
        size_t is_index = key.find('=');
        if (is_index != std::string::npos) {
            val = key.substr(is_index + 1);
            key.erase(is_index);
        }
#ifdef WIN32
        key = ToLower(key);
        if (key[0] == '/')
            key[0] = '-';
#endif

        if (key[0] != '-') {
            if (!m_accept_any_command && m_command.empty()) {
                // The first non-dash arg is a registered command
                std::optional<unsigned int> flags = GetArgFlags(key);
                if (!flags || !(*flags & ArgsManager::COMMAND)) {
                    error = strprintf("Invalid command '%s'", argv[i]);
                    return false;
                }
            }
            m_command.push_back(key);
            while (++i < argc) {
                // The remaining args are command args
                m_command.push_back(argv[i]);
            }
            break;
        }

        // Transform --foo to -foo
        if (key.length() > 1 && key[1] == '-')
            key.erase(0, 1);

        // Transform -foo to foo
        key.erase(0, 1);
        std::string section;
        util::SettingsValue value = InterpretOption(section, key, val);
        std::optional<unsigned int> flags = GetArgFlags('-' + key);

        // Unknown command line options and command line options with dot
        // characters (which are returned from InterpretOption with nonempty
        // section strings) are not valid.
        if (!flags || !section.empty()) {
            error = strprintf("Invalid parameter %s", argv[i]);
            return false;
        }

        if (!CheckValid(key, value, *flags, error)) return false;

        m_settings.command_line_options[key].push_back(value);
    }

    // we do not allow -includeconf from command line, only -noincludeconf
    if (auto* includes = util::FindKey(m_settings.command_line_options, "includeconf")) {
        const util::SettingsSpan values{*includes};
        // Range may be empty if -noincludeconf was passed
        if (!values.empty()) {
            error = "-includeconf cannot be used from commandline; -includeconf=" + values.begin()->write();
            return false; // pick first value as example
        }
    }
    return true;
}

std::optional<unsigned int> ArgsManager::GetArgFlags(const std::string& name) const
{
    LOCK(cs_args);
    for (const auto& arg_map : m_available_args) {
        const auto search = arg_map.second.find(name);
        if (search != arg_map.second.end()) {
            return search->second.m_flags;
        }
    }
    return std::nullopt;
}

const fs::path& ArgsManager::GetBlocksDirPath() const
{
    LOCK(cs_args);
    fs::path& path = m_cached_blocks_path;

    // Cache the path to avoid calling fs::create_directories on every call of
    // this function
    if (!path.empty()) return path;

    if (IsArgSet("-blocksdir")) {
        path = fs::system_complete(GetArg("-blocksdir", ""));
        if (!fs::is_directory(path)) {
            path = "";
            return path;
        }
    } else {
        path = GetDataDirBase();
    }

    path /= BaseParams().DataDir();
    path /= "blocks";
    fs::create_directories(path);
    path = StripRedundantLastElementsOfPath(path);
    return path;
}

const fs::path& ArgsManager::GetDataDir(bool net_specific) const
{
    LOCK(cs_args);
    fs::path& path = net_specific ? m_cached_network_datadir_path : m_cached_datadir_path;

    // Cache the path to avoid calling fs::create_directories on every call of
    // this function
    if (!path.empty()) return path;

    std::string datadir = GetArg("-datadir", "");
    if (!datadir.empty()) {
        path = fs::system_complete(datadir);
        if (!fs::is_directory(path)) {
            path = "";
            return path;
        }
    } else {
        path = GetDefaultDataDir();
    }
    if (net_specific)
        path /= BaseParams().DataDir();

    if (fs::create_directories(path)) {
        // This is the first run, create wallets subdirectory too
        fs::create_directories(path / "wallets");
    }

    path = StripRedundantLastElementsOfPath(path);
    return path;
}

void ArgsManager::ClearPathCache()
{
    LOCK(cs_args);

    m_cached_datadir_path = fs::path();
    m_cached_network_datadir_path = fs::path();
    m_cached_blocks_path = fs::path();
}

std::optional<const ArgsManager::Command> ArgsManager::GetCommand() const
{
    Command ret;
    LOCK(cs_args);
    auto it = m_command.begin();
    if (it == m_command.end()) {
        // No command was passed
        return std::nullopt;
    }
    if (!m_accept_any_command) {
        // The registered command
        ret.command = *(it++);
    }
    while (it != m_command.end()) {
        // The unregistered command and args (if any)
        ret.args.push_back(*(it++));
    }
    return ret;
}

std::vector<std::string> ArgsManager::GetArgs(const std::string& strArg) const
{
    std::vector<std::string> result;
    for (const util::SettingsValue& value : GetSettingsList(strArg)) {
        result.push_back(value.isFalse() ? "0" : value.isTrue() ? "1" : value.get_str());
    }
    return result;
}

bool ArgsManager::IsArgSet(const std::string& strArg) const
{
    return !GetSetting(strArg).isNull();
}

bool ArgsManager::InitSettings(std::string& error)
{
    if (!GetSettingsPath()) {
        return true; // Do nothing if settings file disabled.
    }

    std::vector<std::string> errors;
    if (!ReadSettingsFile(&errors)) {
        error = strprintf("Failed loading settings file:\n- %s\n", Join(errors, "\n- "));
        return false;
    }
    if (!WriteSettingsFile(&errors)) {
        error = strprintf("Failed saving settings file:\n- %s\n", Join(errors, "\n- "));
        return false;
    }
    return true;
}

bool ArgsManager::GetSettingsPath(fs::path* filepath, bool temp) const
{
    if (IsArgNegated("-settings")) {
        return false;
    }
    if (filepath) {
        std::string settings = GetArg("-settings", BITCOIN_SETTINGS_FILENAME);
        *filepath = fsbridge::AbsPathJoin(GetDataDirNet(), temp ? settings + ".tmp" : settings);
    }
    return true;
}

static void SaveErrors(const std::vector<std::string> errors, std::vector<std::string>* error_out)
{
    for (const auto& error : errors) {
        if (error_out) {
            error_out->emplace_back(error);
        } else {
            LogPrintf("%s\n", error);
        }
    }
}

bool ArgsManager::ReadSettingsFile(std::vector<std::string>* errors)
{
    fs::path path;
    if (!GetSettingsPath(&path, /* temp= */ false)) {
        return true; // Do nothing if settings file disabled.
    }

    LOCK(cs_args);
    m_settings.rw_settings.clear();
    std::vector<std::string> read_errors;
    if (!util::ReadSettings(path, m_settings.rw_settings, read_errors)) {
        SaveErrors(read_errors, errors);
        return false;
    }
    for (const auto& setting : m_settings.rw_settings) {
        std::string section;
        std::string key = setting.first;
        (void)InterpretOption(section, key, /* value */ {}); // Split setting key into section and argname
        if (!GetArgFlags('-' + key)) {
            LogPrintf("Ignoring unknown rw_settings value %s\n", setting.first);
        }
    }
    return true;
}

bool ArgsManager::WriteSettingsFile(std::vector<std::string>* errors) const
{
    fs::path path, path_tmp;
    if (!GetSettingsPath(&path, /* temp= */ false) || !GetSettingsPath(&path_tmp, /* temp= */ true)) {
        throw std::logic_error("Attempt to write settings file when dynamic settings are disabled.");
    }

    LOCK(cs_args);
    std::vector<std::string> write_errors;
    if (!util::WriteSettings(path_tmp, m_settings.rw_settings, write_errors)) {
        SaveErrors(write_errors, errors);
        return false;
    }
    if (!RenameOver(path_tmp, path)) {
        SaveErrors({strprintf("Failed renaming settings file %s to %s\n", path_tmp.string(), path.string())}, errors);
        return false;
    }
    return true;
}

bool ArgsManager::IsArgNegated(const std::string& strArg) const
{
    return GetSetting(strArg).isFalse();
}

std::string ArgsManager::GetArg(const std::string& strArg, const std::string& strDefault) const
{
    const util::SettingsValue value = GetSetting(strArg);
    return value.isNull() ? strDefault : value.isFalse() ? "0" : value.isTrue() ? "1" : value.get_str();
}

int64_t ArgsManager::GetArg(const std::string& strArg, int64_t nDefault) const
{
    const util::SettingsValue value = GetSetting(strArg);
    return value.isNull() ? nDefault : value.isFalse() ? 0 : value.isTrue() ? 1 : value.isNum() ? value.get_int64() : atoi64(value.get_str());
}

bool ArgsManager::GetBoolArg(const std::string& strArg, bool fDefault) const
{
    const util::SettingsValue value = GetSetting(strArg);
    return value.isNull() ? fDefault : value.isBool() ? value.get_bool() : InterpretBool(value.get_str());
}

bool ArgsManager::SoftSetArg(const std::string& strArg, const std::string& strValue)
{
    LOCK(cs_args);
    if (IsArgSet(strArg)) return false;
    ForceSetArg(strArg, strValue);
    return true;
}

bool ArgsManager::SoftSetBoolArg(const std::string& strArg, bool fValue)
{
    if (fValue)
        return SoftSetArg(strArg, std::string("1"));
    else
        return SoftSetArg(strArg, std::string("0"));
}

void ArgsManager::ForceSetArg(const std::string& strArg, const std::string& strValue)
{
    LOCK(cs_args);
    m_settings.forced_settings[SettingName(strArg)] = strValue;
}

void ArgsManager::ForceSetArg(const std::string& strArg, int64_t nValue)
{
    ForceSetArg(strArg, ToString(nValue));
}

void ArgsManager::AddCommand(const std::string& cmd, const std::string& help)
{
    Assert(cmd.find('=') == std::string::npos);
    Assert(cmd.at(0) != '-');

    LOCK(cs_args);
    m_accept_any_command = false; // latch to false
    std::map<std::string, Arg>& arg_map = m_available_args[OptionsCategory::COMMANDS];
    auto ret = arg_map.emplace(cmd, Arg{"", help, ArgsManager::COMMAND});
    Assert(ret.second); // Fail on duplicate commands
}

void ArgsManager::AddArg(const std::string& name, const std::string& help, unsigned int flags, const OptionsCategory& cat)
{
    Assert((flags & ArgsManager::COMMAND) == 0); // use AddCommand

    // Split arg name from its help param
    size_t eq_index = name.find('=');
    if (eq_index == std::string::npos) {
        eq_index = name.size();
    }
    std::string arg_name = name.substr(0, eq_index);

    LOCK(cs_args);
    std::map<std::string, Arg>& arg_map = m_available_args[cat];
    auto ret = arg_map.emplace(arg_name, Arg{name.substr(eq_index, name.size() - eq_index), help, flags});
    assert(ret.second); // Make sure an insertion actually happened

    if (flags & ArgsManager::NETWORK_ONLY) {
        m_network_only_args.emplace(arg_name);
    }
}

void ArgsManager::AddHiddenArgs(const std::vector<std::string>& names)
{
    for (const std::string& name : names) {
        AddArg(name, "", ArgsManager::ALLOW_ANY, OptionsCategory::HIDDEN);
    }
}

std::string ArgsManager::GetHelpMessage() const
{
    const bool show_debug = GetBoolArg("-help-debug", false);

    std::string usage = "";
    LOCK(cs_args);
    for (const auto& arg_map : m_available_args) {
        switch(arg_map.first) {
            case OptionsCategory::OPTIONS:
                usage += HelpMessageGroup("Options:");
                break;
            case OptionsCategory::CONNECTION:
                usage += HelpMessageGroup("Connection options:");
                break;
            case OptionsCategory::ZMQ:
                usage += HelpMessageGroup("ZeroMQ notification options:");
                break;
            case OptionsCategory::DEBUG_TEST:
                usage += HelpMessageGroup("Debugging/Testing options:");
                break;
            case OptionsCategory::NODE_RELAY:
                usage += HelpMessageGroup("Node relay options:");
                break;
            case OptionsCategory::BLOCK_CREATION:
                usage += HelpMessageGroup("Block creation options:");
                break;
            case OptionsCategory::RPC:
                usage += HelpMessageGroup("RPC server options:");
                break;
            case OptionsCategory::WALLET:
                usage += HelpMessageGroup("Wallet options:");
                break;
            case OptionsCategory::WALLET_DEBUG_TEST:
                if (show_debug) usage += HelpMessageGroup("Wallet debugging/testing options:");
                break;
            case OptionsCategory::CHAINPARAMS:
                usage += HelpMessageGroup("Chain selection options:");
                break;
            case OptionsCategory::GUI:
                usage += HelpMessageGroup("UI Options:");
                break;
            case OptionsCategory::COMMANDS:
                usage += HelpMessageGroup("Commands:");
                break;
            case OptionsCategory::REGISTER_COMMANDS:
                usage += HelpMessageGroup("Register Commands:");
                break;
            case OptionsCategory::STATS:
                usage += HelpMessageGroup("Statistic options:");
                break;
            default:
                break;
        }

        // When we get to the hidden options, stop
        if (arg_map.first == OptionsCategory::HIDDEN) break;

        for (const auto& arg : arg_map.second) {
            if (show_debug || !(arg.second.m_flags & ArgsManager::DEBUG_ONLY)) {
                std::string name;
                if (arg.second.m_help_param.empty()) {
                    name = arg.first;
                } else {
                    name = arg.first + arg.second.m_help_param;
                }
                usage += HelpMessageOpt(name, arg.second.m_help_text);
            }
        }
    }
    return usage;
}

bool HelpRequested(const ArgsManager& args)
{
    return args.IsArgSet("-?") || args.IsArgSet("-h") || args.IsArgSet("-help") || args.IsArgSet("-help-debug");
}

void SetupHelpOptions(ArgsManager& args)
{
    args.AddArg("-?", "Print this help message and exit", ArgsManager::ALLOW_ANY, OptionsCategory::OPTIONS);
    args.AddHiddenArgs({"-h", "-help"});
}

static const int screenWidth = 79;
static const int optIndent = 2;
static const int msgIndent = 7;

std::string HelpMessageGroup(const std::string &message) {
    return std::string(message) + std::string("\n\n");
}

std::string HelpMessageOpt(const std::string &option, const std::string &message) {
    return std::string(optIndent,' ') + std::string(option) +
           std::string("\n") + std::string(msgIndent,' ') +
           FormatParagraph(message, screenWidth - msgIndent, msgIndent) +
           std::string("\n\n");
}

static std::string FormatException(const std::exception* pex, const char* pszThread)
{
#ifdef WIN32
    char pszModule[MAX_PATH] = "";
    GetModuleFileNameA(nullptr, pszModule, sizeof(pszModule));
#else
    const char* pszModule = "bitcoin";
#endif
    if (pex)
        return strprintf(
            "EXCEPTION: %s       \n%s       \n%s in %s       \n", typeid(*pex).name(), pex->what(), pszModule, pszThread);
    else
        return strprintf(
            "UNKNOWN EXCEPTION       \n%s in %s       \n", pszModule, pszThread);
}

void PrintExceptionContinue(const std::exception* pex, const char* pszThread)
{
    std::string message = FormatException(pex, pszThread);
    LogPrintf("\n\n************************\n%s\n", message);
    tfm::format(std::cerr, "\n\n************************\n%s\n", message);
}

fs::path GetDefaultDataDir()
{
    // Windows: C:\Users\Username\AppData\Roaming\Bitcoin
    // macOS: ~/Library/Application Support/Bitcoin
    // Unix-like: ~/.bitcoin
#ifdef WIN32
    // Windows
    return GetSpecialFolderPath(CSIDL_APPDATA) / "Bitcoin";
#else
    fs::path pathRet;
    char* pszHome = getenv("HOME");
    if (pszHome == nullptr || strlen(pszHome) == 0)
        pathRet = fs::path("/");
    else
        pathRet = fs::path(pszHome);
#ifdef MAC_OSX
    // macOS
    return pathRet / "Library/Application Support/Bitcoin";
#else
    // Unix-like
    return pathRet / ".bitcoin";
#endif
#endif
}

bool CheckDataDirOption()
{
    std::string datadir = gArgs.GetArg("-datadir", "");
    return datadir.empty() || fs::is_directory(fs::system_complete(datadir));
}

fs::path GetConfigFile(const std::string& confPath)
{
    return AbsPathForConfigVal(fs::path(confPath), false);
}

fs::path GetRWConfigFile(const std::string& confPath)
{
    return AbsPathForConfigVal(fs::path(confPath));
}

static bool GetConfigOptions(std::istream& stream, const std::string& filepath, std::string& error, std::vector<std::pair<std::string, std::string>>& options, std::list<SectionInfo>& sections)
{
    std::string str, prefix;
    std::string::size_type pos;
    int linenr = 1;
    while (std::getline(stream, str)) {
        bool used_hash = false;
        if ((pos = str.find('#')) != std::string::npos) {
            str = str.substr(0, pos);
            used_hash = true;
        }
        const static std::string pattern = " \t\r\n";
        str = TrimString(str, pattern);
        if (!str.empty()) {
            if (*str.begin() == '[' && *str.rbegin() == ']') {
                const std::string section = str.substr(1, str.size() - 2);
                sections.emplace_back(SectionInfo{section, filepath, linenr});
                prefix = section + '.';
            } else if (*str.begin() == '-') {
                error = strprintf("parse error on line %i: %s, options in configuration file must be specified without leading -", linenr, str);
                return false;
            } else if ((pos = str.find('=')) != std::string::npos) {
                std::string name = prefix + TrimString(str.substr(0, pos), pattern);
                std::string value = TrimString(str.substr(pos + 1), pattern);
                if (used_hash && name.find("rpcpassword") != std::string::npos) {
                    error = strprintf("parse error on line %i, using # in rpcpassword can be ambiguous and should be avoided", linenr);
                    return false;
                }
                options.emplace_back(name, value);
                if ((pos = name.rfind('.')) != std::string::npos && prefix.length() <= pos) {
                    sections.emplace_back(SectionInfo{name.substr(0, pos), filepath, linenr});
                }
            } else {
                error = strprintf("parse error on line %i: %s", linenr, str);
                if (str.size() >= 2 && str.substr(0, 2) == "no") {
                    error += strprintf(", if you intended to specify a negated option, use %s=1 instead", str);
                }
                return false;
            }
        }
        ++linenr;
    }
    return true;
}

bool ArgsManager::ReadConfigStream(std::istream& stream, const std::string& filepath, std::string& error, bool ignore_invalid_keys, std::map<std::string, std::vector<util::SettingsValue>>* settings_target)
{
    LOCK(cs_args);
    std::vector<std::pair<std::string, std::string>> options;
    if (!GetConfigOptions(stream, filepath, error, options, m_config_sections)) {
        return false;
    }
    for (const std::pair<std::string, std::string>& option : options) {
        std::string section;
        std::string key = option.first;
        util::SettingsValue value = InterpretOption(section, key, option.second);
        std::optional<unsigned int> flags = GetArgFlags('-' + key);
        if (flags) {
            if (!CheckValid(key, value, *flags, error)) {
                return false;
            }
            if (settings_target) {
                (*settings_target)[key].push_back(value);
            } else
            m_settings.ro_config[section][key].push_back(value);
        } else {
            if (ignore_invalid_keys) {
                LogPrintf("Ignoring unknown configuration value %s\n", option.first);
            } else {
                error = strprintf("Invalid configuration value %s", option.first);
                return false;
            }
        }
    }
    return true;
}

bool ArgsManager::ReadConfigFiles(std::string& error, bool ignore_invalid_keys)
{
    {
        LOCK(cs_args);
        m_settings.ro_config.clear();
        m_settings.rw_config.clear();
        rwconf_had_prune_option = false;
        m_config_sections.clear();
    }

    const std::string confPath = GetArg("-conf", BITCOIN_CONF_FILENAME);
    fsbridge::ifstream stream(GetConfigFile(confPath));

    // ok to not have a config file
    if (stream.good()) {
        if (!ReadConfigStream(stream, confPath, error, ignore_invalid_keys)) {
            return false;
        }
        // `-includeconf` cannot be included in the command line arguments except
        // as `-noincludeconf` (which indicates that no included conf file should be used).
        bool use_conf_file{true};
        {
            LOCK(cs_args);
            if (auto* includes = util::FindKey(m_settings.command_line_options, "includeconf")) {
                // ParseParameters() fails if a non-negated -includeconf is passed on the command-line
                assert(util::SettingsSpan(*includes).last_negated());
                use_conf_file = false;
            }
        }
        if (use_conf_file) {
            std::string chain_id = GetChainName();
            std::vector<std::string> conf_file_names;

            auto add_includes = [&](const std::string& network, size_t skip = 0) {
                size_t num_values = 0;
                LOCK(cs_args);
                if (auto* section = util::FindKey(m_settings.ro_config, network)) {
                    if (auto* values = util::FindKey(*section, "includeconf")) {
                        for (size_t i = std::max(skip, util::SettingsSpan(*values).negated()); i < values->size(); ++i) {
                            conf_file_names.push_back((*values)[i].get_str());
                        }
                        num_values = values->size();
                    }
                }
                return num_values;
            };

            // We haven't set m_network yet (that happens in SelectParams()), so manually check
            // for network.includeconf args.
            const size_t chain_includes = add_includes(chain_id);
            const size_t default_includes = add_includes({});

            for (const std::string& conf_file_name : conf_file_names) {
                fsbridge::ifstream conf_file_stream(GetConfigFile(conf_file_name));
                if (conf_file_stream.good()) {
                    if (!ReadConfigStream(conf_file_stream, conf_file_name, error, ignore_invalid_keys)) {
                        return false;
                    }
                    LogPrintf("Included configuration file %s\n", conf_file_name);
                } else {
                    error = "Failed to include configuration file " + conf_file_name;
                    return false;
                }
            }

            // Warn about recursive -includeconf
            conf_file_names.clear();
            add_includes(chain_id, /* skip= */ chain_includes);
            add_includes({}, /* skip= */ default_includes);
            std::string chain_id_final = GetChainName();
            if (chain_id_final != chain_id) {
                // Also warn about recursive includeconf for the chain that was specified in one of the includeconfs
                add_includes(chain_id_final);
            }
            for (const std::string& conf_file_name : conf_file_names) {
                tfm::format(std::cerr, "warning: -includeconf cannot be used from included files; ignoring -includeconf=%s\n", conf_file_name);
            }
        }
    }

    // Check for chain settings (BaseParams() calls are only valid after this clause)
    try {
        SelectBaseParams(gArgs.GetChainName());
    } catch (const std::exception& e) {
        error = e.what();
        return false;
    }

    // If datadir is changed in .conf file:
    gArgs.ClearPathCache();
    if (!CheckDataDirOption()) {
        error = strprintf("specified data directory \"%s\" does not exist.", GetArg("-datadir", ""));
        return false;
    }

    const std::string rwconf_path_str = GetArg("-confrw", BITCOIN_RW_CONF_FILENAME);
    const auto new_rwconf_path = GetRWConfigFile(rwconf_path_str);
    LOCK(cs_args);
    rwconf_path = new_rwconf_path;
    fs::ifstream rwconf_stream(rwconf_path);
    if (rwconf_stream.good()) {
        if (!ReadConfigStream(rwconf_stream, rwconf_path_str, error, ignore_invalid_keys, &m_settings.rw_config)) {
            return false;
        }
        rwconf_had_prune_option = m_settings.rw_config.count("prune");
    }

    return true;
}

std::string ArgsManager::GetChainName() const
{
    auto get_net = [&](const std::string& arg) {
        LOCK(cs_args);
        util::SettingsValue value = util::GetSetting(m_settings, /* section= */ "", SettingName(arg),
            /* ignore_default_section_config= */ false,
            /* get_chain_name= */ true);
        return value.isNull() ? false : value.isBool() ? value.get_bool() : InterpretBool(value.get_str());
    };

    const bool fRegTest = get_net("-regtest");
    const bool fSigNet  = get_net("-signet");
    const bool fTestNet = get_net("-testnet");
    const bool is_chain_arg_set = IsArgSet("-chain");

    if ((int)is_chain_arg_set + (int)fRegTest + (int)fSigNet + (int)fTestNet > 1) {
        throw std::runtime_error("Invalid combination of -regtest, -signet, -testnet and -chain. Can use at most one.");
    }
    if (fRegTest)
        return CBaseChainParams::REGTEST;
    if (fSigNet) {
        return CBaseChainParams::SIGNET;
    }
    if (fTestNet)
        return CBaseChainParams::TESTNET;

    return GetArg("-chain", CBaseChainParams::MAIN);
}

bool ArgsManager::UseDefaultSection(const std::string& arg) const
{
    return m_network == CBaseChainParams::MAIN || m_network_only_args.count(arg) == 0;
}

util::SettingsValue ArgsManager::GetSetting(const std::string& arg) const
{
    LOCK(cs_args);
    return util::GetSetting(
        m_settings, m_network, SettingName(arg), !UseDefaultSection(arg), /* get_chain_name= */ false);
}

std::vector<util::SettingsValue> ArgsManager::GetSettingsList(const std::string& arg) const
{
    LOCK(cs_args);
    return util::GetSettingsList(m_settings, m_network, SettingName(arg), !UseDefaultSection(arg));
}

void ArgsManager::logArgsPrefix(
    const std::string& prefix,
    const std::string& section,
    const std::map<std::string, std::vector<util::SettingsValue>>& args) const
{
    std::string section_str = section.empty() ? "" : "[" + section + "] ";
    for (const auto& arg : args) {
        for (const auto& value : arg.second) {
            std::optional<unsigned int> flags = GetArgFlags('-' + arg.first);
            if (flags) {
                std::string value_str = (*flags & SENSITIVE) ? "****" : value.write();
                LogPrintf("%s %s%s=%s\n", prefix, section_str, arg.first, value_str);
            }
        }
    }
}

void ArgsManager::LogArgs() const
{
    LOCK(cs_args);
    for (const auto& section : m_settings.ro_config) {
        logArgsPrefix("Config file arg:", section.first, section.second);
    }
    for (const auto& setting : m_settings.rw_settings) {
        LogPrintf("Setting file arg: %s = %s\n", setting.first, setting.second.write());
    }
    logArgsPrefix("R/W config file arg:", "", m_settings.rw_config);
    logArgsPrefix("Command-line arg:", "", m_settings.command_line_options);
}

namespace {

    // Like std::getline, but includes the EOL character in the result
    bool getline_with_eol(std::istream& stream, std::string& result)
    {
        int current_char;
        current_char = stream.get();
        if (current_char == std::char_traits<char>::eof()) {
            return false;
        }
        result.clear();
        result.push_back(char(current_char));
        while (current_char != '\n') {
            current_char = stream.get();
            if (current_char == std::char_traits<char>::eof()) {
                break;
            }
            result.push_back(char(current_char));
        }
        return true;
    }

    const char * const ModifyRWConfigFile_ws_chars = " \t\r\n";

    void ModifyRWConfigFile_SanityCheck(const std::string& s)
    {
        if (s.empty()) {
            // Dereferencing .begin or .rbegin below is invalid unless the string has at least one character.
            return;
        }

        static const char * const newline_chars = "\r\n";
        static std::string ws_chars(ModifyRWConfigFile_ws_chars);
        if (s.find_first_of(newline_chars) != std::string::npos) {
            throw std::invalid_argument("New-line in config name/value");
        }
        if (ws_chars.find(*s.begin()) != std::string::npos || ws_chars.find(*s.rbegin()) != std::string::npos) {
            throw std::invalid_argument("Config name/value has leading/trailing whitespace");
        }
    }

    void ModifyRWConfigFile_WriteRemaining(std::ostream& stream_out, const std::map<std::string, std::string>& settings_to_change, std::set<std::string>& setFound)
    {
        for (const auto& setting_pair : settings_to_change) {
            const std::string& key = setting_pair.first;
            const std::string& val = setting_pair.second;
            if (setFound.find(key) != setFound.end()) {
                continue;
            }
            setFound.insert(key);
            ModifyRWConfigFile_SanityCheck(key);
            ModifyRWConfigFile_SanityCheck(val);
            stream_out << key << "=" << val << "\n";
        }
    }
} // namespace

void ModifyRWConfigStream(std::istream& stream_in, std::ostream& stream_out, const std::map<std::string, std::string>& settings_to_change)
{
    static const char * const ws_chars = ModifyRWConfigFile_ws_chars;
    std::set<std::string> setFound;
    std::string s, lineend, linebegin, key;
    std::string::size_type n, n2;
    bool inside_group = false, have_eof_nl = true;
    std::map<std::string, std::string>::const_iterator iterCS;
    size_t lineno = 0;
    while (getline_with_eol(stream_in, s)) {
        ++lineno;

        have_eof_nl = (!s.empty()) && (*s.rbegin() == '\n');
        n = s.find('#');
        const bool has_comment = (n != std::string::npos);
        if (!has_comment) {
            n = s.size();
        }
        if (n > 0) {
            n2 = s.find_last_not_of(ws_chars, n - 1);
            if (n2 != std::string::npos) {
                n = n2 + 1;
            }
        }
        n2 = s.find_first_not_of(ws_chars);
        if (n2 == std::string::npos || n2 >= n) {
            // Blank or comment-only line
            stream_out << s;
            continue;
        }
        lineend = s.substr(n);
        linebegin = s.substr(0, n2);
        s = s.substr(n2, n - n2);

        // It is impossible for s to be empty here, due to the blank line check above
        if (*s.begin() == '[' && *s.rbegin() == ']') {
            // We don't use sections, so we could possibly just write out the rest of the file - but we need to check for unparsable lines, so we just set a flag to ignore settings from here on
            ModifyRWConfigFile_WriteRemaining(stream_out, settings_to_change, setFound);
            inside_group = true;
            key.clear();

            stream_out << linebegin << s << lineend;
            continue;
        }

        n = s.find('=');
        if (n == std::string::npos) {
            // Bad line; this causes boost to throw an exception when parsing, so we comment out the entire file
            stream_in.seekg(0, std::ios_base::beg);
            stream_out.seekp(0, std::ios_base::beg);
            if (!(stream_in.good() && stream_out.good())) {
                throw std::ios_base::failure("Failed to rewind (to comment out existing file)");
            }
            // First, write out all the settings we intend to set
            setFound.clear();
            ModifyRWConfigFile_WriteRemaining(stream_out, settings_to_change, setFound);
            // We then define a category to ensure new settings get added before the invalid stuff
            stream_out << "[INVALID]\n";
            // Then, describe the problem in a comment
            stream_out << "# Error parsing line " << lineno << ": " << s << "\n";
            // Finally, dump the rest of the file commented out
            while (getline_with_eol(stream_in, s)) {
                stream_out << "#" << s;
            }
            return;
        }

        if (!inside_group) {
            // We don't support/use groups, so once we're inside key is always null to avoid setting anything
            n2 = s.find_last_not_of(ws_chars, n - 1);
            if (n2 == std::string::npos) {
                n2 = n - 1;
            } else {
                ++n2;
            }
            key = s.substr(0, n2);
        }
        if ((!key.empty()) && (iterCS = settings_to_change.find(key)) != settings_to_change.end() && setFound.find(key) == setFound.end()) {
            // This is the key we want to change
            const std::string& val = iterCS->second;
            setFound.insert(key);
            ModifyRWConfigFile_SanityCheck(val);
            if (has_comment) {
                // Rather than change a commented line, comment it out entirely (the existing comment may relate to the value) and replace it
                stream_out << key << "=" << val << "\n";
                linebegin.insert(linebegin.begin(), '#');
            } else {
                // Just modify the value in-line otherwise
                n2 = s.find_first_not_of(ws_chars, n + 1);
                if (n2 == std::string::npos) {
                    n2 = n + 1;
                }
                s = s.substr(0, n2) + val;
            }
        }
        stream_out << linebegin << s << lineend;
    }
    if (setFound.size() < settings_to_change.size()) {
        if (!have_eof_nl) {
            stream_out << "\n";
        }
        ModifyRWConfigFile_WriteRemaining(stream_out, settings_to_change, setFound);
    }
}

void ArgsManager::ModifyRWConfigFile(const std::map<std::string, std::string>& settings_to_change, const bool also_settings_json)
{
    LOCK(cs_args);
    assert(!rwconf_path.empty());
    fs::path rwconf_new_path = rwconf_path;
    rwconf_new_path += ".new";
    const std::string new_path_str = rwconf_new_path.string();
    try {
        std::remove(new_path_str.c_str());
        fs::ofstream streamRWConfigOut(rwconf_new_path, std::ios_base::out | std::ios_base::trunc);
        if (fs::exists(rwconf_path)) {
            fs::ifstream streamRWConfig(rwconf_path);
            ::ModifyRWConfigStream(streamRWConfig, streamRWConfigOut, settings_to_change);
        } else {
            std::istringstream streamIn;
            ::ModifyRWConfigStream(streamIn, streamRWConfigOut, settings_to_change);
        }
    } catch (...) {
        std::remove(new_path_str.c_str());
        throw;
    }
    if (!RenameOver(rwconf_new_path, rwconf_path)) {
        std::remove(new_path_str.c_str());
        throw std::ios_base::failure(strprintf("Failed to replace %s", new_path_str));
    }
    if (also_settings_json && !IsArgNegated("-settings")) {
        // Also save to settings.json for Core (0.21+) compatibility
        for (const auto& setting_change : settings_to_change) {
            m_settings.rw_settings[setting_change.first] = setting_change.second;
        }
        WriteSettingsFile();
    }
    if (settings_to_change.count("prune")) {
        rwconf_had_prune_option = true;
    }
}

void ArgsManager::ModifyRWConfigFile(const std::string& setting_to_change, const std::string& new_value, const bool also_settings_json)
{
    std::map<std::string, std::string> settings_to_change;
    settings_to_change[setting_to_change] = new_value;
    ModifyRWConfigFile(settings_to_change, also_settings_json);
}

void ArgsManager::EraseRWConfigFile()
{
    LOCK(cs_args);
    assert(!rwconf_path.empty());
    if (!fs::exists(rwconf_path)) {
        return;
    }
    if (!IsArgNegated("-settings")) {
        // Also remove from settings.json (stored for Core (0.21+) compatibility)
        fs::ifstream rwconf_stream(rwconf_path);
        if (!rwconf_stream.good()) {
            throw std::ios_base::failure(strprintf("%s: Failed to open %s", __func__, rwconf_path));
        }
        std::string error;
        std::map<std::string, std::vector<util::SettingsValue>> current_rwconf_settings;
        if (!ReadConfigStream(rwconf_stream, rwconf_path.string(), error, /* ignore_invalid_keys= */ true, &current_rwconf_settings)) {
            throw std::ios_base::failure(strprintf("%s: Failed to read %s: %s", __func__, rwconf_path, error));
        }
        for (const auto& setting : current_rwconf_settings) {
            m_settings.rw_settings.erase(setting.first);
        }
        WriteSettingsFile();
    }
    fs::path rwconf_reset_path = rwconf_path;
    rwconf_reset_path += ".reset";
    if (!RenameOver(rwconf_path, rwconf_reset_path)) {
        const std::string path_str = rwconf_path.string();
        if (std::remove(path_str.c_str())) {
            throw std::ios_base::failure(strprintf("Failed to remove %s", path_str));
        }
    }
}

bool RenameOver(fs::path src, fs::path dest)
{
#ifdef WIN32
    return MoveFileExW(src.wstring().c_str(), dest.wstring().c_str(),
                       MOVEFILE_REPLACE_EXISTING) != 0;
#else
    int rc = std::rename(src.string().c_str(), dest.string().c_str());
    return (rc == 0);
#endif /* WIN32 */
}

/**
 * Ignores exceptions thrown by Boost's create_directories if the requested directory exists.
 * Specifically handles case where path p exists, but it wasn't possible for the user to
 * write to the parent directory.
 */
bool TryCreateDirectories(const fs::path& p)
{
    try
    {
        return fs::create_directories(p);
    } catch (const fs::filesystem_error&) {
        if (!fs::exists(p) || !fs::is_directory(p))
            throw;
    }

    // create_directories didn't create the directory, it had to have existed already
    return false;
}

bool FileCommit(FILE *file)
{
    if (fflush(file) != 0) { // harmless if redundantly called
        LogPrintf("%s: fflush failed: %d\n", __func__, errno);
        return false;
    }
#ifdef WIN32
    HANDLE hFile = (HANDLE)_get_osfhandle(_fileno(file));
    if (FlushFileBuffers(hFile) == 0) {
        LogPrintf("%s: FlushFileBuffers failed: %d\n", __func__, GetLastError());
        return false;
    }
#elif defined(MAC_OSX) && defined(F_FULLFSYNC)
    if (fcntl(fileno(file), F_FULLFSYNC, 0) == -1) { // Manpage says "value other than -1" is returned on success
        LogPrintf("%s: fcntl F_FULLFSYNC failed: %d\n", __func__, errno);
        return false;
    }
#elif HAVE_FDATASYNC
    if (fdatasync(fileno(file)) != 0 && errno != EINVAL) { // Ignore EINVAL for filesystems that don't support sync
        LogPrintf("%s: fdatasync failed: %d\n", __func__, errno);
        return false;
    }
#else
    if (fsync(fileno(file)) != 0 && errno != EINVAL) {
        LogPrintf("%s: fsync failed: %d\n", __func__, errno);
        return false;
    }
#endif
    return true;
}

void DirectoryCommit(const fs::path &dirname)
{
#ifndef WIN32
    FILE* file = fsbridge::fopen(dirname, "r");
    if (file) {
        fsync(fileno(file));
        fclose(file);
    }
#endif
}

bool TruncateFile(FILE *file, unsigned int length) {
#if defined(WIN32)
    return _chsize(_fileno(file), length) == 0;
#else
    return ftruncate(fileno(file), length) == 0;
#endif
}

/**
 * this function tries to raise the file descriptor limit to the requested number.
 * It returns the actual file descriptor limit (which may be more or less than nMinFD)
 */
int RaiseFileDescriptorLimit(int nMinFD) {
#if defined(WIN32)
    return 2048;
#else
    struct rlimit limitFD;
    if (getrlimit(RLIMIT_NOFILE, &limitFD) != -1) {
        if (limitFD.rlim_cur < (rlim_t)nMinFD) {
            limitFD.rlim_cur = nMinFD;
            if (limitFD.rlim_cur > limitFD.rlim_max)
                limitFD.rlim_cur = limitFD.rlim_max;
            setrlimit(RLIMIT_NOFILE, &limitFD);
            getrlimit(RLIMIT_NOFILE, &limitFD);
        }
        return limitFD.rlim_cur;
    }
    return nMinFD; // getrlimit failed, assume it's fine
#endif
}

/**
 * this function tries to make a particular range of a file allocated (corresponding to disk space)
 * it is advisory, and the range specified in the arguments will never contain live data
 */
void AllocateFileRange(FILE *file, unsigned int offset, unsigned int length) {
#if defined(WIN32)
    // Windows-specific version
    HANDLE hFile = (HANDLE)_get_osfhandle(_fileno(file));
    LARGE_INTEGER nFileSize;
    int64_t nEndPos = (int64_t)offset + length;
    nFileSize.u.LowPart = nEndPos & 0xFFFFFFFF;
    nFileSize.u.HighPart = nEndPos >> 32;
    SetFilePointerEx(hFile, nFileSize, 0, FILE_BEGIN);
    SetEndOfFile(hFile);
#elif defined(MAC_OSX)
    // OSX specific version
    // NOTE: Contrary to other OS versions, the OSX version assumes that
    // NOTE: offset is the size of the file.
    fstore_t fst;
    fst.fst_flags = F_ALLOCATECONTIG;
    fst.fst_posmode = F_PEOFPOSMODE;
    fst.fst_offset = 0;
    fst.fst_length = length; // mac os fst_length takes the # of free bytes to allocate, not desired file size
    fst.fst_bytesalloc = 0;
    if (fcntl(fileno(file), F_PREALLOCATE, &fst) == -1) {
        fst.fst_flags = F_ALLOCATEALL;
        fcntl(fileno(file), F_PREALLOCATE, &fst);
    }
    ftruncate(fileno(file), static_cast<off_t>(offset) + length);
#else
    #if defined(HAVE_POSIX_FALLOCATE)
    // Use posix_fallocate to advise the kernel how much data we have to write,
    // if this system supports it.
    off_t nEndPos = (off_t)offset + length;
    if (0 == posix_fallocate(fileno(file), 0, nEndPos)) return;
    #endif
    // Fallback version
    // TODO: just write one byte per block
    static const char buf[65536] = {};
    if (fseek(file, offset, SEEK_SET)) {
        return;
    }
    while (length > 0) {
        unsigned int now = 65536;
        if (length < now)
            now = length;
        fwrite(buf, 1, now, file); // allowed to fail; this function is advisory anyway
        length -= now;
    }
#endif
}

FILE* AdviseSequential(FILE *file) {
#if _POSIX_C_SOURCE >= 200112L
    // Since this whole thing is advisory anyway, we can ignore any errors
    // encountered up to and including the posix_fadvise call. However, we must
    // rewind the file to the appropriate position if we've changed the seek
    // offset.
    if (file == nullptr) {
        return nullptr;
    }
    const int fd = fileno(file);
    if (fd == -1) {
        return file;
    }
    const off_t start = lseek(fd, 0, SEEK_CUR);
    if (start == -1) {
        return file;
    }
    posix_fadvise(fd, start, 0, POSIX_FADV_WILLNEED);
    posix_fadvise(fd, start, 0, POSIX_FADV_SEQUENTIAL);
#endif
    return file;
}

int CloseAndUncache(FILE *file) {
#if _POSIX_C_SOURCE >= 200112L
    // Ignore any errors up to and including the posix_fadvise call since it's
    // advisory.
    if (file != nullptr) {
        const int fd = fileno(file);
        if (fd != -1) {
            const off_t end = lseek(fd, 0, SEEK_END);
            if (end != (off_t)-1) {
                posix_fadvise(fd, 0, end, POSIX_FADV_DONTNEED);
            }
        }
    }
#endif
    return fclose(file);
}

#ifdef WIN32
fs::path GetSpecialFolderPath(int nFolder, bool fCreate)
{
    WCHAR pszPath[MAX_PATH] = L"";

    if(SHGetSpecialFolderPathW(nullptr, pszPath, nFolder, fCreate))
    {
        return fs::path(pszPath);
    }

    LogPrintf("SHGetSpecialFolderPathW() failed, could not obtain requested path.\n");
    return fs::path("");
}
#endif

std::string ShellEscape(const std::string& arg)
{
    std::string escaped = arg;
    boost::replace_all(escaped, "'", "'\\''");
    return "'" + escaped + "'";
}

#if HAVE_SYSTEM
void runCommand(const std::string& strCommand)
{
    if (strCommand.empty()) return;
#ifndef WIN32
    int nErr = ::system(strCommand.c_str());
#else
    int nErr = ::_wsystem(std::wstring_convert<std::codecvt_utf8_utf16<wchar_t>,wchar_t>().from_bytes(strCommand).c_str());
#endif
    if (nErr)
        LogPrintf("runCommand error: system(%s) returned %d\n", strCommand, nErr);
}
#endif

UniValue RunCommandParseJSON(const std::string& str_command, const std::string& str_std_in)
{
#ifdef ENABLE_EXTERNAL_SIGNER
    namespace bp = boost::process;

    UniValue result_json;
    bp::opstream stdin_stream;
    bp::ipstream stdout_stream;
    bp::ipstream stderr_stream;

    if (str_command.empty()) return UniValue::VNULL;

    bp::child c(
        str_command,
        bp::std_out > stdout_stream,
        bp::std_err > stderr_stream,
        bp::std_in < stdin_stream
#ifdef HAVE_BPE_CLOSE_EXCESS_FDS
        , bpe_close_excess_fds()
#endif
    );
    if (!str_std_in.empty()) {
        stdin_stream << str_std_in << std::endl;
    }
    stdin_stream.pipe().close();

    std::string result;
    std::string error;
    std::getline(stdout_stream, result);
    std::getline(stderr_stream, error);

    c.wait();
    const int n_error = c.exit_code();
    if (n_error) throw std::runtime_error(strprintf("RunCommandParseJSON error: process(%s) returned %d: %s\n", str_command, n_error, error));
    if (!result_json.read(result)) throw std::runtime_error("Unable to parse JSON: " + result);

    return result_json;
#else
    throw std::runtime_error("Compiled without external signing support (required for external signing).");
#endif // ENABLE_EXTERNAL_SIGNER
}

void SetupEnvironment()
{
#ifdef HAVE_MALLOPT_ARENA_MAX
    // glibc-specific: On 32-bit systems set the number of arenas to 1.
    // By default, since glibc 2.10, the C library will create up to two heap
    // arenas per core. This is known to cause excessive virtual address space
    // usage in our usage. Work around it by setting the maximum number of
    // arenas to 1.
    if (sizeof(void*) == 4) {
        mallopt(M_ARENA_MAX, 1);
    }
#endif
    // On most POSIX systems (e.g. Linux, but not BSD) the environment's locale
    // may be invalid, in which case the "C.UTF-8" locale is used as fallback.
#if !defined(WIN32) && !defined(MAC_OSX) && !defined(__FreeBSD__) && !defined(__OpenBSD__) && !defined(__NetBSD__)
    try {
        std::locale(""); // Raises a runtime error if current locale is invalid
    } catch (const std::runtime_error&) {
        setenv("LC_ALL", "C.UTF-8", 1);
    }
#elif defined(WIN32)
    // Set the default input/output charset is utf-8
    SetConsoleCP(CP_UTF8);
    SetConsoleOutputCP(CP_UTF8);
#endif
    // The path locale is lazy initialized and to avoid deinitialization errors
    // in multithreading environments, it is set explicitly by the main thread.
    // A dummy locale is used to extract the internal default locale, used by
    // fs::path, which is then used to explicitly imbue the path.
    std::locale loc = fs::path::imbue(std::locale::classic());
#ifndef WIN32
    fs::path::imbue(loc);
#else
    fs::path::imbue(std::locale(loc, new std::codecvt_utf8_utf16<wchar_t>()));
#endif
}

bool SetupNetworking()
{
#ifdef WIN32
    // Initialize Windows Sockets
    WSADATA wsadata;
    int ret = WSAStartup(MAKEWORD(2,2), &wsadata);
    if (ret != NO_ERROR || LOBYTE(wsadata.wVersion ) != 2 || HIBYTE(wsadata.wVersion) != 2)
        return false;
#endif
    return true;
}

int GetNumCores()
{
    return std::thread::hardware_concurrency();
}

std::string CopyrightHolders(const std::string& strPrefix)
{
    const auto copyright_devs = strprintf(_(COPYRIGHT_HOLDERS).translated, COPYRIGHT_HOLDERS_SUBSTITUTION);
    std::string strCopyrightHolders = strPrefix + copyright_devs;

    // Make sure Bitcoin Core copyright is not removed by accident
    if (copyright_devs.find("Bitcoin Core") == std::string::npos) {
        strCopyrightHolders += "\n" + strPrefix + "The Bitcoin Core developers";
    }
    return strCopyrightHolders;
}

// Obtain the application startup time (used for uptime calculation)
int64_t GetStartupTime()
{
    return nStartupTime;
}

fs::path AbsPathForConfigVal(const fs::path& path, bool net_specific)
{
    if (path.is_absolute()) {
        return path;
    }
    return fsbridge::AbsPathJoin(net_specific ? gArgs.GetDataDirNet() : gArgs.GetDataDirBase(), path);
}

void ScheduleBatchPriority()
{
#ifdef SCHED_BATCH
    const static sched_param param{};
    const int rc = pthread_setschedparam(pthread_self(), SCHED_BATCH, &param);
    if (rc != 0) {
        LogPrintf("Failed to pthread_setschedparam: %s\n", strerror(rc));
    }
#endif
}

namespace util {
#ifdef WIN32
WinCmdLineArgs::WinCmdLineArgs()
{
    wchar_t** wargv = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::wstring_convert<std::codecvt_utf8_utf16<wchar_t>, wchar_t> utf8_cvt;
    argv = new char*[argc];
    args.resize(argc);
    for (int i = 0; i < argc; i++) {
        args[i] = utf8_cvt.to_bytes(wargv[i]);
        argv[i] = &*args[i].begin();
    }
    LocalFree(wargv);
}

WinCmdLineArgs::~WinCmdLineArgs()
{
    delete[] argv;
}

std::pair<int, char**> WinCmdLineArgs::get()
{
    return std::make_pair(argc, argv);
}
#endif

size_t g_low_memory_threshold = 10 * 1024 * 1024 /* 10 MB */;

bool SystemNeedsMemoryReleased()
{
    if (g_low_memory_threshold <= 0) {
        // Intentionally bypass other metrics when disabled entirely
        return false;
    }
#ifdef WIN32
    MEMORYSTATUSEX mem_status;
    mem_status.dwLength = sizeof(mem_status);
    if (GlobalMemoryStatusEx(&mem_status)) {
        if (mem_status.dwMemoryLoad >= 99 ||
            mem_status.ullAvailPhys < g_low_memory_threshold ||
            mem_status.ullAvailVirtual < g_low_memory_threshold) {
            LogPrintf("%s: YES: %s%% memory load; %s available physical memory; %s available virtual memory\n", __func__, int(mem_status.dwMemoryLoad), size_t(mem_status.ullAvailPhys), size_t(mem_status.ullAvailVirtual));
            return true;
        }
    }
#endif
#ifdef HAVE_LINUX_SYSINFO
    struct sysinfo sys_info;
    if (!sysinfo(&sys_info)) {
        // Explicitly 64-bit in case of 32-bit userspace on 64-bit kernel
        const uint64_t free_ram = uint64_t(sys_info.freeram) * sys_info.mem_unit;
        const uint64_t buffer_ram = uint64_t(sys_info.bufferram) * sys_info.mem_unit;
        if (free_ram + buffer_ram < g_low_memory_threshold) {
            LogPrintf("%s: YES: %s free RAM + %s buffer RAM\n", __func__, free_ram, buffer_ram);
            return true;
        }
    }
#endif
    // NOTE: sysconf(_SC_AVPHYS_PAGES) doesn't account for caches on at least Linux, so not safe to use here
    return false;
}

} // namespace util
