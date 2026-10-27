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
 * the pipes, so no other local program can talk to the core. A missing, crashed, hung or misbehaving core
 * becomes an error response (E_CORE_UNAVAILABLE, E_TIMEOUT, E_PROTOCOL, E_RESPONSE_TOO_LARGE), never a
 * launcher crash; after a timeout or protocol error the process is killed and the next request starts a fresh
 * one. Callbacks run on the GUI thread at most once each, and never after the client is destroyed.
 * Destroying the client never blocks: a busy core is killed, an idle one gets EOF and exits by itself.
 */
class CoreClient : public QObject {
    Q_OBJECT
   public:
    struct Config {
        QString program;        // absolute path of the node executable
        QStringList arguments;  // path to the core's src/ipc-server.mjs
        int requestTimeoutMs = 30000;
        qsizetype maxResponseBytes = qsizetype(64) * 1024 * 1024;
    };
    /**
     * PRISM_EXT_NODE (default: "node" resolved on PATH to an absolute path, never looked up in the current
     * directory) and PRISM_EXT_CORE (path to ipc-server.mjs; unset = not configured).
     */
    static Config configFromEnvironment();
    static QJsonObject errorResponse(const QString& code, const QString& message);

    using Callback = std::function<void(const QJsonObject& response)>;

    explicit CoreClient(Config config, QObject* parent = nullptr);
    ~CoreClient() override;

    /** `timeoutMs` < 0 uses Config::requestTimeoutMs. */
    void request(const QString& command, const QJsonObject& params, Callback callback, int timeoutMs = -1);
    bool isConfigured() const { return !m_config.program.isEmpty() && !m_config.arguments.isEmpty(); }

   private:
    struct Pending {
        Callback callback;
        QTimer* timer;
    };
    bool ensureStarted(QString* error);
    void onReadyRead();
    void deliver(const QString& requestId, const QJsonObject& response);
    /** Fails every pending request; returns false if a callback destroyed this client. */
    bool failAll(const QString& code, const QString& message);
    /** Fails everything and kills the process; the next request starts a new one. */
    void reset(const QString& code, const QString& message);
    void releaseProcess(bool kill);
    QString withStderr(const QString& message) const;

    Config m_config;
    QProcess* m_process = nullptr;
    QByteArray m_buffer;
    QByteArray m_stderrTail;
    QHash<QString, Pending> m_pending;
    quint64 m_nextId = 1;
};
