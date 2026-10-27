// SPDX-License-Identifier: GPL-3.0-only
/*
 *  Prism Extended (a fork of Prism Launcher; not affiliated with or endorsed by the Prism Launcher project)
 *
 *  Client for the read-only Prism Extended core: newline-delimited JSON over stdio, protocol v1
 *  (see the DevKit's schemas/ipc-envelope.schema.json and schemas/ipc-response.schema.json).
 */
#pragma once

#include <QHash>
#include <QJsonObject>
#include <QObject>
#include <QStringList>
#include <functional>

class QProcess;
class QTimer;

/**
 * One core child process per client, one request per line, one response per line. Only this process holds
 * the pipes, so no other local program can talk to the core. A missing, crashed or hung core becomes an error
 * response (E_CORE_UNAVAILABLE, E_TIMEOUT, E_RESPONSE_TOO_LARGE), never a launcher crash, and the core exits
 * by itself when its stdin closes. Callbacks run on the GUI thread, exactly once per request.
 */
class CoreClient : public QObject {
    Q_OBJECT
   public:
    struct Config {
        QString program;        // node executable
        QStringList arguments;  // path to the core's src/ipc-server.mjs
        int requestTimeoutMs = 30000;
        qsizetype maxResponseBytes = 64 * 1024 * 1024;
    };
    /** PRISM_EXT_NODE (default "node") and PRISM_EXT_CORE (path to ipc-server.mjs; unset = not configured). */
    static Config configFromEnvironment();
    static QJsonObject errorResponse(const QString& code, const QString& message);

    using Callback = std::function<void(const QJsonObject& response)>;

    explicit CoreClient(Config config, QObject* parent = nullptr);
    ~CoreClient() override;

    void request(const QString& command, const QJsonObject& params, Callback callback);
    bool isConfigured() const { return !m_config.program.isEmpty() && !m_config.arguments.isEmpty(); }

   private:
    struct Pending {
        Callback callback;
        QTimer* timer;
    };
    bool ensureStarted(QString* error);
    void onReadyRead();
    void deliver(const QString& requestId, const QJsonObject& response);
    void failAll(const QString& code, const QString& message);

    Config m_config;
    QProcess* m_process = nullptr;
    QByteArray m_buffer;
    QHash<QString, Pending> m_pending;
    quint64 m_nextId = 1;
};
