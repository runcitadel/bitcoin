// Copyright (c) 2011-2020 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#if defined(HAVE_CONFIG_H)
#include <config/bitcoin-config.h>
#endif

#include <qt/rpcconsole.h>
#include <qt/forms/ui_debugwindow.h>

#include <chainparams.h>
#include <interfaces/node.h>
#include <netbase.h>
#include <qt/bantablemodel.h>
#include <qt/clientmodel.h>
#include <qt/optionsmodel.h>
#include <qt/pairingpage.h>
#include <qt/peertablesortproxy.h>
#include <qt/platformstyle.h>
#include <qt/walletmodel.h>
#include <rpc/client.h>
#include <rpc/server.h>
#include <util/strencodings.h>
#include <util/string.h>
#include <util/system.h>
#include <util/threadnames.h>

#include <univalue.h>

#ifdef ENABLE_WALLET
#ifdef USE_BDB
#include <wallet/bdb.h>
#endif
#include <wallet/db.h>
#include <wallet/wallet.h>
#endif

#include <QAbstractButton>
#include <QAbstractItemModel>
#include <QDateTime>
#include <QFont>
#include <QFontMetrics>
#include <QKeyEvent>
#include <QLatin1String>
#include <QLocale>
#include <QMenu>
#include <QMessageBox>
#include <QScreen>
#include <QScrollBar>
#include <QSettings>
#include <QShortcut>
#include <QString>
#include <QStringList>
#include <QStyledItemDelegate>
#include <QTime>
#include <QTimer>
#include <QVariant>

const int CONSOLE_HISTORY = 50;
const int INITIAL_TRAFFIC_GRAPH_MINS = 30;
const QSize FONT_RANGE(4, 40);
const char fontSizeSettingsKey[] = "consoleFontSize";

const struct {
    const char *url;
    const char *source;
} ICON_MAPPING[] = {
    {"cmd-request", ":/icons/tx_input"},
    {"cmd-reply", ":/icons/tx_output"},
    {"cmd-error", ":/icons/tx_output"},
    {"misc", ":/icons/tx_inout"},
    {nullptr, nullptr}
};

namespace {

// don't add private key handling cmd's to the history
const QStringList historyFilter = QStringList()
    << "importprivkey"
    << "importmulti"
    << "sethdseed"
    << "signmessagewithprivkey"
    << "signrawtransactionwithkey"
    << "sweepprivkeys"
    << "walletpassphrase"
    << "walletpassphrasechange"
    << "encryptwallet";

}

/* Object for executing console RPC commands in a separate thread.
*/
class RPCExecutor : public QObject
{
    Q_OBJECT
public:
    explicit RPCExecutor(interfaces::Node& node) : m_node(node) {}

public Q_SLOTS:
    void request(const QString &command, const WalletModel* wallet_model);

Q_SIGNALS:
    void reply(int category, const QString &command);

private:
    interfaces::Node& m_node;
};

/** Class for handling RPC timers
 * (used for e.g. re-locking the wallet after a timeout)
 */
class QtRPCTimerBase: public QObject, public RPCTimerBase
{
    Q_OBJECT
public:
    QtRPCTimerBase(std::function<void()>& _func, int64_t millis):
        func(_func)
    {
        timer.setSingleShot(true);
        connect(&timer, &QTimer::timeout, [this]{ func(); });
        timer.start(millis);
    }
    ~QtRPCTimerBase() {}
private:
    QTimer timer;
    std::function<void()> func;
};

class QtRPCTimerInterface: public RPCTimerInterface
{
public:
    ~QtRPCTimerInterface() {}
    const char *Name() override { return "Qt"; }
    RPCTimerBase* NewTimer(std::function<void()>& func, int64_t millis) override
    {
        return new QtRPCTimerBase(func, millis);
    }
};

class PeerIdViewDelegate : public QStyledItemDelegate
{
    Q_OBJECT
public:
    explicit PeerIdViewDelegate(QObject* parent = nullptr)
        : QStyledItemDelegate(parent) {}

    QString displayText(const QVariant& value, const QLocale& locale) const override
    {
        // Additional spaces should visually separate right-aligned content
        // from the next column to the right.
        return value.toString() + QLatin1String("   ");
    }
};

#include <qt/rpcconsole.moc>

/**
 * Split shell command line into a list of arguments and optionally execute the command(s).
 * Aims to emulate \c bash and friends.
 *
 * - Command nesting is possible with parenthesis; for example: validateaddress(getnewaddress())
 * - Arguments are delimited with whitespace or comma
 * - Extra whitespace at the beginning and end and between arguments will be ignored
 * - Text can be "double" or 'single' quoted
 * - The backslash \c \ is used as escape character
 *   - Outside quotes, any character can be escaped
 *   - Within double quotes, only escape \c " and backslashes before a \c " or another backslash
 *   - Within single quotes, no escaping is possible and no special interpretation takes place
 *
 * @param[in]    node    optional node to execute command on
 * @param[out]   strResult   stringified result from the executed command(chain)
 * @param[in]    strCommand  Command line to split
 * @param[in]    fExecute    set true if you want the command to be executed
 * @param[out]   pstrFilteredOut  Command line, filtered to remove any sensitive data
 */

bool RPCConsole::RPCParseCommandLine(interfaces::Node* node, std::string &strResult, const std::string &strCommand, const bool fExecute, std::string * const pstrFilteredOut, const WalletModel* wallet_model)
{
    std::vector< std::vector<std::string> > stack;
    stack.push_back(std::vector<std::string>());

    enum CmdParseState
    {
        STATE_EATING_SPACES,
        STATE_EATING_SPACES_IN_ARG,
        STATE_EATING_SPACES_IN_BRACKETS,
        STATE_ARGUMENT,
        STATE_SINGLEQUOTED,
        STATE_DOUBLEQUOTED,
        STATE_ESCAPE_OUTER,
        STATE_ESCAPE_DOUBLEQUOTED,
        STATE_COMMAND_EXECUTED,
        STATE_COMMAND_EXECUTED_INNER
    } state = STATE_EATING_SPACES;
    std::string curarg;
    UniValue lastResult;
    unsigned nDepthInsideSensitive = 0;
    size_t filter_begin_pos = 0, chpos;
    std::vector<std::pair<size_t, size_t>> filter_ranges;

    auto add_to_current_stack = [&](const std::string& strArg) {
        if (stack.back().empty() && (!nDepthInsideSensitive) && historyFilter.contains(QString::fromStdString(strArg), Qt::CaseInsensitive)) {
            nDepthInsideSensitive = 1;
            filter_begin_pos = chpos;
        }
        // Make sure stack is not empty before adding something
        if (stack.empty()) {
            stack.push_back(std::vector<std::string>());
        }
        stack.back().push_back(strArg);
    };

    auto close_out_params = [&]() {
        if (nDepthInsideSensitive) {
            if (!--nDepthInsideSensitive) {
                assert(filter_begin_pos);
                filter_ranges.push_back(std::make_pair(filter_begin_pos, chpos));
                filter_begin_pos = 0;
            }
        }
        stack.pop_back();
    };

    std::string strCommandTerminated = strCommand;
    if (strCommandTerminated.back() != '\n')
        strCommandTerminated += "\n";
    for (chpos = 0; chpos < strCommandTerminated.size(); ++chpos)
    {
        char ch = strCommandTerminated[chpos];
        switch(state)
        {
            case STATE_COMMAND_EXECUTED_INNER:
            case STATE_COMMAND_EXECUTED:
            {
                bool breakParsing = true;
                switch(ch)
                {
                    case '[': curarg.clear(); state = STATE_COMMAND_EXECUTED_INNER; break;
                    default:
                        if (state == STATE_COMMAND_EXECUTED_INNER)
                        {
                            if (ch != ']')
                            {
                                // append char to the current argument (which is also used for the query command)
                                curarg += ch;
                                break;
                            }
                            if (curarg.size() && fExecute)
                            {
                                // if we have a value query, query arrays with index and objects with a string key
                                UniValue subelement;
                                if (lastResult.isArray())
                                {
                                    for(char argch: curarg)
                                        if (!IsDigit(argch))
                                            throw std::runtime_error("Invalid result query");
                                    subelement = lastResult[atoi(curarg.c_str())];
                                }
                                else if (lastResult.isObject())
                                    subelement = find_value(lastResult, curarg);
                                else
                                    throw std::runtime_error("Invalid result query"); //no array or object: abort
                                lastResult = subelement;
                            }

                            state = STATE_COMMAND_EXECUTED;
                            break;
                        }
                        // don't break parsing when the char is required for the next argument
                        breakParsing = false;

                        // pop the stack and return the result to the current command arguments
                        close_out_params();

                        // don't stringify the json in case of a string to avoid doublequotes
                        if (lastResult.isStr())
                            curarg = lastResult.get_str();
                        else
                            curarg = lastResult.write(2);

                        // if we have a non empty result, use it as stack argument otherwise as general result
                        if (curarg.size())
                        {
                            if (stack.size())
                                add_to_current_stack(curarg);
                            else
                                strResult = curarg;
                        }
                        curarg.clear();
                        // assume eating space state
                        state = STATE_EATING_SPACES;
                }
                if (breakParsing)
                    break;
                [[fallthrough]];
            }
            case STATE_ARGUMENT: // In or after argument
            case STATE_EATING_SPACES_IN_ARG:
            case STATE_EATING_SPACES_IN_BRACKETS:
            case STATE_EATING_SPACES: // Handle runs of whitespace
                switch(ch)
            {
                case '"': state = STATE_DOUBLEQUOTED; break;
                case '\'': state = STATE_SINGLEQUOTED; break;
                case '\\': state = STATE_ESCAPE_OUTER; break;
                case '(': case ')': case '\n':
                    if (state == STATE_EATING_SPACES_IN_ARG)
                        throw std::runtime_error("Invalid Syntax");
                    if (state == STATE_ARGUMENT)
                    {
                        if (ch == '(' && stack.size() && stack.back().size() > 0)
                        {
                            if (nDepthInsideSensitive) {
                                ++nDepthInsideSensitive;
                            }
                            stack.push_back(std::vector<std::string>());
                        }

                        // don't allow commands after executed commands on baselevel
                        if (!stack.size())
                            throw std::runtime_error("Invalid Syntax");

                        add_to_current_stack(curarg);
                        curarg.clear();
                        state = STATE_EATING_SPACES_IN_BRACKETS;
                    }
                    if ((ch == ')' || ch == '\n') && stack.size() > 0)
                    {
                        if (fExecute) {
                            // Convert argument list to JSON objects in method-dependent way,
                            // and pass it along with the method name to the dispatcher.
                            UniValue params = RPCConvertValues(stack.back()[0], std::vector<std::string>(stack.back().begin() + 1, stack.back().end()));
                            std::string method = stack.back()[0];
                            std::string uri;
#ifdef ENABLE_WALLET
                            if (wallet_model) {
                                QByteArray encodedName = QUrl::toPercentEncoding(wallet_model->getWalletName());
                                uri = "/wallet/"+std::string(encodedName.constData(), encodedName.length());
                            }
#endif
                            assert(node);
                            lastResult = node->executeRpc(method, params, uri);
                        }

                        state = STATE_COMMAND_EXECUTED;
                        curarg.clear();
                    }
                    break;
                case ' ': case ',': case '\t':
                    if(state == STATE_EATING_SPACES_IN_ARG && curarg.empty() && ch == ',')
                        throw std::runtime_error("Invalid Syntax");

                    else if(state == STATE_ARGUMENT) // Space ends argument
                    {
                        add_to_current_stack(curarg);
                        curarg.clear();
                    }
                    if ((state == STATE_EATING_SPACES_IN_BRACKETS || state == STATE_ARGUMENT) && ch == ',')
                    {
                        state = STATE_EATING_SPACES_IN_ARG;
                        break;
                    }
                    state = STATE_EATING_SPACES;
                    break;
                default: curarg += ch; state = STATE_ARGUMENT;
            }
                break;
            case STATE_SINGLEQUOTED: // Single-quoted string
                switch(ch)
            {
                case '\'': state = STATE_ARGUMENT; break;
                default: curarg += ch;
            }
                break;
            case STATE_DOUBLEQUOTED: // Double-quoted string
                switch(ch)
            {
                case '"': state = STATE_ARGUMENT; break;
                case '\\': state = STATE_ESCAPE_DOUBLEQUOTED; break;
                default: curarg += ch;
            }
                break;
            case STATE_ESCAPE_OUTER: // '\' outside quotes
                curarg += ch; state = STATE_ARGUMENT;
                break;
            case STATE_ESCAPE_DOUBLEQUOTED: // '\' in double-quoted text
                if(ch != '"' && ch != '\\') curarg += '\\'; // keep '\' for everything but the quote and '\' itself
                curarg += ch; state = STATE_DOUBLEQUOTED;
                break;
        }
    }
    if (pstrFilteredOut) {
        if (STATE_COMMAND_EXECUTED == state) {
            assert(!stack.empty());
            close_out_params();
        }
        *pstrFilteredOut = strCommand;
        for (auto i = filter_ranges.rbegin(); i != filter_ranges.rend(); ++i) {
            pstrFilteredOut->replace(i->first, i->second - i->first, "(…)");
        }
    }
    switch(state) // final state
    {
        case STATE_COMMAND_EXECUTED:
            if (lastResult.isStr())
                strResult = lastResult.get_str();
            else
                strResult = lastResult.write(2);
            [[fallthrough]];
        case STATE_ARGUMENT:
        case STATE_EATING_SPACES:
            return true;
        default: // ERROR to end in one of the other states
            return false;
    }
}

void RPCExecutor::request(const QString &command, const WalletModel* wallet_model)
{
    try
    {
        std::string result;
        std::string executableCommand = command.toStdString() + "\n";

        // Catch the console-only-help command before RPC call is executed and reply with help text as-if a RPC reply.
        if(executableCommand == "help-console\n") {
            Q_EMIT reply(RPCConsole::CMD_REPLY, QString(("\n"
                "This console accepts RPC commands using the standard syntax.\n"
                "   example:    getblockhash 0\n\n"

                "This console can also accept RPC commands using the parenthesized syntax.\n"
                "   example:    getblockhash(0)\n\n"

                "Commands may be nested when specified with the parenthesized syntax.\n"
                "   example:    getblock(getblockhash(0) 1)\n\n"

                "A space or a comma can be used to delimit arguments for either syntax.\n"
                "   example:    getblockhash 0\n"
                "               getblockhash,0\n\n"

                "Named results can be queried with a non-quoted key string in brackets using the parenthesized syntax.\n"
                "   example:    getblock(getblockhash(0) 1)[tx]\n\n"

                "Results without keys can be queried with an integer in brackets using the parenthesized syntax.\n"
                "   example:    getblock(getblockhash(0),1)[tx][0]\n\n")));
            return;
        }
        if (!RPCConsole::RPCExecuteCommandLine(m_node, result, executableCommand, nullptr, wallet_model)) {
            Q_EMIT reply(RPCConsole::CMD_ERROR, QString("Parse error: unbalanced ' or \""));
            return;
        }

        Q_EMIT reply(RPCConsole::CMD_REPLY, QString::fromStdString(result));
    }
    catch (UniValue& objError)
    {
        try // Nice formatting for standard-format error
        {
            int code = find_value(objError, "code").get_int();
            std::string message = find_value(objError, "message").get_str();
            Q_EMIT reply(RPCConsole::CMD_ERROR, QString::fromStdString(message) + " (code " + QString::number(code) + ")");
        }
        catch (const std::runtime_error&) // raised when converting to invalid type, i.e. missing code or message
        {   // Show raw JSON object
            Q_EMIT reply(RPCConsole::CMD_ERROR, QString::fromStdString(objError.write()));
        }
    }
    catch (const std::exception& e)
    {
        Q_EMIT reply(RPCConsole::CMD_ERROR, QString("Error: ") + QString::fromStdString(e.what()));
    }
}

RPCConsole::RPCConsole(interfaces::Node& node, const PlatformStyle *_platformStyle, QWidget *parent) :
    QWidget(parent),
    m_node(node),
    ui(new Ui::RPCConsole),
    platformStyle(_platformStyle)
{
    ui->setupUi(this);

    // Default tabs are identified by their UI index
    for (int i = ui->tabWidget->count(); i--; ) {
        m_tabs[TabTypes(i)] = ui->tabWidget->widget(i);
    }

    QSettings settings;
#ifdef ENABLE_WALLET
    if (WalletModel::isWalletEnabled()) {
        // RPCConsole widget is a window.
        if (!restoreGeometry(settings.value("RPCConsoleWindowGeometry").toByteArray())) {
            // Restore failed (perhaps missing setting), center the window
            move(QGuiApplication::primaryScreen()->availableGeometry().center() - frameGeometry().center());
        }
        ui->splitter->restoreState(settings.value("RPCConsoleWindowPeersTabSplitterSizes_Knots21").toByteArray());
    } else
#endif // ENABLE_WALLET
    {
        // RPCConsole is a child widget.
        ui->splitter->restoreState(settings.value("RPCConsoleWidgetPeersTabSplitterSizes_Knots21").toByteArray());
    }

    m_peer_widget_header_state = settings.value("PeersTabPeerHeaderState_Knots21").toByteArray();
    m_banlist_widget_header_state = settings.value("PeersTabBanlistHeaderState").toByteArray();
    m_alternating_row_colors = settings.value("PeersTabAlternatingRowColors").toBool();

    constexpr QChar nonbreaking_hyphen(8209);
    const std::vector<QString> CONNECTION_TYPE_DOC{
        tr("Inbound: initiated by peer"),
        tr("Outbound Full Relay: default"),
        tr("Outbound Block Relay: does not relay transactions or addresses"),
        tr("Outbound Manual: added using RPC %1 or %2/%3 configuration options")
            .arg("addnode")
            .arg(QString(nonbreaking_hyphen) + "addnode")
            .arg(QString(nonbreaking_hyphen) + "connect"),
        tr("Outbound Feeler: short-lived, for testing addresses"),
        tr("Outbound Address Fetch: short-lived, for soliciting addresses")};
    const QString list{"<ul><li>" + Join(CONNECTION_TYPE_DOC, QString("</li><li>")) + "</li></ul>"};
    ui->peerConnectionTypeLabel->setToolTip(ui->peerConnectionTypeLabel->toolTip().arg(list));
    const QString hb_list{"<ul><li>\""
        + ts.to + "\" – " + tr("we selected the peer for high bandwidth relay") + "</li><li>\""
        + ts.from + "\" – " + tr("the peer selected us for high bandwidth relay") + "</li><li>\""
        + ts.no + "\" – " + tr("no high bandwidth relay selected") + "</li></ul>"};
    ui->peerHighBandwidthLabel->setToolTip(ui->peerHighBandwidthLabel->toolTip().arg(hb_list));
    ui->dataDir->setToolTip(ui->dataDir->toolTip().arg(QString(nonbreaking_hyphen) + "datadir"));
    ui->blocksDir->setToolTip(ui->blocksDir->toolTip().arg(QString(nonbreaking_hyphen) + "blocksdir"));
    ui->openDebugLogfileButton->setToolTip(ui->openDebugLogfileButton->toolTip().arg(PACKAGE_NAME));

    if (platformStyle->getImagesOnButtons()) {
        ui->openDebugLogfileButton->setIcon(platformStyle->SingleColorIcon(":/icons/export"));
    }
    ui->clearButton->setIcon(platformStyle->SingleColorIcon(":/icons/remove"));

    ui->fontBiggerButton->setIcon(platformStyle->SingleColorIcon(":/icons/fontbigger"));
    //: Main shortcut to increase the RPC console font size.
    ui->fontBiggerButton->setShortcut(tr("Ctrl++"));
    //: Secondary shortcut to increase the RPC console font size.
    GUIUtil::AddButtonShortcut(ui->fontBiggerButton, tr("Ctrl+="));

    ui->fontSmallerButton->setIcon(platformStyle->SingleColorIcon(":/icons/fontsmaller"));
    //: Main shortcut to decrease the RPC console font size.
    ui->fontSmallerButton->setShortcut(tr("Ctrl+-"));
    //: Secondary shortcut to decrease the RPC console font size.
    GUIUtil::AddButtonShortcut(ui->fontSmallerButton, tr("Ctrl+_"));

    ui->promptIcon->setIcon(platformStyle->SingleColorIcon(QStringLiteral(":/icons/prompticon")));

    // Install event filter for up and down arrow
    ui->lineEdit->installEventFilter(this);
    ui->lineEdit->setMaxLength(16 * 1024 * 1024);
    ui->messagesWidget->installEventFilter(this);

    connect(ui->clearButton, &QAbstractButton::clicked, [this] { clear(); });
    connect(ui->fontBiggerButton, &QAbstractButton::clicked, this, &RPCConsole::fontBigger);
    connect(ui->fontSmallerButton, &QAbstractButton::clicked, this, &RPCConsole::fontSmaller);
    connect(ui->btnClearTrafficGraph, &QPushButton::clicked, ui->trafficGraph, &TrafficGraphWidget::clear);

    // disable the wallet selector by default
    ui->WalletSelector->setVisible(false);
    ui->WalletSelectorLabel->setVisible(false);

    // Register RPC timer interface
    rpcTimerInterface = new QtRPCTimerInterface();
    // avoid accidentally overwriting an existing, non QTThread
    // based timer interface
    m_node.rpcSetTimerInterfaceIfUnset(rpcTimerInterface);

    setTrafficGraphRange(INITIAL_TRAFFIC_GRAPH_MINS);
    updateDetailWidget();

    consoleFontSize = settings.value(fontSizeSettingsKey, QFont().pointSize()).toInt();
    clear();

    // load history
    QMap<size_t, QString> rewrite_replace;
    int size = settings.beginReadArray("nRPCConsoleWindowHistory");
    history.clear();
    for (int i = 0; i < size; ++i) {
        settings.setArrayIndex(i);
        QString cmd = settings.value("cmd").toString();
        QString filtered_cmd;
        {
            std::string strFilteredCmd, dummy;
            if (RPCParseCommandLine(nullptr, dummy, cmd.toStdString(), false, &strFilteredCmd)) {
                filtered_cmd = QString::fromStdString(strFilteredCmd);
            } else {
                // Failed to parse command, so we cannot even filter it for the history
                filtered_cmd = cmd;
            }
        }
        if (cmd != filtered_cmd) {
            // Overwrite this line, and trigger an immediate rewrite of history to purge it
            cmd = QString(cmd.size(), 'x');
            rewrite_replace[history.size()] = filtered_cmd;
        }
        history.append(cmd);
    }
    historyPtr = history.size();
    settings.endArray();
    if (!rewrite_replace.empty()) {
        WriteCommandHistory();
        for (QMapIterator<size_t, QString> i(rewrite_replace); i.hasNext(); ) {
            i.next();
            history[i.key()] = i.value();
        }
        WriteCommandHistory();
    }

    GUIUtil::handleCloseWindowShortcut(this);
    QObject::connect(new QShortcut(QKeySequence(Qt::CTRL + Qt::Key_D), ui->tab_console), &QShortcut::activated, this, &QWidget::close);
}

void RPCConsole::WriteCommandHistory()
{
    // persist history
    QSettings settings;
    settings.beginWriteArray("nRPCConsoleWindowHistory");
    for (int i = 0; i < history.size(); ++i) {
        settings.setArrayIndex(i);
        settings.setValue("cmd", history.at(i));
    }
    settings.endArray();
}

RPCConsole::~RPCConsole()
{
    QSettings settings;
#ifdef ENABLE_WALLET
    if (WalletModel::isWalletEnabled()) {
        // RPCConsole widget is a window.
        settings.setValue("RPCConsoleWindowGeometry", saveGeometry());
        settings.setValue("RPCConsoleWindowPeersTabSplitterSizes_Knots21", ui->splitter->saveState());
    } else
#endif // ENABLE_WALLET
    {
        // RPCConsole is a child widget.
        settings.setValue("RPCConsoleWidgetPeersTabSplitterSizes_Knots21", ui->splitter->saveState());
    }

    settings.setValue("PeersTabPeerHeaderState_Knots21", m_peer_widget_header_state);
    settings.setValue("PeersTabBanlistHeaderState", m_banlist_widget_header_state);

    WriteCommandHistory();

    m_node.rpcUnsetTimerInterface(rpcTimerInterface);
    delete rpcTimerInterface;
    delete ui;
}

bool RPCConsole::eventFilter(QObject* obj, QEvent *event)
{
    if(event->type() == QEvent::KeyPress) // Special key handling
    {
        QKeyEvent *keyevt = static_cast<QKeyEvent*>(event);
        int key = keyevt->key();
        Qt::KeyboardModifiers mod = keyevt->modifiers();
        switch(key)
        {
        case Qt::Key_Up: if(obj == ui->lineEdit) { browseHistory(-1); return true; } break;
        case Qt::Key_Down: if(obj == ui->lineEdit) { browseHistory(1); return true; } break;
        case Qt::Key_PageUp: /* pass paging keys to messages widget */
        case Qt::Key_PageDown:
            if(obj == ui->lineEdit)
            {
                QApplication::postEvent(ui->messagesWidget, new QKeyEvent(*keyevt));
                return true;
            }
            break;
        case Qt::Key_Return:
        case Qt::Key_Enter:
            // forward these events to lineEdit
            if(obj == autoCompleter->popup()) {
                QApplication::postEvent(ui->lineEdit, new QKeyEvent(*keyevt));
                autoCompleter->popup()->hide();
                return true;
            }
            break;
        default:
            // Typing in messages widget brings focus to line edit, and redirects key there
            // Exclude most combinations and keys that emit no text, except paste shortcuts
            if(obj == ui->messagesWidget && (
                  (!mod && !keyevt->text().isEmpty() && key != Qt::Key_Tab) ||
                  ((mod & Qt::ControlModifier) && key == Qt::Key_V) ||
                  ((mod & Qt::ShiftModifier) && key == Qt::Key_Insert)))
            {
                ui->lineEdit->setFocus();
                QApplication::postEvent(ui->lineEdit, new QKeyEvent(*keyevt));
                return true;
            }
        }
    }
    return QWidget::eventFilter(obj, event);
}

void RPCConsole::setClientModel(ClientModel *model, int bestblock_height, int64_t bestblock_date, double verification_progress)
{
    clientModel = model;

    bool wallet_enabled{false};
#ifdef ENABLE_WALLET
    wallet_enabled = WalletModel::isWalletEnabled();
#endif // ENABLE_WALLET
    if (model && !wallet_enabled) {
        // Show warning, for example if this is a prerelease version
        connect(model, &ClientModel::alertsChanged, this, &RPCConsole::updateAlerts);
        updateAlerts(model->getStatusBarWarnings());
    }

    ui->trafficGraph->setClientModel(model);
    if (m_tab_pairing) m_tab_pairing->setClientModel(model);
    if (model && clientModel->getPeerTableModel() && clientModel->getBanTableModel()) {
        // Keep up to date with client
        setNumConnections(model->getNumConnections());
        connect(model, &ClientModel::numConnectionsChanged, this, &RPCConsole::setNumConnections);

        setNumBlocks(bestblock_height, QDateTime::fromTime_t(bestblock_date), verification_progress, false);
        connect(model, &ClientModel::numBlocksChanged, this, &RPCConsole::setNumBlocks);

        updateNetworkState();
        connect(model, &ClientModel::networkActiveChanged, this, &RPCConsole::setNetworkActive);

        interfaces::Node& node = clientModel->node();
        updateTrafficStats(node.getTotalBytesRecv(), node.getTotalBytesSent());
        connect(model, &ClientModel::bytesChanged, this, &RPCConsole::updateTrafficStats);

        connect(model, &ClientModel::mempoolSizeChanged, this, &RPCConsole::setMempoolSize);

        connect(model->getOptionsModel(), &OptionsModel::peersTabAlternatingRowColorsChanged, [this](bool alternating_row_colors) {
            ui->peerWidget->setAlternatingRowColors(alternating_row_colors);
            ui->banlistWidget->setAlternatingRowColors(alternating_row_colors);
        });

        // set up peer table
        clientModel->getPeerTableModel()->updatePalette();
        ui->peerWidget->setModel(model->peerTableSortProxy());
        ui->peerWidget->verticalHeader()->hide();
        ui->peerWidget->setSelectionBehavior(QAbstractItemView::SelectRows);
        ui->peerWidget->setSelectionMode(QAbstractItemView::ExtendedSelection);
        ui->peerWidget->setContextMenuPolicy(Qt::CustomContextMenu);

        if (!ui->peerWidget->horizontalHeader()->restoreState(m_peer_widget_header_state)) {
            const QFontMetrics fm = ui->peerWidget->fontMetrics();
            ui->peerWidget->setColumnWidth(PeerTableModel::NetNodeId, GUIUtil::TextWidth(fm, "99999"));
            ui->peerWidget->setColumnWidth(PeerTableModel::Direction, DIRECTION_COLUMN_WIDTH);
            ui->peerWidget->setColumnWidth(PeerTableModel::Address, ADDRESS_COLUMN_WIDTH);
            ui->peerWidget->setColumnWidth(PeerTableModel::ConnectionType, GUIUtil::TextWidth(fm, GUIUtil::ConnectionTypeToQString(ConnectionType::ADDR_FETCH /* TODO: Find the longest string? */, /* prepend_direction */ false)));
            const auto bytesize_width = GUIUtil::TextWidth(fm, GUIUtil::formatBytes(999'000'000'000) + "x");
            ui->peerWidget->setColumnWidth(PeerTableModel::Sent, bytesize_width);
            ui->peerWidget->setColumnWidth(PeerTableModel::Received, bytesize_width);
            ui->peerWidget->setColumnWidth(PeerTableModel::Subversion, SUBVERSION_COLUMN_WIDTH);
            ui->peerWidget->setColumnWidth(PeerTableModel::Ping, PING_COLUMN_WIDTH);
        }
        ui->peerWidget->horizontalHeader()->setStretchLastSection(true);
        ui->peerWidget->setItemDelegateForColumn(PeerTableModel::NetNodeId, new PeerIdViewDelegate(this));
        ui->peerWidget->setAlternatingRowColors(m_alternating_row_colors);

        // create peer table context menu
        peersTableContextMenu = new QMenu(this);
        //: Context menu action to copy the address of a peer
        peersTableContextMenu->addAction(tr("&Copy address"), [this] {
            GUIUtil::copyEntryData(ui->peerWidget, PeerTableModel::Address, Qt::DisplayRole);
        });
        peersTableContextMenu->addSeparator();
        peersTableContextMenu->addAction(tr("&Disconnect"), this, &RPCConsole::disconnectSelectedNode);
        peersTableContextMenu->addAction(ts.ban_for + " " + tr("1 &hour"), [this] { banSelectedNode(60 * 60); });
        peersTableContextMenu->addAction(ts.ban_for + " " + tr("1 d&ay"), [this] { banSelectedNode(60 * 60 * 24); });
        peersTableContextMenu->addAction(ts.ban_for + " " + tr("1 &week"), [this] { banSelectedNode(60 * 60 * 24 * 7); });
        peersTableContextMenu->addAction(ts.ban_for + " " + tr("1 &year"), [this] { banSelectedNode(60 * 60 * 24 * 365); });
        connect(ui->peerWidget, &QTableView::customContextMenuRequested, this, &RPCConsole::showPeersTableContextMenu);

        // peer table signal handling - update peer details when selecting new node
        connect(ui->peerWidget->selectionModel(), &QItemSelectionModel::selectionChanged, this, &RPCConsole::updateDetailWidget);
        connect(model->getPeerTableModel(), &QAbstractItemModel::dataChanged, [this] { updateDetailWidget(); });

        // set up ban table
        ui->banlistWidget->setModel(model->getBanTableModel());
        ui->banlistWidget->verticalHeader()->hide();
        ui->banlistWidget->setSelectionBehavior(QAbstractItemView::SelectRows);
        ui->banlistWidget->setSelectionMode(QAbstractItemView::SingleSelection);
        ui->banlistWidget->setContextMenuPolicy(Qt::CustomContextMenu);

        if (!ui->banlistWidget->horizontalHeader()->restoreState(m_banlist_widget_header_state)) {
            ui->banlistWidget->setColumnWidth(BanTableModel::Address, BANSUBNET_COLUMN_WIDTH);
            ui->banlistWidget->setColumnWidth(BanTableModel::Bantime, BANTIME_COLUMN_WIDTH);
        }
        ui->banlistWidget->horizontalHeader()->setStretchLastSection(true);
        ui->banlistWidget->setAlternatingRowColors(m_alternating_row_colors);

        // create ban table context menu
        banTableContextMenu = new QMenu(this);
        /*: Context menu action to copy the IP/Netmask of a banned peer.
            IP/Netmask is the combination of a peer's IP address and its Netmask.
            For IP address see: https://en.wikipedia.org/wiki/IP_address */
        banTableContextMenu->addAction(tr("&Copy IP/Netmask"), [this] {
            GUIUtil::copyEntryData(ui->banlistWidget, BanTableModel::Address, Qt::DisplayRole);
        });
        banTableContextMenu->addSeparator();
        banTableContextMenu->addAction(tr("&Unban"), this, &RPCConsole::unbanSelectedNode);
        connect(ui->banlistWidget, &QTableView::customContextMenuRequested, this, &RPCConsole::showBanTableContextMenu);

        // ban table signal handling - clear peer details when clicking a peer in the ban table
        connect(ui->banlistWidget, &QTableView::clicked, this, &RPCConsole::clearSelectedNode);
        // ban table signal handling - ensure ban table is shown or hidden (if empty)
        connect(model->getBanTableModel(), &BanTableModel::layoutChanged, this, &RPCConsole::showOrHideBanTableIfRequired);
        showOrHideBanTableIfRequired();

        // Provide initial values
        ui->clientVersion->setText(model->formatFullVersion());
        ui->clientUserAgent->setText(model->formatSubVersion());
        ui->dataDir->setText(model->dataDir());
        ui->blocksDir->setText(model->blocksDir());
        ui->startupTime->setText(model->formatClientStartupTime());
        ui->networkName->setText(QString::fromStdString(Params().NetworkIDString()));

        //Setup autocomplete and attach it
        QStringList wordList;
        std::vector<std::string> commandList = m_node.listRpcCommands();
        for (size_t i = 0; i < commandList.size(); ++i)
        {
            wordList << commandList[i].c_str();
            wordList << ("help " + commandList[i]).c_str();
        }

        wordList << "help-console";
        wordList.sort();
        autoCompleter = new QCompleter(wordList, this);
        autoCompleter->setModelSorting(QCompleter::CaseSensitivelySortedModel);
        // ui->lineEdit is initially disabled because running commands is only
        // possible from now on.
        ui->lineEdit->setEnabled(true);
        ui->lineEdit->setCompleter(autoCompleter);
        autoCompleter->popup()->installEventFilter(this);
        // Start thread to execute RPC commands.
        startExecutor();
    }
    if (!model) {
        // Client model is being set to 0, this means shutdown() is about to be called.
        thread.quit();
        thread.wait();
    }
}

void RPCConsole::addPairingTab()
{
    assert(!m_tab_pairing);
    m_tab_pairing = new PairingPage(this);
    ui->tabWidget->insertTab(1, m_tab_pairing, tr("&Pairing"));
    m_tabs[TabTypes::PAIRING] = m_tab_pairing;
    if (clientModel) m_tab_pairing->setClientModel(clientModel);
}

#ifdef ENABLE_WALLET
void RPCConsole::addWallet(WalletModel * const walletModel)
{
    // use name for text and wallet model for internal data object (to allow to move to a wallet id later)
    ui->WalletSelector->addItem(walletModel->getDisplayName(), QVariant::fromValue(walletModel));
    if (ui->WalletSelector->count() == 2 && !isVisible()) {
        // First wallet added, set to default so long as the window isn't presently visible (and potentially in use)
        ui->WalletSelector->setCurrentIndex(1);
    }
    if (ui->WalletSelector->count() > 2) {
        ui->WalletSelector->setVisible(true);
        ui->WalletSelectorLabel->setVisible(true);
    }
}

void RPCConsole::removeWallet(WalletModel * const walletModel)
{
    ui->WalletSelector->removeItem(ui->WalletSelector->findData(QVariant::fromValue(walletModel)));
    if (ui->WalletSelector->count() == 2) {
        ui->WalletSelector->setVisible(false);
        ui->WalletSelectorLabel->setVisible(false);
    }
}
#endif

static QString categoryClass(int category)
{
    switch(category)
    {
    case RPCConsole::CMD_REQUEST:  return "cmd-request"; break;
    case RPCConsole::CMD_REPLY:    return "cmd-reply"; break;
    case RPCConsole::CMD_ERROR:    return "cmd-error"; break;
    default:                       return "misc";
    }
}

void RPCConsole::fontBigger()
{
    setFontSize(consoleFontSize+1);
}

void RPCConsole::fontSmaller()
{
    setFontSize(consoleFontSize-1);
}

void RPCConsole::setFontSize(int newSize)
{
    QSettings settings;

    //don't allow an insane font size
    if (newSize < FONT_RANGE.width() || newSize > FONT_RANGE.height())
        return;

    // temp. store the console content
    QString str = ui->messagesWidget->toHtml();

    // replace font tags size in current content
    str.replace(QString("font-size:%1pt").arg(consoleFontSize), QString("font-size:%1pt").arg(newSize));

    // store the new font size
    consoleFontSize = newSize;
    settings.setValue(fontSizeSettingsKey, consoleFontSize);

    // clear console (reset icon sizes, default stylesheet) and re-add the content
    float oldPosFactor = 1.0 / ui->messagesWidget->verticalScrollBar()->maximum() * ui->messagesWidget->verticalScrollBar()->value();
    clear(/* keep_prompt */ true);
    ui->messagesWidget->setHtml(str);
    ui->messagesWidget->verticalScrollBar()->setValue(oldPosFactor * ui->messagesWidget->verticalScrollBar()->maximum());
}

void RPCConsole::clear(bool keep_prompt)
{
    ui->messagesWidget->clear();
    if (!keep_prompt) ui->lineEdit->clear();
    ui->lineEdit->setFocus();

    // Add smoothly scaled icon images.
    // (when using width/height on an img, Qt uses nearest instead of linear interpolation)
    for(int i=0; ICON_MAPPING[i].url; ++i)
    {
        ui->messagesWidget->document()->addResource(
                    QTextDocument::ImageResource,
                    QUrl(ICON_MAPPING[i].url),
                    platformStyle->SingleColorImage(ICON_MAPPING[i].source).scaled(QSize(consoleFontSize*2, consoleFontSize*2), Qt::IgnoreAspectRatio, Qt::SmoothTransformation));
    }

    // Set default style sheet
    QFontInfo fixedFontInfo(GUIUtil::fixedPitchFont());
    ui->messagesWidget->document()->setDefaultStyleSheet(
        QString(
                "table { }"
                "td.time { color: #808080; font-size: %2; padding-top: 3px; } "
                "td.message { font-family: %1; font-size: %2; white-space:pre-wrap; } "
                "td.cmd-request { color: #006060; } "
                "td.cmd-error { color: red; } "
                ".secwarning { color: red; }"
                "b { color: #006060; } "
            ).arg(fixedFontInfo.family(), QString("%1pt").arg(consoleFontSize))
        );

    static const QString welcome_message =
        /*: RPC console welcome message.
            Placeholders %7 and %8 are style tags for the warning content, and
            they are not space separated from the rest of the text intentionally. */
        tr("Welcome to the %1 RPC console.\n"
           "Use up and down arrows to navigate history, and %2 to clear screen.\n"
           "Use %3 and %4 to increase or decrease the font size.\n"
           "Type %5 for an overview of available commands.\n"
           "For more information on using this console, type %6.\n"
           "\n"
           "%7WARNING: Scammers have been active, telling users to type"
           " commands here, stealing their wallet contents. Do not use this console"
           " without fully understanding the ramifications of a command.%8")
            .arg(PACKAGE_NAME,
                 "<b>" + ui->clearButton->shortcut().toString(QKeySequence::NativeText) + "</b>",
                 "<b>" + ui->fontBiggerButton->shortcut().toString(QKeySequence::NativeText) + "</b>",
                 "<b>" + ui->fontSmallerButton->shortcut().toString(QKeySequence::NativeText) + "</b>",
                 "<b>help</b>",
                 "<b>help-console</b>",
                 "<span class=\"secwarning\">",
                 "<span>");

    message(CMD_REPLY, welcome_message, true);
}

void RPCConsole::keyPressEvent(QKeyEvent *event)
{
    if(windowType() != Qt::Widget && event->key() == Qt::Key_Escape)
    {
        close();
    }
}

void RPCConsole::changeEvent(QEvent* e)
{
    if (e->type() == QEvent::PaletteChange) {
        ui->clearButton->setIcon(platformStyle->SingleColorIcon(QStringLiteral(":/icons/remove")));
        ui->fontBiggerButton->setIcon(platformStyle->SingleColorIcon(QStringLiteral(":/icons/fontbigger")));
        ui->fontSmallerButton->setIcon(platformStyle->SingleColorIcon(QStringLiteral(":/icons/fontsmaller")));
        ui->promptIcon->setIcon(platformStyle->SingleColorIcon(QStringLiteral(":/icons/prompticon")));

        for (int i = 0; ICON_MAPPING[i].url; ++i) {
            ui->messagesWidget->document()->addResource(
                QTextDocument::ImageResource,
                QUrl(ICON_MAPPING[i].url),
                platformStyle->SingleColorImage(ICON_MAPPING[i].source).scaled(QSize(consoleFontSize * 2, consoleFontSize * 2), Qt::IgnoreAspectRatio, Qt::SmoothTransformation));
        }

        if (clientModel && clientModel->getPeerTableModel()) {
            clientModel->getPeerTableModel()->updatePalette();
        }
    }

    QWidget::changeEvent(e);
}

void RPCConsole::message(int category, const QString &message, bool html)
{
    QTime time = QTime::currentTime();
    QString timeString = time.toString();
    QString out;
    out += "<table><tr><td class=\"time\" width=\"65\">" + timeString + "</td>";
    out += "<td class=\"icon\" width=\"32\"><img src=\"" + categoryClass(category) + "\"></td>";
    out += "<td class=\"message " + categoryClass(category) + "\" valign=\"middle\">";
    if(html)
        out += message;
    else
        out += GUIUtil::HtmlEscape(message, false);
    out += "</td></tr></table>";
    ui->messagesWidget->append(out);
}

void RPCConsole::updateNetworkState()
{
    QString connections = QString::number(clientModel->getNumConnections()) + " (";
    connections += tr("In:") + " " + QString::number(clientModel->getNumConnections(CONNECTIONS_IN)) + " / ";
    connections += tr("Out:") + " " + QString::number(clientModel->getNumConnections(CONNECTIONS_OUT)) + ")";

    if(!clientModel->node().getNetworkActive()) {
        connections += " (" + tr("Network activity disabled") + ")";
    }

    ui->numberOfConnections->setText(connections);
}

void RPCConsole::setNumConnections(int count)
{
    if (!clientModel)
        return;

    updateNetworkState();
}

void RPCConsole::setNetworkActive(bool networkActive)
{
    updateNetworkState();
}

void RPCConsole::setNumBlocks(int count, const QDateTime& blockDate, double nVerificationProgress, bool headers)
{
    if (!headers) {
        ui->numberOfBlocks->setText(QString::number(count));
        ui->lastBlockTime->setText(blockDate.toString());
    }
}

void RPCConsole::setMempoolSize(long numberOfTxs, size_t dynUsage)
{
    ui->mempoolNumberTxs->setText(QString::number(numberOfTxs));

    if (dynUsage < 1000000)
        ui->mempoolSize->setText(QString::number(dynUsage/1000.0, 'f', 2) + " KB");
    else
        ui->mempoolSize->setText(QString::number(dynUsage/1000000.0, 'f', 2) + " MB");
}

void RPCConsole::on_lineEdit_returnPressed()
{
    QString cmd = ui->lineEdit->text().trimmed();

    if (cmd.isEmpty()) {
        return;
    }

    std::string strFilteredCmd;
    try {
        std::string dummy;
        if (!RPCParseCommandLine(nullptr, dummy, cmd.toStdString(), false, &strFilteredCmd)) {
            // Failed to parse command, so we cannot even filter it for the history
            throw std::runtime_error("Invalid command line");
        }
    } catch (const std::exception& e) {
        QMessageBox::critical(this, "Error", QString("Error: ") + QString::fromStdString(e.what()));
        return;
    }

    // A special case allows to request shutdown even a long-running command is executed.
    if (cmd == QLatin1String("stop")) {
        std::string dummy;
        RPCExecuteCommandLine(m_node, dummy, cmd.toStdString());
        return;
    }

    if (m_is_executing) {
        return;
    }

    ui->lineEdit->clear();

#ifdef ENABLE_WALLET
    WalletModel* wallet_model = ui->WalletSelector->currentData().value<WalletModel*>();

    if (m_last_wallet_model != wallet_model) {
        if (wallet_model) {
            message(CMD_REQUEST, tr("Executing command using \"%1\" wallet").arg(wallet_model->getWalletName()));
        } else {
            message(CMD_REQUEST, tr("Executing command without any wallet"));
        }
        m_last_wallet_model = wallet_model;
    }
#endif // ENABLE_WALLET

    message(CMD_REQUEST, QString::fromStdString(strFilteredCmd));
    //: A console message indicating an entered command is currently being executed.
    message(CMD_REPLY, tr("Executing…"));
    m_is_executing = true;
    Q_EMIT cmdRequest(cmd, m_last_wallet_model);

    cmd = QString::fromStdString(strFilteredCmd);

    // Remove command, if already in history
    history.removeOne(cmd);
    // Append command to history
    history.append(cmd);
    // Enforce maximum history size
    while (history.size() > CONSOLE_HISTORY) {
        history.removeFirst();
    }
    // Set pointer to end of history
    historyPtr = history.size();

    // Scroll console view to end
    scrollToEnd();
}

void RPCConsole::browseHistory(int offset)
{
    // store current text when start browsing through the history
    if (historyPtr == history.size()) {
        cmdBeforeBrowsing = ui->lineEdit->text();
    }

    historyPtr += offset;
    if(historyPtr < 0)
        historyPtr = 0;
    if(historyPtr > history.size())
        historyPtr = history.size();
    QString cmd;
    if(historyPtr < history.size())
        cmd = history.at(historyPtr);
    else if (!cmdBeforeBrowsing.isNull()) {
        cmd = cmdBeforeBrowsing;
    }
    ui->lineEdit->setText(cmd);
}

void RPCConsole::startExecutor()
{
    RPCExecutor *executor = new RPCExecutor(m_node);
    executor->moveToThread(&thread);

    // Replies from executor object must go to this object
    connect(executor, &RPCExecutor::reply, this, [this](int category, const QString& command) {
        // Remove "Executing…" message.
        ui->messagesWidget->undo();
        message(category, command);
        scrollToEnd();
        m_is_executing = false;
    });

    // Requests from this object must go to executor
    connect(this, &RPCConsole::cmdRequest, executor, &RPCExecutor::request);

    // Make sure executor object is deleted in its own thread
    connect(&thread, &QThread::finished, executor, &RPCExecutor::deleteLater);

    // Default implementation of QThread::run() simply spins up an event loop in the thread,
    // which is what we want.
    thread.start();
    QTimer::singleShot(0, executor, []() {
        util::ThreadRename("qt-rpcconsole");
    });
}

void RPCConsole::on_tabWidget_currentChanged(int index)
{
    if (ui->tabWidget->widget(index) == ui->tab_console) {
        ui->lineEdit->setFocus();
    }
}

void RPCConsole::on_openDebugLogfileButton_clicked()
{
    GUIUtil::openDebugLogfile();
}

void RPCConsole::scrollToEnd()
{
    QScrollBar *scrollbar = ui->messagesWidget->verticalScrollBar();
    scrollbar->setValue(scrollbar->maximum());
}

void RPCConsole::on_sldGraphRange_valueChanged(int value)
{
    const int multiplier = 5; // each position on the slider represents 5 min
    int mins = value * multiplier;
    setTrafficGraphRange(mins);
}

void RPCConsole::setTrafficGraphRange(int mins)
{
    ui->trafficGraph->setGraphRangeMins(mins);
    ui->lblGraphRange->setText(GUIUtil::formatDurationStr(mins * 60));
}

void RPCConsole::updateTrafficStats(quint64 totalBytesIn, quint64 totalBytesOut)
{
    ui->lblBytesIn->setText(GUIUtil::formatBytes(totalBytesIn));
    ui->lblBytesOut->setText(GUIUtil::formatBytes(totalBytesOut));
}

void RPCConsole::updateDetailWidget()
{
    const QList<QModelIndex> selected_peers = GUIUtil::getEntryData(ui->peerWidget, PeerTableModel::NetNodeId);
    if (!clientModel || !clientModel->getPeerTableModel() || selected_peers.size() != 1) {
        ui->peersTabRightPanel->hide();
        ui->peerHeading->setText(tr("Select a peer to view detailed information."));
        return;
    }
    const auto stats = selected_peers.first().data(PeerTableModel::StatsRole).value<CNodeCombinedStats*>();
    // update the detail ui with latest node information
    QString peerAddrDetails(QString::fromStdString(stats->nodeStats.addrName) + " ");
    peerAddrDetails += tr("(peer: %1)").arg(QString::number(stats->nodeStats.nodeid));
    if (!stats->nodeStats.addrLocal.empty())
        peerAddrDetails += "<br />" + tr("via %1").arg(QString::fromStdString(stats->nodeStats.addrLocal));
    ui->peerHeading->setText(peerAddrDetails);
    ui->peerServices->setText(GUIUtil::formatServicesStr(stats->nodeStats.nServices));
    ui->peerRelayTxes->setText(stats->nodeStats.fRelayTxes ? ts.yes : ts.no);
    QString bip152_hb_settings;
    if (stats->nodeStats.m_bip152_highbandwidth_to) bip152_hb_settings = ts.to;
    if (stats->nodeStats.m_bip152_highbandwidth_from) bip152_hb_settings += (bip152_hb_settings.isEmpty() ? ts.from : QLatin1Char('/') + ts.from);
    if (bip152_hb_settings.isEmpty()) bip152_hb_settings = ts.no;
    ui->peerHighBandwidth->setText(bip152_hb_settings);
    const int64_t time_now{GetTimeSeconds()};
    ui->peerConnTime->setText(GUIUtil::formatDurationStr(time_now - stats->nodeStats.nTimeConnected));
    ui->peerLastBlock->setText(TimeDurationField(time_now, stats->nodeStats.nLastBlockTime));
    ui->peerLastTx->setText(TimeDurationField(time_now, stats->nodeStats.nLastTXTime));
    ui->peerLastSend->setText(TimeDurationField(time_now, stats->nodeStats.nLastSend));
    ui->peerLastRecv->setText(TimeDurationField(time_now, stats->nodeStats.nLastRecv));
    ui->peerBytesSent->setText(GUIUtil::formatBytes(stats->nodeStats.nSendBytes));
    ui->peerBytesRecv->setText(GUIUtil::formatBytes(stats->nodeStats.nRecvBytes));
    ui->peerPingTime->setText(GUIUtil::formatPingTime(stats->nodeStats.m_last_ping_time));
    ui->peerMinPing->setText(GUIUtil::formatPingTime(stats->nodeStats.m_min_ping_time));
    ui->timeoffset->setText(GUIUtil::formatTimeOffset(stats->nodeStats.nTimeOffset));
    ui->peerVersion->setText(QString::number(stats->nodeStats.nVersion));
    ui->peerSubversion->setText(QString::fromStdString(stats->nodeStats.cleanSubVer));
    ui->peerConnectionType->setText(GUIUtil::ConnectionTypeToQString(stats->nodeStats.m_conn_type, /* prepend_direction */ true));
    ui->peerNetwork->setText(GUIUtil::NetworkToQString(stats->nodeStats.m_network));
    if (stats->nodeStats.m_permissionFlags == NetPermissionFlags::None) {
        ui->peerPermissions->setText(ts.na);
    } else {
        QStringList permissions;
        for (const auto& permission : NetPermissions::ToStrings(stats->nodeStats.m_permissionFlags)) {
            permissions.append(QString::fromStdString(permission));
        }
        ui->peerPermissions->setText(permissions.join(" & "));
    }
    ui->peerMappedAS->setText(stats->nodeStats.m_mapped_as != 0 ? QString::number(stats->nodeStats.m_mapped_as) : ts.na);

    // This check fails for example if the lock was busy and
    // nodeStateStats couldn't be fetched.
    if (stats->fNodeStateStatsAvailable) {
        // Sync height is init to -1
        if (stats->nodeStateStats.nSyncHeight > -1) {
            ui->peerSyncHeight->setText(QString("%1").arg(stats->nodeStateStats.nSyncHeight));
        } else {
            ui->peerSyncHeight->setText(ts.unknown);
        }
        // Common height is init to -1
        if (stats->nodeStateStats.nCommonHeight > -1) {
            ui->peerCommonHeight->setText(QString("%1").arg(stats->nodeStateStats.nCommonHeight));
        } else {
            ui->peerCommonHeight->setText(ts.unknown);
        }
        ui->peerHeight->setText(QString::number(stats->nodeStateStats.m_starting_height));
        ui->peerPingWait->setText(GUIUtil::formatPingTime(stats->nodeStateStats.m_ping_wait));
    }

    ui->peersTabRightPanel->show();
}

void RPCConsole::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
}

void RPCConsole::showEvent(QShowEvent *event)
{
    QWidget::showEvent(event);

    if (!clientModel || !clientModel->getPeerTableModel())
        return;

    // start PeerTableModel auto refresh
    clientModel->getPeerTableModel()->startAutoRefresh();
}

void RPCConsole::hideEvent(QHideEvent *event)
{
    // It is too late to call QHeaderView::saveState() in ~RPCConsole(), as all of
    // the columns of QTableView child widgets will have zero width at that moment.
    m_peer_widget_header_state = ui->peerWidget->horizontalHeader()->saveState();
    m_banlist_widget_header_state = ui->banlistWidget->horizontalHeader()->saveState();

    QWidget::hideEvent(event);

    if (!clientModel || !clientModel->getPeerTableModel())
        return;

    // stop PeerTableModel auto refresh
    clientModel->getPeerTableModel()->stopAutoRefresh();
}

void RPCConsole::showPeersTableContextMenu(const QPoint& point)
{
    QModelIndex index = ui->peerWidget->indexAt(point);
    if (index.isValid())
        peersTableContextMenu->exec(QCursor::pos());
}

void RPCConsole::showBanTableContextMenu(const QPoint& point)
{
    QModelIndex index = ui->banlistWidget->indexAt(point);
    if (index.isValid())
        banTableContextMenu->exec(QCursor::pos());
}

void RPCConsole::disconnectSelectedNode()
{
    // Get selected peer addresses
    QList<QModelIndex> nodes = GUIUtil::getEntryData(ui->peerWidget, PeerTableModel::NetNodeId);
    for(int i = 0; i < nodes.count(); i++)
    {
        // Get currently selected peer address
        NodeId id = nodes.at(i).data().toLongLong();
        // Find the node, disconnect it and clear the selected node
        if(m_node.disconnectById(id))
            clearSelectedNode();
    }
}

void RPCConsole::banSelectedNode(int bantime)
{
    if (!clientModel)
        return;

    for (const QModelIndex& peer : GUIUtil::getEntryData(ui->peerWidget, PeerTableModel::NetNodeId)) {
        // Find possible nodes, ban it and clear the selected node
        const auto stats = peer.data(PeerTableModel::StatsRole).value<CNodeCombinedStats*>();
        if (stats) {
            m_node.ban(stats->nodeStats.addr, bantime);
            m_node.disconnectByAddress(stats->nodeStats.addr);
        }
    }
    clearSelectedNode();
    clientModel->getBanTableModel()->refresh();
}

void RPCConsole::unbanSelectedNode()
{
    if (!clientModel)
        return;

    // Get selected ban addresses
    QList<QModelIndex> nodes = GUIUtil::getEntryData(ui->banlistWidget, BanTableModel::Address);
    for(int i = 0; i < nodes.count(); i++)
    {
        // Get currently selected ban address
        QString strNode = nodes.at(i).data().toString();
        CSubNet possibleSubnet;

        LookupSubNet(strNode.toStdString(), possibleSubnet);
        if (possibleSubnet.IsValid() && m_node.unban(possibleSubnet))
        {
            clientModel->getBanTableModel()->refresh();
        }
    }
}

void RPCConsole::clearSelectedNode()
{
    ui->peerWidget->selectionModel()->clearSelection();
    cachedNodeids.clear();
    updateDetailWidget();
}

void RPCConsole::showOrHideBanTableIfRequired()
{
    if (!clientModel)
        return;

    bool visible = clientModel->getBanTableModel()->shouldShow();
    ui->banlistWidget->setVisible(visible);
    ui->banHeading->setVisible(visible);
}

std::vector<RPCConsole::TabTypes> RPCConsole::tabs() const
{
    std::vector<TabTypes> ret;
    ret.reserve(m_tabs.size());

    std::map<QWidget*, TabTypes> tabtype_map;
    for (const auto& tab : m_tabs) {
        tabtype_map[tab.second] = tab.first;
    }

    for (int i = 0; i < ui->tabWidget->count(); ++i) {
        auto tabtype = tabtype_map.find(ui->tabWidget->widget(i));
        if (tabtype != tabtype_map.end()) {
            ret.push_back(tabtype->second);
        }
    }
    return ret;
}

void RPCConsole::setTabFocus(enum TabTypes tabType)
{
    ui->tabWidget->setCurrentWidget(m_tabs[tabType]);
}

QString RPCConsole::tabTitle(TabTypes tab_type) const
{
    const int tab_index = ui->tabWidget->indexOf(m_tabs.at(tab_type));
    return ui->tabWidget->tabText(tab_index);
}

QKeySequence RPCConsole::tabShortcut(TabTypes tab_type) const
{
    switch (tab_type) {
    case TabTypes::INFO: return QKeySequence(Qt::CTRL + Qt::Key_I);
    case TabTypes::CONSOLE: return QKeySequence(Qt::CTRL + Qt::Key_T);
    case TabTypes::GRAPH: return QKeySequence(Qt::CTRL + Qt::Key_N);
    case TabTypes::PAIRING: return QKeySequence(Qt::ALT + Qt::Key_5);  // Only used in disablewallet mode - matches wallet GUI's pairing shortcut
    case TabTypes::PEERS: return QKeySequence(Qt::CTRL + Qt::Key_P);
    } // no default case, so the compiler can warn about missing cases

    assert(false);
}

void RPCConsole::updateAlerts(const QString& warnings)
{
    this->ui->label_alerts->setVisible(!warnings.isEmpty());
    this->ui->label_alerts->setText(warnings);
}
