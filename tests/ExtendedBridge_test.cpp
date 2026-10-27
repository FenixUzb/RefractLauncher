// SPDX-License-Identifier: GPL-3.0-only
/*
 *  Prism Extended (a fork of Prism Launcher; not affiliated with or endorsed by the Prism Launcher project)
 *
 *  Bridge tests. Core-backed cases need PRISM_EXT_TEST_CORE (path to the DevKit's src/ipc-server.mjs) and a
 *  node executable (PRISM_EXT_NODE or "node" on PATH); without them they are skipped. All files are synthetic
 *  and live in QTemporaryDir. Set PRISM_EXT_TEST_SCREENSHOT to a .png path to save a rendering of the page.
 */
#include <QApplication>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QProcessEnvironment>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>
#include <QTreeWidget>

#include "extended/CoreClient.h"
#include "extended/ExtendedAnalyzePage.h"

namespace {
QString nodeProgram()
{
    const QString configured = qEnvironmentVariable("PRISM_EXT_NODE");
    return configured.isEmpty() ? QStandardPaths::findExecutable("node") : configured;
}

/** Sends one request and waits (event loop running) for its single callback. */
QJsonObject roundTrip(CoreClient& client, const QString& command, const QJsonObject& params = {})
{
    QJsonObject response;
    int calls = 0;
    client.request(command, params, [&](const QJsonObject& r) {
        response = r;
        calls++;
    });
    [&] { QTRY_COMPARE_WITH_TIMEOUT(calls, 1, 15000); }();
    QTest::qWait(50);
    [&] { QCOMPARE(calls, 1); }();  // exactly once
    return response;
}
}  // namespace

class ExtendedBridgeTest : public QObject {
    Q_OBJECT

   private slots:
    void unconfiguredCoreIsAnErrorNotACrash()
    {
        CoreClient client(CoreClient::Config{});
        const auto response = roundTrip(client, "core.health");
        QCOMPARE(response.value("ok").toBool(true), false);
        QCOMPARE(response.value("error").toObject().value("code").toString(), QString("E_CORE_UNAVAILABLE"));
    }

    void missingRuntimeIsAnError()
    {
        CoreClient client({ "Z:/prism-ext-nonexistent/node.exe", { "core.mjs" } });
        QCOMPARE(roundTrip(client, "core.health").value("error").toObject().value("code").toString(), QString("E_CORE_UNAVAILABLE"));
    }

    void exitingCoreFailsPendingRequests()
    {
        if (nodeProgram().isEmpty())
            QSKIP("node not found");
        CoreClient client({ nodeProgram(), { "-e", "process.exit(3)" } });
        QCOMPARE(roundTrip(client, "core.health").value("error").toObject().value("code").toString(), QString("E_CORE_UNAVAILABLE"));
    }

    void hungCoreTimesOut()
    {
        if (nodeProgram().isEmpty())
            QSKIP("node not found");
        CoreClient::Config config{ nodeProgram(), { "-e", "setInterval(() => {}, 1000)" } };
        config.requestTimeoutMs = 300;
        CoreClient client(config);
        QCOMPARE(roundTrip(client, "core.health").value("error").toObject().value("code").toString(), QString("E_TIMEOUT"));
    }

    void realCoreHealthAndReadOnlyInventory()
    {
        const QString core = qEnvironmentVariable("PRISM_EXT_TEST_CORE");
        if (core.isEmpty() || nodeProgram().isEmpty())
            QSKIP("PRISM_EXT_TEST_CORE or node not set");
        CoreClient client({ nodeProgram(), { core } });
        const auto health = roundTrip(client, "core.health");
        QVERIFY(health.value("ok").toBool());
        QVERIFY(health.value("result").toObject().value("readOnly").toBool());

        QTemporaryDir dir;
        QFile file(dir.filePath("synthetic.jar"));
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("SYNTHETIC, not a zip");
        file.close();
        const auto inventory = roundTrip(client, "inventory.analyze", { { "modsDir", dir.path() } });
        QVERIFY2(inventory.value("ok").toBool(), qPrintable(QJsonDocument(inventory).toJson()));
        const auto record = inventory.value("result").toObject().value("files").toArray().at(0).toObject();
        QCOMPARE(record.value("status").toString(), QString("invalid_jar_signature"));
        QCOMPARE(record.value("identity").toObject().value("status").toString(), QString("not_checked"));
        QFile check(dir.filePath("synthetic.jar"));
        QVERIFY(check.open(QIODevice::ReadOnly));
        QCOMPARE(check.readAll(), QByteArray("SYNTHETIC, not a zip"));  // read-only
    }

    void pageRendersAResultAndErrors()
    {
        ExtendedAnalyzePage page("synthetic-mods", CoreClient::Config{});
        const auto mod = [](const char* loader, const char* id, const char* version) {
            return QJsonObject{ { "loader", loader }, { "modId", id }, { "version", version }, { "source", "fabric.mod.json" } };
        };
        const QJsonObject result{
            { "files",
              QJsonArray{
                  QJsonObject{ { "filename", "synthfabric-1.0.jar" },
                               { "enabled", true },
                               { "status", "unidentified" },
                               { "metadata", QJsonObject{ { "mods", QJsonArray{ mod("fabric", "synthfabric", "1.0.0") } } } },
                               { "identity", QJsonObject{ { "status", "claimed" }, { "claimedProjectId", "SYNpr001" } } } },
                  QJsonObject{ { "filename", "broken.jar.disabled" },
                               { "enabled", false },
                               { "status", "invalid_archive" },
                               { "reason", "crc_mismatch" },
                               { "identity", QJsonObject{ { "status", "not_checked" }, { "reason", "not_a_valid_archive" } } } },
              } },
            { "summary", QJsonObject{ { "identified", 0 }, { "claimed", 1 }, { "unknown", 0 }, { "notChecked", 1 } } },
        };
        page.showResult(QJsonObject{ { "ok", true }, { "result", result } });
        QCOMPARE(page.tree()->topLevelItemCount(), 2);
        const auto* first = page.tree()->topLevelItem(1);  // sorted by file name: broken.jar.disabled first
        QCOMPARE(first->text(0), QString("synthfabric-1.0.jar"));
        QCOMPARE(first->text(3), QString("fabric"));
        QCOMPARE(first->text(6), QString("claimed"));
        QCOMPARE(first->text(7), QString("SYNpr001"));
        QVERIFY(page.statusText().contains("1 confirmed by install records"));

        const QString screenshot = qEnvironmentVariable("PRISM_EXT_TEST_SCREENSHOT");
        if (!screenshot.isEmpty()) {
            page.resize(1000, 320);
            page.show();
            QVERIFY(page.grab().save(screenshot));
        }

        page.showResult(CoreClient::errorResponse("E_CORE_UNAVAILABLE", "synthetic failure"));
        QCOMPARE(page.tree()->topLevelItemCount(), 0);
        QVERIFY(page.statusText().contains("synthetic failure"));
    }
};

int main(int argc, char** argv)
{
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM"))
        qputenv("QT_QPA_PLATFORM", "offscreen");  // no window on the developer's desktop or in CI
    QApplication app(argc, argv);
    ExtendedBridgeTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "ExtendedBridge_test.moc"
