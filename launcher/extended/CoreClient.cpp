// SPDX-License-Identifier: GPL-3.0-only
/*
 *  Prism Extended (a fork of Prism Launcher; not affiliated with or endorsed by the Prism Launcher project)
 */
#include "CoreClient.h"

#include <QCoreApplication>
#include <QFileInfo>
#include <QJsonDocument>
#include <QPointer>
#include <QProcess>
#include <QProcessEnvironment>
#include <QStandardPaths>
#include <QTimer>
#include <utility>

namespace {
constexpr qsizetype stderrTailBytes = 4096;
}

CoreClient::Config CoreClient::configFromEnvironment()
{
    const auto env = QProcessEnvironment::systemEnvironment();
    Config config;
    // On Windows QProcess looks for a bare program name in the current directory (Prism's data dir) before
    // PATH; resolving it here means only PATH or an explicit absolute path is ever used.
    const QString node = env.value("PRISM_EXT_NODE", "node");
    config.program = QFileInfo(node).isAbsolute() ? node : QStandardPaths::findExecutable(node);
    const QString core = env.value("PRISM_EXT_CORE");
    if (!core.isEmpty())
        config.arguments = { core };
    return config;
}

QJsonObject CoreClient::errorResponse(const QString& code, const QString& message)
{
    return QJsonObject{ { "ok", false }, { "error", QJsonObject{ { "code", code }, { "message", message } } } };
}

CoreClient::CoreClient(Config config, QObject* parent) : QObject(parent), m_config(std::move(config)) {}

CoreClient::~CoreClient()
{
    // The owner is going away: pending callbacks are dropped, not called.
    const bool busy = !m_pending.isEmpty();
    m_pending.clear();
    releaseProcess(busy);
}

void CoreClient::releaseProcess(bool kill)
{
    if (!m_process)
        return;
    QProcess* process = std::exchange(m_process, nullptr);
    process->disconnect(this);
    if (process->state() == QProcess::NotRunning) {
        process->deleteLater();
        return;
    }
    // Never wait on the GUI thread: hand the process to a reaper owned by the application. A busy core is killed
    // now; an idle one gets EOF (it exits by itself) and is killed after 2 s if it has not.
    if (auto* app = QCoreApplication::instance())
        process->setParent(app);
    QObject::connect(process, &QProcess::finished, process, &QObject::deleteLater);
    if (kill) {
        process->kill();
    } else {
        process->closeWriteChannel();
        QTimer::singleShot(2000, process, [process] { process->kill(); });
    }
    if (!process->parent())
        process->deleteLater();
}

bool CoreClient::ensureStarted(QString* error)
{
    if (m_process && m_process->state() != QProcess::NotRunning)
        return true;
    if (!isConfigured()) {
        *error = m_config.program.isEmpty()
                     ? tr("Node.js was not found: install it or set PRISM_EXT_NODE to the absolute path of node.exe.")
                     : tr("The Prism Extended core is not configured: set PRISM_EXT_CORE to the path of src/ipc-server.mjs.");
        return false;
    }
    releaseProcess(true);
    m_buffer.clear();
    m_stderrTail.clear();
    m_process = new QProcess(this);
    m_process->setProcessChannelMode(QProcess::SeparateChannels);
    auto env = QProcessEnvironment::systemEnvironment();
    env.remove("NODE_OPTIONS");  // no injected --require/--import into the core
    m_process->setProcessEnvironment(env);
    connect(m_process, &QProcess::readyReadStandardOutput, this, &CoreClient::onReadyRead);
    connect(m_process, &QProcess::readyReadStandardError, this, [this] {
        m_stderrTail += m_process->readAllStandardError();
        if (m_stderrTail.size() > stderrTailBytes)
            m_stderrTail = m_stderrTail.right(stderrTailBytes);
    });
    connect(m_process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError) {
        failAll("E_CORE_UNAVAILABLE", withStderr(m_process->errorString()));
    });
    connect(m_process, &QProcess::finished, this, [this](int exitCode) {
        m_stderrTail += m_process->readAllStandardError();
        failAll("E_CORE_UNAVAILABLE", withStderr(tr("The core exited (code %1).").arg(exitCode)));
    });
    m_process->start(m_config.program, m_config.arguments);
    if (!m_process->waitForStarted(5000)) {
        *error = tr("The core could not be started: %1").arg(m_process->errorString());
        return false;
    }
    return true;
}

QString CoreClient::withStderr(const QString& message) const
{
    const QString tail = QString::fromUtf8(m_stderrTail).trimmed();
    return tail.isEmpty() ? message : QString("%1\n%2").arg(message, tail.right(1000));
}

void CoreClient::request(const QString& command, const QJsonObject& params, Callback callback, int timeoutMs)
{
    QString error;
    if (!ensureStarted(&error)) {
        QTimer::singleShot(0, this, [callback = std::move(callback), error] { callback(errorResponse("E_CORE_UNAVAILABLE", error)); });
        return;
    }
    const QString requestId = QString("q%1").arg(m_nextId++);
    auto* timer = new QTimer(this);
    timer->setSingleShot(true);
    // The core answers in order: a request it is still working on blocks every later one, so a timeout
    // restarts the core instead of leaving the queue stuck behind it.
    connect(timer, &QTimer::timeout, this, [this] { reset("E_TIMEOUT", tr("The core did not answer in time and was restarted.")); });
    m_pending.insert(requestId, { std::move(callback), timer });
    timer->start(timeoutMs < 0 ? m_config.requestTimeoutMs : timeoutMs);
    const QJsonObject envelope{
        { "protocolVersion", 1 }, { "requestId", requestId }, { "command", command }, { "params", params }, { "dryRun", true }
    };
    m_process->write(QJsonDocument(envelope).toJson(QJsonDocument::Compact) + '\n');
}

void CoreClient::onReadyRead()
{
    QPointer<CoreClient> self(this);
    m_buffer += m_process->readAllStandardOutput();
    for (qsizetype newline = m_buffer.indexOf('\n'); newline >= 0; newline = m_buffer.indexOf('\n')) {
        const QByteArray line = m_buffer.left(newline).trimmed();
        m_buffer.remove(0, newline + 1);
        if (line.isEmpty())
            continue;
        if (line.size() > m_config.maxResponseBytes) {
            reset("E_RESPONSE_TOO_LARGE", tr("The core sent a response larger than the client accepts."));
            return;
        }
        QJsonParseError parseError;
        const auto document = QJsonDocument::fromJson(line, &parseError);
        const QString requestId = document.object().value("requestId").toString();
        if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
            // stdout carries protocol lines only; anything else means the stream can no longer be trusted.
            reset("E_PROTOCOL", tr("The core wrote something that is not a protocol message."));
            return;
        }
        if (!requestId.isEmpty())
            deliver(requestId, document.object());  // a line without one of our ids answers nothing
        if (!self)
            return;  // a callback destroyed the client
    }
    if (m_buffer.size() > m_config.maxResponseBytes)
        reset("E_RESPONSE_TOO_LARGE", tr("The core sent a response larger than the client accepts."));
}

void CoreClient::deliver(const QString& requestId, const QJsonObject& response)
{
    auto it = m_pending.find(requestId);
    if (it == m_pending.end())
        return;
    const Pending pending = it.value();
    m_pending.erase(it);
    pending.timer->stop();
    pending.timer->deleteLater();
    pending.callback(response);
}

bool CoreClient::failAll(const QString& code, const QString& message)
{
    const auto pending = std::exchange(m_pending, {});
    for (const auto& entry : pending) {  // timers first, while this client certainly exists
        entry.timer->stop();
        entry.timer->deleteLater();
    }
    QPointer<CoreClient> self(this);
    for (const auto& entry : pending) {
        entry.callback(errorResponse(code, message));
        if (!self)
            return false;
    }
    return true;
}

void CoreClient::reset(const QString& code, const QString& message)
{
    if (m_process)
        m_process->disconnect(this);  // its finished/error signals must not fail requests made after the reset
    QProcess* process = std::exchange(m_process, nullptr);
    if (!failAll(code, message)) {
        // A callback destroyed this client; the process is no longer ours to track but must still go away.
        if (process) {
            process->kill();
            process->deleteLater();
        }
        return;
    }
    m_process = process;
    releaseProcess(true);
}
