// SPDX-License-Identifier: GPL-3.0-only
/*
 *  Refract Launcher (a fork of Prism Launcher; not affiliated with or endorsed by the Prism Launcher project)
 *
 *  Client for the read-only Refract core: newline-delimited JSON over stdio, protocol v1
 *  (see the DevKit's schemas/ipc-envelope.schema.json and schemas/ipc-response.schema.json).
 */
#pragma once

#include <QJsonObject>
#include <QMap>
#include <QObject>
#include <QProcessEnvironment>
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
    /** discover() for this executable's folder and the process environment. */
    static Config configFromEnvironment();
    /**
     * Where the core is. PRISM_EXT_NODE / PRISM_EXT_CORE win when set (development). Otherwise the bundle next to
     * the executable: `<appDir>/refract-core/src/ipc-server.mjs`, run by `<appDir>/refract-core/node[.exe]` when
     * present, else by "node" resolved on PATH to an absolute path (never looked up in the current directory).
     * No core found = empty arguments, which the bridge reports as not configured.
     */
    static Config discover(const QString& appDir, const QProcessEnvironment& env);
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
    /** Fails every pending request; stops early if a callback destroys this client. */
    void failAll(const QString& code, const QString& message);
    /** Fails everything and kills the process; the next request starts a new one. */
    void reset(const QString& code, const QString& message);
    void releaseProcess(bool kill);
    QString withStderr(const QString& message) const;

    Config m_config;
    QProcess* m_process = nullptr;
    QByteArray m_buffer;
    QByteArray m_stderrTail;
    QMap<quint64, Pending> m_pending;  // ordered by request number: failures are reported in request order
    quint64 m_nextId = 1;
};
