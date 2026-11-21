/*
 * Copyright (c) Contributors to the Open 3D Engine Project.
 * For complete copyright and license terms please see the LICENSE at the root of this distribution.
 *
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 *
 */

#pragma once

#include "LocalMcpToolProvider.h"

#include <Source/AutoGen/LocalMcpRuntimeTools.McpTools.h>

#include <AzCore/Debug/TraceMessageBus.h>
#include <AzCore/JSON/document.h>
#include <AzCore/Preprocessor/Enum.h>
#include <AzCore/Settings/SettingsRegistry.h>
#include <AzCore/std/chrono/chrono.h>
#include <AzCore/std/containers/deque.h>
#include <AzCore/std/containers/unordered_map.h>
#include <AzCore/std/containers/vector.h>
#include <AzCore/std/parallel/mutex.h>
#include <AzCore/std/smart_ptr/unique_ptr.h>
#include <AzCore/std/string/regex.h>
#include <AzNetworking/TcpTransport/TcpSocket.h>
#include <AzNetworking/Utilities/EncryptionCommon.h>

namespace LocalMcp
{
    //! Settings registry path of the runtime endpoint's settings.
    inline constexpr const char* SettingsRoot = "/O3DE/Gems/LocalMcp";

    // Values of Config::m_auth.
    inline constexpr const char* AuthNone = "none";
    inline constexpr const char* AuthBasic = "basic";
    inline constexpr const char* AuthMutualTls = "mtls";

    // Accepted ranges of the numeric settings.
    inline constexpr AZ::s64 MaxPort = 65535;
    inline constexpr AZ::s64 MinLogLines = 1;
    inline constexpr AZ::s64 MaxLogLines = 1048576;
    inline constexpr AZ::s64 MinIdleTimeoutMs = 1000;
    inline constexpr AZ::s64 MaxIdleTimeoutMs = 3600000;

    // Defaults of the numeric settings.
    inline constexpr AZ::s64 DefaultPort = 4600;
    inline constexpr AZ::s64 DefaultLogLines = 512;
    inline constexpr AZ::s64 DefaultIdleTimeoutMs = 10000;

    // HTTP request limits. Larger requests are refused with HttpStatus::ContentTooLarge.
    inline constexpr size_t MaxHeaderSize = 8192;
    inline constexpr size_t MaxBodySize = 65536;

    //! HTTP status codes the endpoint replies with.
    AZ_ENUM_CLASS_WITH_UNDERLYING_TYPE(
        HttpStatus,
        AZ::u16,
        (Ok, 200),
        (Accepted, 202),
        (BadRequest, 400),
        (Unauthorized, 401),
        (Forbidden, 403),
        (NotFound, 404),
        (MethodNotAllowed, 405),
        (ContentTooLarge, 413),
        (UnsupportedMediaType, 415));

    //! JSON-RPC 2.0 error codes.
    AZ_ENUM_CLASS_WITH_UNDERLYING_TYPE(
        JsonRpcError, int, (ParseError, -32700), (InvalidRequest, -32600), (MethodNotFound, -32601), (InvalidParams, -32602));

    //! Endpoint settings, read by ReadConfig().
    struct Config
    {
        bool m_enabled = false;
        //! Bind to 127.0.0.1 only. Set by the editor owner; never read from settings, so it cannot be turned off remotely.
        bool m_loopbackOnly = false;
        //! TCP port to listen on; 0 picks a free port.
        AZ::s64 m_port = DefaultPort;
        //! Number of trace lines kept in the log ring.
        AZ::s64 m_logLines = DefaultLogLines;
        //! A connection with no traffic for this long is closed.
        AZ::s64 m_idleTimeoutMs = DefaultIdleTimeoutMs;
        //! One of AuthNone, AuthBasic or AuthMutualTls.
        AZStd::string m_auth = AuthNone;
        //! Basic authentication credentials.
        AZStd::string m_username;
        AZStd::string m_password;
        //! Mutual TLS: PEM server certificate chain, its private key, and the CA that client certificates must chain to.
        AZStd::string m_certificate;
        AZStd::string m_privateKey;
        AZStd::string m_ca;
    };

    //! Reads config from the settings under root, starting from defaults.
    //! A ConfigFile setting names an .ini file that is merged under root first.
    //! Returns false, with a warning, when a setting has the wrong type or is out of range.
    bool ReadConfig(
        AZ::SettingsRegistryInterface& registry, Config& config, AZStd::string_view root = SettingsRoot, const Config& defaults = Config{});

    //! Local MCP endpoint: JSON-RPC over HTTP/1.1, serving AZ console and log tools plus an optional ToolProvider's tools.
    //! Captures trace output into a bounded log ring from any thread.
    //! Start, Stop and Poll run on the main thread only, because console functors may touch engine state.
    class Server
        : private AZ::Debug::TraceMessageBus::Handler
        , private RuntimeToolsHandler
    {
    public:
        //! provider is optional and borrowed; the owner destroys the Server before the provider.
        explicit Server(ToolProvider* provider = nullptr);
        ~Server();

        //! Stops, then listens with config. Returns false and stays stopped on invalid config or a bind or TLS failure.
        bool Start(const Config& config);

        //! Closes every connection, cancelling pending work, closes the listener and clears the log history.
        void Stop();

        //! Accepts connections and advances every request without blocking.
        void Poll();

        //! Bound port, or 0 when stopped.
        uint16_t GetPort() const;

    private:
        friend class LocalMcpTests;

        struct Client;

        // Trace severities, as reported in log lines.
        static constexpr const char* ErrorLevel = "error";
        static constexpr const char* WarningLevel = "warning";
        static constexpr const char* InfoLevel = "info";

        //! One captured trace line.
        struct LogLine
        {
            uint64_t m_sequence;
            AZStd::string m_window;
            AZStd::string m_message;
            AZStd::string m_level = InfoLevel;
            AZStd::string m_tag;
        };

        //! Counters for one trace window.
        struct Source
        {
            //! Lines since startup.
            uint64_t m_total = 0;
            //! Lines still in the ring.
            uint64_t m_ring = 0;
            //! Unix time of the latest line, in milliseconds.
            int64_t m_lastTimeMs = 0;
            //! Lines since startup by severity.
            uint64_t m_errorCount = 0;
            uint64_t m_warningCount = 0;
            uint64_t m_infoCount = 0;
        };

        //! A logs_wait request held open on its connection until a match or its deadline.
        struct Wait
        {
            AZStd::regex m_pattern;
            AZStd::vector<AZStd::string> m_windows;
            bool m_filterWindows = false;
            //! Cursor reported back: the last line examined.
            uint64_t m_next = 0;
            uint64_t m_dropped = 0;
            AZStd::chrono::steady_clock::time_point m_until;
            //! The first matching line; m_sequence stays 0 until one is found.
            LogLine m_match{};
        };

        //! Lines printed by the console command that is running on this thread.
        struct Capture
        {
            Server* m_server = nullptr;
            AZStd::vector<LogLine> m_lines;
            uint64_t m_truncated = 0;
        };

        //! Binds the loopback interface only, unlike TcpSocket::Listen().
        class Listener final : public AzNetworking::TcpSocket
        {
        public:
            bool ListenLoopback(uint16_t port);
        };

        // AZ::Debug::TraceMessageBus::Handler overrides
        bool OnOutput(const char* window, const char* message) override;
        bool OnPreWarning(const char* window, const char* fileName, int line, const char* func, const char* message) override;
        bool OnWarning(const char* window, const char* message) override;
        bool OnPreError(const char* window, const char* fileName, int line, const char* func, const char* message) override;
        bool OnError(const char* window, const char* message) override;
        bool OnPreAssert(const char* fileName, int line, const char* func, const char* message) override;
        bool OnAssert(const char* message) override;
        bool OnPrintf(const char* window, const char* message) override;

        // Connections
        //! Advances one connection. Returns false when it is finished and must be closed.
        bool ServiceClient(Client& client);
        //! Cancels the connection's pending provider operation and drops its log wait.
        void ReleasePending(Client& client);

        // Request handling
        //! Returns the full HTTP response, or an empty string while the request is incomplete or held open.
        AZStd::string HandleHttp(AZStd::string_view request, Client* client = nullptr);
        //! Returns the JSON-RPC response body, or an empty string for a notification or a held request.
        AZStd::string HandleRpc(AZStd::string_view body, HttpStatus& status, uint64_t* waitId = nullptr, bool* providerPending = nullptr);

        // RuntimeToolsHandler overrides. Console tools report an unavailable console as data.error.
        bool HandleConsoleComplete(const ConsoleCompleteArgs& args, ToolCall& call) override;
        bool HandleConsoleExecute(const ConsoleExecuteArgs& args, ToolCall& call) override;
        bool HandleConsoleSearch(const ConsoleSearchArgs& args, ToolCall& call) override;
        bool HandleLogTag(const LogTagArgs& args, ToolCall& call) override;
        bool HandleLogTags(const LogTagsArgs& args, ToolCall& call) override;
        bool HandleLogsSources(const LogsSourcesArgs& args, ToolCall& call) override;
        bool HandleLogsTail(const LogsTailArgs& args, ToolCall& call) override;
        //! Sets *call.m_waitId while the wait is held, and leaves call.m_data empty until it completes.
        bool HandleLogsWait(const LogsWaitArgs& args, ToolCall& call) override;

        //! Sequence of the oldest line in the ring. Requires m_logMutex.
        uint64_t GetFirstSequence() const;
        static rapidjson::Value LineToJson(const LogLine& line, rapidjson::Document::AllocatorType& allocator);

        //! Capture of the console command running on this thread, if any.
        static thread_local Capture* s_capture;
        //! Severity of the trace message being printed on this thread, set by the OnPre* handlers.
        static thread_local const char* s_level;

        //! Borrowed; see the constructor.
        ToolProvider* m_provider = nullptr;

        // Transport
        Listener m_listener;
        AZStd::vector<AZStd::unique_ptr<Client>> m_clients;
        SSL_CTX* m_tlsContext = nullptr;
        //! Expected Authorization header value for basic authentication; empty when not required.
        AZStd::string m_authorization;
        uint16_t m_port = 0;
        bool m_socketLayerInitialized = false;
        AZ::s64 m_idleTimeoutMs = DefaultIdleTimeoutMs;

        // Log history, guarded by m_logMutex because traces arrive from any thread
        AZStd::mutex m_logMutex;
        AZStd::deque<LogLine> m_logs;
        uint64_t m_sequence = 0;
        size_t m_logLines = static_cast<size_t>(DefaultLogLines);
        AZStd::unordered_map<AZStd::string, Source> m_sources;
        AZStd::unordered_map<uint64_t, Wait> m_waits;
        uint64_t m_nextWait = 0;
    };
} // namespace LocalMcp
