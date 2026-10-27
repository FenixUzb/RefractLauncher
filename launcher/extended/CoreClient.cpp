// SPDX-License-Identifier: GPL-3.0-only
/*
 *  Prism Extended (a fork of Prism Launcher; not affiliated with or endorsed by the Prism Launcher project)
 */
#include "CoreClient.h"

#include <QJsonDocument>
#include <QProcess>
#include <QProcessEnvironment>
#include <QTimer>
#include <utility>

CoreClient::Config CoreClient::configFromEnvironment()
{
    const auto env = QProcessEnvironment::systemEnvironment();
    Config config;
    config.program = env.value("PRISM_EXT_NODE", "node");
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
    for (auto& pending : m_pending)
        delete pending.timer;
    m_pending.clear();
    if (m_process) {
        m_process->disconnect(this);
        m_process->closeWriteChannel();  // the core exits on stdin EOF
        if (!m_process->waitForFinished(1000)) {
            m_process->kill();
            m_process->waitForFinished(1000);
        }
    }
}

bool CoreClient::ensureStarted(QString* error)
{
    if (m_process && m_process->state() != QProcess::NotRunning)
        return true;
    if (!isConfigured()) {
        *error = tr("The Prism Extended core is not configured: set PRISM_EXT_CORE to the path of src/ipc-server.mjs.");
        return false;
    }
    if (m_process) {
        m_process->disconnect(this);
        m_process->deleteLater();
    }
    m_buffer.clear();
    m_process = new QProcess(this);
    m_process->setProcessChannelMode(QProcess::SeparateChannels);
    m_process->setStandardErrorFile(QProcess::nullDevice());  // the core's log is not protocol traffic
    connect(m_process, &QProcess::readyReadStandardOutput, this, &CoreClient::onReadyRead);
    connect(m_process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError) {
        failAll("E_CORE_UNAVAILABLE", m_process->errorString());
    });
    connect(m_process, &QProcess::finished, this,
            [this](int exitCode) { failAll("E_CORE_UNAVAILABLE", tr("The core exited (code %1).").arg(exitCode)); });
    m_process->start(m_config.program, m_config.arguments);
    if (!m_process->waitForStarted(5000)) {
        *error = tr("The core could not be started: %1").arg(m_process->errorString());
        return false;
    }
    return true;
}

void CoreClient::request(const QString& command, const QJsonObject& params, Callback callback)
{
    QString error;
    if (!ensureStarted(&error)) {
        QTimer::singleShot(0, this, [callback = std::move(callback), error] { callback(errorResponse("E_CORE_UNAVAILABLE", error)); });
        return;
    }
    const QString requestId = QString("q%1").arg(m_nextId++);
    auto* timer = new QTimer(this);
    timer->setSingleShot(true);
    connect(timer, &QTimer::timeout, this,
            [this, requestId] { deliver(requestId, errorResponse("E_TIMEOUT", tr("The core did not answer in time."))); });
    m_pending.insert(requestId, { std::move(callback), timer });
    timer->start(m_config.requestTimeoutMs);
    const QJsonObject envelope{
        { "protocolVersion", 1 }, { "requestId", requestId }, { "command", command }, { "params", params }, { "dryRun", true }
    };
    m_process->write(QJsonDocument(envelope).toJson(QJsonDocument::Compact) + '\n');
}

void CoreClient::onReadyRead()
{
    m_buffer += m_process->readAllStandardOutput();
    for (qsizetype newline = m_buffer.indexOf('\n'); newline >= 0; newline = m_buffer.indexOf('\n')) {
        const QByteArray line = m_buffer.left(newline).trimmed();
        m_buffer.remove(0, newline + 1);
        QJsonParseError parseError;
        const auto document = QJsonDocument::fromJson(line, &parseError);
        const QString requestId = document.object().value("requestId").toString();
        // A line without one of our ids (e.g. the core rejecting a line we never sent) answers nothing.
        if (parseError.error == QJsonParseError::NoError && document.isObject() && !requestId.isEmpty())
            deliver(requestId, document.object());
    }
    if (m_buffer.size() > m_config.maxResponseBytes) {
        m_buffer.clear();
        failAll("E_RESPONSE_TOO_LARGE", tr("The core sent a response larger than the client accepts."));
        m_process->kill();
    }
}

void CoreClient::deliver(const QString& requestId, const QJsonObject& response)
{
    auto it = m_pending.find(requestId);
    if (it == m_pending.end())
        return;
    Pending pending = it.value();
    m_pending.erase(it);
    pending.timer->stop();
    pending.timer->deleteLater();
    pending.callback(response);
}

void CoreClient::failAll(const QString& code, const QString& message)
{
    const auto pending = std::exchange(m_pending, {});
    for (const auto& entry : pending) {
        entry.timer->stop();
        entry.timer->deleteLater();
        entry.callback(errorResponse(code, message));
    }
}
