/*
 * Copyright (c) Contributors to the Open 3D Engine Project.
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 */
#pragma once

#include "LocalMcpToolProvider.h"
#include <AzCore/Debug/TraceMessageBus.h>
#include <AzCore/Settings/SettingsRegistry.h>
#include <AzCore/std/containers/deque.h>
#include <AzCore/std/containers/vector.h>
#include <AzCore/std/containers/unordered_map.h>
#include <AzCore/std/string/regex.h>
#include <AzCore/std/chrono/chrono.h>
#include <AzCore/JSON/document.h>
#include <AzCore/std/parallel/mutex.h>
#include <AzCore/std/smart_ptr/unique_ptr.h>
#include <AzNetworking/TcpTransport/TcpSocket.h>
#include <AzNetworking/Utilities/EncryptionCommon.h>

namespace LocalMcp
{
    inline constexpr const char* SettingsRoot = "/O3DE/Gems/LocalMcp";

    struct Config
    {
        bool m_enabled = false;
        bool m_loopbackOnly = false; // Set by the editor owner; never a remote-enable setting.
        AZ::s64 m_port = 4600;
        AZ::s64 m_logLines = 512;
        AZ::s64 m_idleTimeoutMs = 10000;
        AZStd::string m_auth = "none";
        AZStd::string m_username;
        AZStd::string m_password;
        AZStd::string m_certificate;
        AZStd::string m_privateKey;
        AZStd::string m_ca;
    };

    bool ReadConfig(AZ::SettingsRegistryInterface& registry, Config& config,
        AZStd::string_view root = SettingsRoot, const Config& defaults = Config{});

    //! Poll only on the main thread: console functors may touch engine state.
    class Server : private AZ::Debug::TraceMessageBus::Handler
    {
    public:
        explicit Server(ToolProvider* provider = nullptr);
        ~Server();
        bool Start(const Config& config);
        void Stop();
        void Poll();
        uint16_t GetPort() const;

    private:
        friend class LocalMcpTests;
        struct Client;
        struct LogLine
        {
            uint64_t m_sequence;
            AZStd::string m_window;
            AZStd::string m_message;
            AZStd::string m_level = "info";
            AZStd::string m_tag;
        };

        struct Source
        {
            uint64_t m_total = 0;
            uint64_t m_ring = 0;
            int64_t m_lastTime = 0;
            uint64_t m_levels[3]{};
        };
        struct Wait
        {
            AZStd::regex m_pattern;
            AZStd::vector<AZStd::string> m_windows;
            bool m_filterWindows = false;
            uint64_t m_next = 0;
            uint64_t m_dropped = 0;
            AZStd::chrono::steady_clock::time_point m_until;
            LogLine m_match{};
        };
        struct Capture
        {
            Server* m_server = nullptr;
            AZStd::vector<LogLine> m_lines;
            uint64_t m_truncated = 0;
        };
        static thread_local Capture* s_capture;
        static thread_local const char* s_level;
        bool HandleLogTool(AZStd::string_view name, const rapidjson::Value& args, rapidjson::Value& data,
            rapidjson::Document::AllocatorType& allocator, AZStd::string& error, uint64_t* waitId);
        void SearchConsole(const rapidjson::Value& args, rapidjson::Value& data,
            rapidjson::Document::AllocatorType& allocator);
        AZStd::string HandleHttp(AZStd::string_view request, Client* client = nullptr);
        AZStd::string HandleRpc(AZStd::string_view body, int& status, uint64_t* waitId = nullptr,
            bool* providerPending = nullptr);
        void ReleasePending(Client& client);
        bool OnOutput(const char* window, const char* message) override;
        bool OnPreWarning(const char*, const char*, int, const char*, const char*) override { s_level = "warning"; return false; }
        bool OnWarning(const char*, const char*) override { s_level = "info"; return false; }
        bool OnPreError(const char*, const char*, int, const char*, const char*) override { s_level = "error"; return false; }
        bool OnError(const char*, const char*) override { s_level = "info"; return false; }
        bool OnPreAssert(const char*, int, const char*, const char*) override { s_level = "error"; return false; }
        bool OnAssert(const char*) override { s_level = "info"; return false; }
        bool OnPrintf(const char*, const char*) override { s_level = "info"; return false; }

        class Listener final : public AzNetworking::TcpSocket
        {
        public:
            bool ListenLoopback(uint16_t port);
        };
        // Synchronous borrow: the owning component destroys Server before its provider.
        ToolProvider* m_provider = nullptr;
        Listener m_listener;
        AZStd::vector<AZStd::unique_ptr<Client>> m_clients;
        SSL_CTX* m_tlsContext = nullptr;
        AZStd::string m_authorization;
        uint16_t m_port = 0;
        bool m_socketLayerInitialized = false;
        AZStd::mutex m_logMutex;
        AZStd::deque<LogLine> m_logs;
        uint64_t m_sequence = 0;
        size_t m_logLines = 512;
        AZ::s64 m_idleTimeoutMs = 10000;
        AZStd::unordered_map<AZStd::string, Source> m_sources;
        AZStd::unordered_map<uint64_t, Wait> m_waits;
        uint64_t m_nextWait = 0;
    };
}
