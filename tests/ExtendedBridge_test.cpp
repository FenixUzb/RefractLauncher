// SPDX-License-Identifier: GPL-3.0-only
/*
 *  Refract Launcher (a fork of Prism Launcher; not affiliated with or endorsed by the Prism Launcher project)
 *
 *  Bridge tests. Core-backed cases need PRISM_EXT_TEST_CORE (path to the DevKit's src/ipc-server.mjs) and a
 *  node executable (PRISM_EXT_NODE or "node" on PATH); without them they are skipped. All files are synthetic
 *  and live in QTemporaryDir. Set PRISM_EXT_TEST_SCREENSHOT to a .png path to save a rendering of the page.
 */
#include <QApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QPointer>
#include <QJsonArray>
#include <QJsonDocument>
#include <QProcessEnvironment>
#include <QScopeGuard>
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

/** A fake core: node running `js` (CommonJS, stdin lines in `rl`), extra args after it. */
CoreClient::Config fakeCore(const QString& js, const QStringList& args = {})
{
    const QString prelude = "const rl = require('readline').createInterface({ input: process.stdin });\n"
                            "const reply = (line, extra = {}) => { const r = JSON.parse(line);"
                            " process.stdout.write(JSON.stringify({ protocolVersion: 1, requestId: r.requestId, ok: true, result: {}, ...extra }) + '\\n'); };\n";
    return { nodeProgram(), QStringList{ "-e", prelude + js } + args };
}

QString errorCode(const QJsonObject& response)
{
    return response.value("error").toObject().value("code").toString();
}

/** Sends one request and waits (event loop running) for its single callback. */
QJsonObject roundTrip(CoreClient& client, const QString& command, const QJsonObject& params = {}, int timeoutMs = -1)
{
    QJsonObject response;
    int calls = 0;
    client.request(command, params, [&](const QJsonObject& r) {
        response = r;
        calls++;
    }, timeoutMs);
    [&] { QTRY_COMPARE_WITH_TIMEOUT(calls, 1, 15000); }();
    QTest::qWait(50);
    [&] { QCOMPARE(calls, 1); }();  // exactly once
    return response;
}
}  // namespace

class ExtendedBridgeTest : public QObject {
    Q_OBJECT

   private slots:
    void discoveryPrefersOverridesThenTheBundleThenPath()
    {
        QTemporaryDir app;
        QVERIFY(app.isValid());
        const QDir dir(app.path());
        // Without a bundle and without overrides the core is not configured (no arguments).
        QVERIFY(CoreClient::discover(dir.path(), QProcessEnvironment()).arguments.isEmpty());

        QVERIFY(dir.mkpath("refract-core/src"));
        const QString core = dir.filePath("refract-core/src/ipc-server.mjs");
#ifdef Q_OS_WIN
        const QString node = dir.filePath("refract-core/node.exe");
#else
        const QString node = dir.filePath("refract-core/node");
#endif
        for (const QString& path : { core, node }) {
            QFile file(path);
            QVERIFY(file.open(QIODevice::WriteOnly));
        }
        const auto bundled = CoreClient::discover(dir.path(), QProcessEnvironment());
        QCOMPARE(bundled.program, node);
        QCOMPARE(bundled.arguments, QStringList{ core });

        QProcessEnvironment env;
        const QString otherNode = QDir(QDir::tempPath()).absoluteFilePath("refract-dev/node.exe");
        env.insert("PRISM_EXT_NODE", otherNode);
        env.insert("PRISM_EXT_CORE", "D:/dev/core/src/ipc-server.mjs");
        const auto overridden = CoreClient::discover(dir.path(), env);
        QCOMPARE(overridden.program, otherNode);
        QCOMPARE(overridden.arguments, QStringList{ "D:/dev/core/src/ipc-server.mjs" });

        // Overrides must be complete paths; anything else leaves the core unconfigured instead of running something
        // relative to the current folder (Prism's data dir). "C:node.exe" is relative to drive C's current folder.
        for (const QString& bad : { QString("C:node.exe"), QString("tools/node.exe"), QString("node") }) {
            QProcessEnvironment badNode;
            badNode.insert("PRISM_EXT_NODE", bad);
            QVERIFY2(CoreClient::discover(dir.path(), badNode).program.isEmpty(), qPrintable(bad));
        }
        QProcessEnvironment badCore;
        badCore.insert("PRISM_EXT_CORE", "src/ipc-server.mjs");
        QVERIFY(CoreClient::discover(dir.path(), badCore).arguments.isEmpty());

        // A bundle without its own node falls back to node on PATH: only PATH entries that are complete paths,
        // never the current folder, and nothing at all when PATH has no usable entry.
        QVERIFY(QFile::remove(node));
        QTemporaryDir onPath;
        QTemporaryDir cwd;
        QVERIFY(onPath.isValid() && cwd.isValid());
#ifdef Q_OS_WIN
        const QString nodeName = "node.exe";
#else
        const QString nodeName = "node";
#endif
        for (const QString& path : { QDir(onPath.path()).filePath(nodeName), QDir(cwd.path()).filePath(nodeName),
                                     QDir(cwd.path()).filePath("tools/" + nodeName) }) {
            QVERIFY(QDir().mkpath(QFileInfo(path).path()));
            QFile file(path);
            QVERIFY(file.open(QIODevice::WriteOnly));
            file.setPermissions(file.permissions() | QFileDevice::ExeOwner);
        }
        const QString previous = QDir::currentPath();
        QVERIFY(QDir::setCurrent(cwd.path()));
        const auto restore = qScopeGuard([&] { QDir::setCurrent(previous); });
        const QChar sep = QDir::listSeparator();
        QProcessEnvironment pathEnv;
        pathEnv.insert("PATH", QString(".") + sep + "tools" + sep + onPath.path());
        const auto pathNode = CoreClient::discover(dir.path(), pathEnv);
        QCOMPARE(pathNode.arguments, QStringList{ core });
        QCOMPARE(QFileInfo(pathNode.program).canonicalFilePath(), QFileInfo(QDir(onPath.path()).filePath(nodeName)).canonicalFilePath());
        QProcessEnvironment relativeOnly;
        relativeOnly.insert("PATH", QString(".") + sep + "tools");
        QVERIFY(CoreClient::discover(dir.path(), relativeOnly).program.isEmpty());
        QVERIFY(CoreClient::discover(dir.path(), QProcessEnvironment()).program.isEmpty());

        // A folder named like the bundled node is not a program.
        QVERIFY(dir.mkpath(QString("refract-core/") + nodeName));
        QCOMPARE(QFileInfo(CoreClient::discover(dir.path(), pathEnv).program).canonicalFilePath(),
                 QFileInfo(QDir(onPath.path()).filePath(nodeName)).canonicalFilePath());

        // A relative application folder would resolve against the current folder too: the bundle is not used.
        QVERIFY(QDir(cwd.path()).mkpath("app/refract-core/src"));
        QFile relativeCore(QDir(cwd.path()).filePath("app/refract-core/src/ipc-server.mjs"));
        QVERIFY(relativeCore.open(QIODevice::WriteOnly));
        relativeCore.close();
        QVERIFY(CoreClient::discover("app", QProcessEnvironment()).arguments.isEmpty());
    }

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

    void timeoutRestartsTheCore()
    {
        if (nodeProgram().isEmpty())
            QSKIP("node not found");
        // The first process blocks forever on its first request; a restarted process answers normally.
        QTemporaryDir dir;
        const QString marker = dir.filePath("hung-once");
        CoreClient client(fakeCore("const fs = require('fs'); rl.on('line', l => { if (!fs.existsSync(process.argv[1])) {"
                                   " fs.writeFileSync(process.argv[1], 'x'); while (true) {} } reply(l); });",
                                   { marker }));
        QCOMPARE(errorCode(roundTrip(client, "core.health", {}, 300)), QString("E_TIMEOUT"));
        QVERIFY(roundTrip(client, "core.health", {}, 10000).value("ok").toBool());
    }

    void restartsAfterTheCoreExits()
    {
        if (nodeProgram().isEmpty())
            QSKIP("node not found");
        CoreClient client(fakeCore("rl.on('line', l => { reply(l); setTimeout(() => process.exit(0), 10); });"));
        QVERIFY(roundTrip(client, "core.health").value("ok").toBool());
        QTest::qWait(300);
        QVERIFY(roundTrip(client, "core.health").value("ok").toBool());
    }

    void deletingABusyClientDoesNotBlock()
    {
        if (nodeProgram().isEmpty())
            QSKIP("node not found");
        auto* client = new CoreClient(fakeCore("rl.on('line', () => { while (true) {} });"));
        bool called = false;
        client->request("core.health", {}, [&](const QJsonObject&) { called = true; });
        QTest::qWait(300);  // the core is now busy
        QElapsedTimer timer;
        timer.start();
        delete client;
        QVERIFY2(timer.elapsed() < 300, qPrintable(QString("delete blocked %1 ms").arg(timer.elapsed())));
        QTest::qWait(100);
        QVERIFY(!called);  // nothing runs after the client is gone
    }

    void deletingTheClientInsideACallbackIsSafe()
    {
        if (nodeProgram().isEmpty())
            QSKIP("node not found");
        // Two responses arrive in one write; the first callback deletes the client.
        auto* client = new CoreClient(fakeCore("const ids = []; rl.on('line', l => { ids.push(JSON.parse(l).requestId); if (ids.length === 2)"
                                               " process.stdout.write(ids.map(id => JSON.stringify({ protocolVersion: 1, requestId: id, ok: true, result: {} })).join('\\n') + '\\n'); });"));
        QPointer<CoreClient> guard(client);
        int calls = 0;
        client->request("core.health", {}, [&](const QJsonObject&) {
            calls++;
            delete client;
        });
        client->request("core.health", {}, [&](const QJsonObject&) { calls++; });
        QTRY_VERIFY_WITH_TIMEOUT(guard.isNull(), 10000);
        QTest::qWait(100);
        QCOMPARE(calls, 1);
    }

    void retryingFromAnErrorCallbackWorks()
    {
        if (nodeProgram().isEmpty())
            QSKIP("node not found");
        QTemporaryDir dir;
        // First process: hangs (timeout case) or writes garbage (protocol case) on its first request only.
        for (const QString& misbehave : { QString("while (true) {}"), QString("process.stdout.write('not json\\n'); return;") }) {
            const QString marker = dir.filePath(QString("once-%1").arg(misbehave.size()));
            CoreClient client(fakeCore("const fs = require('fs'); rl.on('line', l => { if (!fs.existsSync(process.argv[1])) {"
                                       " fs.writeFileSync(process.argv[1], 'x'); " + misbehave + " } reply(l); });",
                                       { marker }));
            QString firstCode;
            QJsonObject retried;
            client.request("core.health", {}, [&](const QJsonObject& response) {
                firstCode = errorCode(response);
                client.request("core.health", {}, [&](const QJsonObject& again) { retried = again; }, 10000);  // retry from inside
            }, 300);
            QTRY_VERIFY_WITH_TIMEOUT(!retried.isEmpty(), 15000);
            QVERIFY2(firstCode == "E_TIMEOUT" || firstCode == "E_PROTOCOL", qPrintable(firstCode));
            QVERIFY2(retried.value("ok").toBool(), qPrintable(QJsonDocument(retried).toJson()));
        }
    }

    void deletingTheClientInsideAnErrorCallbackIsSafe()
    {
        if (nodeProgram().isEmpty())
            QSKIP("node not found");
        for (const QString& misbehave : { QString("rl.on('line', () => { while (true) {} });"),
                                          QString("rl.on('line', () => process.stdout.write('not json\\n'));") }) {
            auto* client = new CoreClient(fakeCore(misbehave));
            QPointer<CoreClient> guard(client);
            int calls = 0;
            client->request("core.health", {}, [&](const QJsonObject&) {
                calls++;
                delete client;
            }, 300);
            client->request("core.health", {}, [&](const QJsonObject&) { calls++; }, 300);
            QTRY_VERIFY_WITH_TIMEOUT(guard.isNull(), 10000);
            QTest::qWait(500);
            QCOMPARE(calls, 1);
        }
    }

    void protocolViolationsFailFast()
    {
        if (nodeProgram().isEmpty())
            QSKIP("node not found");
        CoreClient garbage(fakeCore("rl.on('line', () => process.stdout.write('not json\\n'));"));
        QElapsedTimer timer;
        timer.start();
        QCOMPARE(errorCode(roundTrip(garbage, "core.health")), QString("E_PROTOCOL"));
        QVERIFY(timer.elapsed() < 5000);

        auto config = fakeCore("rl.on('line', l => reply(l, { pad: 'x'.repeat(1500) }));");
        config.maxResponseBytes = 1000;
        CoreClient big(config);
        QCOMPARE(errorCode(roundTrip(big, "core.health")), QString("E_RESPONSE_TOO_LARGE"));
    }

    void coreStderrExplainsAnExit()
    {
        if (nodeProgram().isEmpty())
            QSKIP("node not found");
        CoreClient client(fakeCore("process.stderr.write('SYNTHETIC boom: cannot find module\\n'); setTimeout(() => process.exit(1), 50);"));
        const auto response = roundTrip(client, "core.health");
        QCOMPARE(errorCode(response), QString("E_CORE_UNAVAILABLE"));
        QVERIFY2(response.value("error").toObject().value("message").toString().contains("SYNTHETIC boom"),
                 qPrintable(QJsonDocument(response).toJson()));
    }

    void environmentConfigResolvesNodeToAnAbsolutePath()
    {
        if (nodeProgram().isEmpty())
            QSKIP("node not found");
        const QByteArray savedNode = qgetenv("PRISM_EXT_NODE");
        const auto restore = qScopeGuard([&] {
            if (savedNode.isEmpty())
                qunsetenv("PRISM_EXT_NODE");
            else
                qputenv("PRISM_EXT_NODE", savedNode);
        });
        qunsetenv("PRISM_EXT_NODE");
        const auto config = CoreClient::configFromEnvironment();
        QVERIFY2(QFileInfo(config.program).isAbsolute(), qPrintable(config.program));  // never looked up in Prism's data dir
        // A bare program name is not a complete path: it is refused rather than searched for.
        qputenv("PRISM_EXT_NODE", "node");
        QVERIFY(CoreClient::configFromEnvironment().program.isEmpty());
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

    void pageAnalyzesARelativeModsFolderEndToEnd()
    {
        const QString core = qEnvironmentVariable("PRISM_EXT_TEST_CORE");
        if (core.isEmpty() || nodeProgram().isEmpty())
            QSKIP("PRISM_EXT_TEST_CORE or node not set");
        QTemporaryDir dir;
        QVERIFY(QDir(dir.path()).mkpath("instances/synthetic/minecraft/mods"));
        QFile file(dir.filePath("instances/synthetic/minecraft/mods/synthetic.jar"));
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("SYNTHETIC, not a zip");
        file.close();
        struct RestoreCurrentDir {
            QString path = QDir::currentPath();
            ~RestoreCurrentDir() { QDir::setCurrent(path); }  // also when a check below fails and returns early
        } restore;
        QDir::setCurrent(dir.path());  // Prism keeps instance paths relative to its data directory
        ExtendedAnalyzePage page("instances/synthetic/minecraft/mods", { nodeProgram(), { core } });
        page.analyze();
        QTRY_VERIFY_WITH_TIMEOUT(page.tree()->topLevelItemCount() == 1, 15000);
        QCOMPARE(page.tree()->topLevelItem(0)->text(0), QString("synthetic.jar"));
        QVERIFY2(page.statusText().startsWith("1 file"), qPrintable(page.statusText()));
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
        auto withIssues = result;
        withIssues.insert("indexIssues", QJsonArray{ QJsonObject{ { "code", "index_unparsable" }, { "indexFile", "x.pw.toml" } } });
        page.showResult(QJsonObject{ { "ok", true }, { "result", withIssues } });
        QCOMPARE(page.tree()->topLevelItemCount(), 2);
        const auto* first = page.tree()->topLevelItem(1);  // sorted by file name: broken.jar.disabled first
        QCOMPARE(first->text(0), QString("synthfabric-1.0.jar"));
        QCOMPARE(first->text(2), QString("valid"));
        QCOMPARE(first->text(3), QString("fabric"));
        // A claimed identity is the launcher's own record, not a provider confirmation: never shown as verified.
        QCOMPARE(first->text(6), QString("claimed (unverified)"));
        QCOMPARE(first->text(7), QString("SYNpr001 (claimed)"));
        QVERIFY2(page.statusText().contains("1 claimed by install records (unverified)"), qPrintable(page.statusText()));
        QVERIFY2(page.statusText().contains("Index records skipped: 1"), qPrintable(page.statusText()));
        QVERIFY(!page.statusText().contains("confirmed"));

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
