// SPDX-License-Identifier: GPL-3.0-only
/*
 *  Prism Extended (a fork of Prism Launcher; not affiliated with or endorsed by the Prism Launcher project)
 */
#include "ExtendedAnalyzePage.h"

#include <QColor>
#include <QDir>
#include <QFileInfo>
#include <QHeaderView>
#include <QJsonArray>
#include <QLabel>
#include <QPushButton>
#include <QSet>
#include <QTreeWidget>
#include <QVBoxLayout>

#include "minecraft/MinecraftInstance.h"

namespace {
enum Column { ColFile, ColEnabled, ColArchive, ColLoader, ColModId, ColVersion, ColIdentity, ColProject, ColumnCount };

QString joinedUnique(const QJsonArray& mods, const char* key)
{
    QStringList values;
    for (const auto& value : mods) {
        const auto mod = value.toObject();
        const QString text = mod.value(key).toString();
        if (!mod.contains("nested") && !text.isEmpty() && !values.contains(text))
            values << text;
    }
    return values.join(", ");
}
}  // namespace

ExtendedAnalyzePage::ExtendedAnalyzePage(MinecraftInstance* inst, QWidget* parent)
    : ExtendedAnalyzePage(inst->modsRoot(), CoreClient::configFromEnvironment(), parent)
{}

ExtendedAnalyzePage::ExtendedAnalyzePage(QString modsDir, CoreClient::Config config, QWidget* parent)
    : QWidget(parent), m_modsDir(std::move(modsDir)), m_client(new CoreClient(std::move(config), this))
{
    buildUi();
}

void ExtendedAnalyzePage::buildUi()
{
    auto* layout = new QVBoxLayout(this);
    m_notice = new QLabel(this);
    m_notice->setWordWrap(true);
    m_status = new QLabel(this);
    m_status->setWordWrap(true);
    m_status->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_analyze = new QPushButton(this);
    connect(m_analyze, &QPushButton::clicked, this, &ExtendedAnalyzePage::analyze);
    m_tree = new QTreeWidget(this);
    m_tree->setColumnCount(ColumnCount);
    m_tree->setRootIsDecorated(false);
    m_tree->setSortingEnabled(true);
    m_tree->setAlternatingRowColors(true);
    m_tree->header()->setSectionResizeMode(QHeaderView::ResizeToContents);
    layout->addWidget(m_notice);
    layout->addWidget(m_analyze, 0, Qt::AlignLeft);
    layout->addWidget(m_status);
    layout->addWidget(m_tree, 1);
    retranslate();
}

void ExtendedAnalyzePage::retranslate()
{
    m_notice->setText(tr("Read-only analysis by the Prism Extended core: files in the mods folder are only read, never changed. "
                         "Identity comes from the launcher's own install records checked against the file hashes; no network is used."));
    m_analyze->setText(tr("Analyze"));
    m_tree->setHeaderLabels({ tr("File"), tr("Enabled"), tr("Archive"), tr("Loader"), tr("Mod ID"), tr("Version"), tr("Identity"),
                              tr("Project") });
}

void ExtendedAnalyzePage::openedImpl()
{
    if (!m_analyzed)
        analyze();
}

void ExtendedAnalyzePage::analyze()
{
    m_analyzed = true;
    m_analyze->setEnabled(false);
    m_status->setText(tr("Analyzing…"));
    // Prism may keep instance paths relative to its data directory; the core accepts only local absolute paths.
    const QString modsDir = QDir::toNativeSeparators(QFileInfo(m_modsDir).absoluteFilePath());
    m_client->request("inventory.analyze", QJsonObject{ { "modsDir", modsDir } }, [this](const QJsonObject& response) {
        m_analyze->setEnabled(true);
        showResult(response);
    });
}

QString ExtendedAnalyzePage::statusText() const
{
    return m_status->text();
}

void ExtendedAnalyzePage::showResult(const QJsonObject& response)
{
    m_tree->clear();
    if (!response.value("ok").toBool()) {
        const auto error = response.value("error").toObject();
        m_status->setText(tr("Analysis unavailable (%1): %2").arg(error.value("code").toString(), error.value("message").toString()));
        return;
    }
    const auto result = response.value("result").toObject();
    const auto files = result.value("files").toArray();
    m_tree->setSortingEnabled(false);
    for (const auto& value : files) {
        const auto file = value.toObject();
        const auto metadata = file.value("metadata").toObject();
        const auto mods = metadata.value("mods").toArray();
        const auto identity = file.value("identity").toObject();
        const QString archive = file.value("reason").toString().isEmpty()
                                    ? file.value("status").toString()
                                    : QString("%1 (%2)").arg(file.value("status").toString(), file.value("reason").toString());
        QString project = identity.value("projectId").toString();
        if (project.isEmpty())
            project = identity.value("claimedProjectId").toString();
        auto* item = new QTreeWidgetItem(m_tree);
        item->setText(ColFile, file.value("filename").toString());
        item->setText(ColEnabled, file.value("enabled").toBool(true) ? tr("yes") : tr("no"));
        item->setText(ColArchive, archive);
        item->setText(ColLoader, joinedUnique(mods, "loader"));
        item->setText(ColModId, joinedUnique(mods, "modId"));
        item->setText(ColVersion, joinedUnique(mods, "version"));
        item->setText(ColIdentity, identity.value("status").toString());
        item->setText(ColProject, project);
        item->setToolTip(ColIdentity, identity.value("reason").toString());
        const QString status = file.value("status").toString();
        if (status == "invalid_archive" || status == "invalid_jar_signature")
            item->setForeground(ColArchive, QColor(0xc0, 0x39, 0x2b));
    }
    m_tree->setSortingEnabled(true);
    m_tree->sortByColumn(ColFile, Qt::AscendingOrder);
    const auto summary = result.value("summary").toObject();
    m_status->setText(tr("%n file(s): %1 identified, %2 confirmed by install records, %3 unknown, %4 not checked.", "", files.size())
                          .arg(summary.value("identified").toInt())
                          .arg(summary.value("claimed").toInt())
                          .arg(summary.value("unknown").toInt())
                          .arg(summary.value("notChecked").toInt()));
}
