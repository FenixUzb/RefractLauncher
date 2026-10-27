// SPDX-License-Identifier: GPL-3.0-only
/*
 *  Refract Launcher (a fork of Prism Launcher; not affiliated with or endorsed by the Prism Launcher project)
 */
#pragma once

#include <QIcon>
#include <QJsonObject>
#include <QWidget>

#include "extended/CoreClient.h"
#include "ui/pages/BasePage.h"

class MinecraftInstance;
class QLabel;
class QPushButton;
class QTreeWidget;

/**
 * Instance page "Mods Analysis": asks the Refract core for a read-only inventory of the instance's mods folder with
 * offline identity (Prism's own install records re-checked against file hashes). It only passes the folder
 * path; it never touches Prism's mod models, which rename and delete files while loading.
 */
class ExtendedAnalyzePage : public QWidget, public BasePage {
    Q_OBJECT
   public:
    explicit ExtendedAnalyzePage(MinecraftInstance* inst, QWidget* parent = nullptr);
    /** For tests: a mods folder and a core configuration, no instance. */
    ExtendedAnalyzePage(QString modsDir, CoreClient::Config config, QWidget* parent = nullptr);

    QString id() const override { return "extended-analyze"; }
    QString displayName() const override { return tr("Mods Analysis"); }
    QIcon icon() const override { return QIcon::fromTheme("loadermods"); }
    void openedImpl() override;
    void retranslate() override;

    void analyze();
    void showResult(const QJsonObject& response);
    QTreeWidget* tree() const { return m_tree; }
    QString statusText() const;

   private:
    void buildUi();

    QString m_modsDir;
    CoreClient* m_client;
    QLabel* m_notice = nullptr;
    QLabel* m_status = nullptr;
    QPushButton* m_analyze = nullptr;
    QTreeWidget* m_tree = nullptr;
    bool m_analyzed = false;
};
