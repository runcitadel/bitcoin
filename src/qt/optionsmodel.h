// Copyright (c) 2011-2020 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_QT_OPTIONSMODEL_H
#define BITCOIN_QT_OPTIONSMODEL_H

#include <amount.h>
#include <cstdint>
#include <qt/guiconstants.h>

#include <QAbstractListModel>

#include <assert.h>

namespace interfaces {
class Node;
}

extern const char *DEFAULT_GUI_PROXY_HOST;
static constexpr uint16_t DEFAULT_GUI_PROXY_PORT = 9050;

/**
 * Convert configured prune target MiB to displayed GB. Round up to avoid underestimating max disk usage.
 */
static inline int PruneMiBtoGB(int64_t mib) { return (mib * 1024 * 1024 + GB_BYTES - 1) / GB_BYTES; }

/**
 * Convert displayed prune target GB to configured MiB. Round down so roundtrip GB -> MiB -> GB conversion is stable.
 */
static inline int64_t PruneGBtoMiB(int gb) { return gb * GB_BYTES / 1024 / 1024; }

/** Interface from Qt to configuration data structure for Bitcoin client.
   To Qt, the options are presented as a list with the different options
   laid out vertically.
   This can be changed to a tree once the settings become sufficiently
   complex.
 */
class OptionsModel : public QAbstractListModel
{
    Q_OBJECT

public:
    explicit OptionsModel(QObject *parent = nullptr, bool resetSettings = false);

    enum OptionID {
        StartAtStartup,         // bool
        ShowTrayIcon,           // bool
        MinimizeToTray,         // bool
        NetworkPort,            // int
        MapPortUPnP,            // bool
        MapPortNatpmp,          // bool
        MinimizeOnClose,        // bool
        ProxyUse,               // bool
        ProxyIP,                // QString
        ProxyPort,              // int
        ProxyUseTor,            // bool
        ProxyIPTor,             // QString
        ProxyPortTor,           // int
        DisplayUnit,            // BitcoinUnits::Unit
        DisplayAddresses,       // bool
        ThirdPartyTxUrls,       // QString
        Language,               // QString
        UseEmbeddedMonospacedFont, // bool
        PeersTabAlternatingRowColors, // bool
        walletrbf,              // bool
        CoinControlFeatures,    // bool
        SubFeeFromAmount,       // bool
        ThreadsScriptVerif,     // int
        PruneMiB,               // int
        DatabaseCache,          // int
        ExternalSignerPath,     // QString
        SpendZeroConfChange,    // bool
        addresstype,            // QString
        Listen,                 // bool
        Server,                 // bool
        maxuploadtarget,
        peerbloomfilters,       // bool
        peerblockfilters,       // bool
        mempoolreplacement,
        maxorphantx,
        maxmempool,
        incrementalrelayfee,
        mempoolexpiry,
        rejectunknownscripts,   // bool
        rejectspkreuse,         // bool
        minrelaytxfee,
        bytespersigop,
        bytespersigopstrict,
        limitancestorcount,
        limitancestorsize,
        limitdescendantcount,
        limitdescendantsize,
        rejectbaremultisig,     // bool
        datacarriersize,
        dustrelayfee,
        blockmintxfee,
        blockmaxsize,
        blockprioritysize,
        blockmaxweight,
        blockreconstructionextratxn,
        corepolicy,
        OptionIDRowCount,
    };

    void Init(bool resetSettings = false);
    void Reset();

    int rowCount(const QModelIndex & parent = QModelIndex()) const override;
    QVariant data(const QModelIndex & index, int role = Qt::DisplayRole) const override;
    bool setData(const QModelIndex & index, const QVariant & value, int role = Qt::EditRole) override;
    /** Updates current unit in memory, settings and emits displayUnitChanged(newUnit) signal */
    void setDisplayUnit(const QVariant &value);

    /* Explicit getters */
    bool getShowTrayIcon() const { return m_show_tray_icon; }
    bool getMinimizeToTray() const { return fMinimizeToTray; }
    bool getMinimizeOnClose() const { return fMinimizeOnClose; }
    int getDisplayUnit() const { return nDisplayUnit; }
    bool getDisplayAddresses() const { return bDisplayAddresses; }
    QString getThirdPartyTxUrls() const { return strThirdPartyTxUrls; }
    bool getUseEmbeddedMonospacedFont() const { return m_use_embedded_monospaced_font; }
    bool getPeersTabAlternatingRowColors() const { return m_peers_tab_alternating_row_colors; }
    bool getCoinControlFeatures() const { return fCoinControlFeatures; }
    bool getSubFeeFromAmount() const { return m_sub_fee_from_amount; }
    const QString& getOverriddenByCommandLine() { return strOverriddenByCommandLine; }

    /* Explicit setters */
    void SetPruneMiB(int64_t prune_target_MiB, bool force = false);

    /* Restart flag helper */
    void setRestartRequired(bool fRequired);
    bool isRestartRequired() const;

    interfaces::Node& node() const { assert(m_node); return *m_node; }
    void setNode(interfaces::Node& node) { assert(!m_node); m_node = &node; }

private:
    interfaces::Node* m_node = nullptr;
    /* Qt-only settings */
    bool m_show_tray_icon;
    bool fMinimizeToTray;
    bool fMinimizeOnClose;
    QString language;
    int nDisplayUnit;
    bool bDisplayAddresses;
    QString strThirdPartyTxUrls;
    bool m_use_embedded_monospaced_font;
    bool m_peers_tab_alternating_row_colors;
    bool fCoinControlFeatures;
    bool m_sub_fee_from_amount;
    /* settings that were overridden by command-line */
    QString strOverriddenByCommandLine;

    /* rwconf settings that require a restart */
    bool f_peerbloomfilters;
    bool f_rejectspkreuse;

    // Add option to list of GUI options overridden through command line/config file
    void addOverriddenOption(const std::string &option);

    // Check settings version and upgrade default values if required
    void checkAndMigrate();
Q_SIGNALS:
    void displayUnitChanged(int unit);
    void coinControlFeaturesChanged(bool);
    void showTrayIconChanged(bool);
    void useEmbeddedMonospacedFontChanged(bool);
    void peersTabAlternatingRowColorsChanged(bool);
};

#endif // BITCOIN_QT_OPTIONSMODEL_H
