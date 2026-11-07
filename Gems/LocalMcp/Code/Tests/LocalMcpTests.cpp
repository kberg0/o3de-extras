/*
 * Copyright (c) Contributors to the Open 3D Engine Project.
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 */
#include "LocalMcpServer.h"
#include <AzCore/Console/Console.h>
#include <AzCore/Console/LoggerSystemComponent.h>
#include <AzCore/IO/Path/Path.h>
#include <AzCore/Settings/SettingsRegistryImpl.h>
#include <AzCore/UnitTest/TestTypes.h>
#include <AzCore/UnitTest/UnitTest.h>
#include <AzCore/std/parallel/thread.h>
#include <AzTest/AzTest.h>
#include <AzTest/Utils.h>
#include <AzNetworking/Utilities/NetworkIncludes.h>
#include <AzCore/JSON/document.h>
#include <openssl/ssl.h>
#include <openssl/pem.h>
#include <openssl/x509v3.h>

AZ_UNIT_TEST_HOOK(DEFAULT_UNIT_TEST_ENV);

namespace LocalMcp
{
    class LocalMcpTests : public UnitTest::LeakDetectionFixture
    {
    public:
        void SetUp() override
        {
            m_savedSuppressPrintf = UnitTest::TestRunner::Instance().m_suppressPrintf;
            // Printf must reach OnOutput; the test runner still suppresses final OS output.
            UnitTest::TestRunner::Instance().m_suppressPrintf = false;
            m_console = AZStd::make_unique<AZ::Console>();
            AZ::Interface<AZ::IConsole>::Register(m_console.get());
            m_logger = AZStd::make_unique<AZ::LoggerSystemComponent>();
            m_console->LinkDeferredFunctors(AZ::ConsoleFunctorBase::GetDeferredHead());
            m_server = AZStd::make_unique<Server>();
        }
        void TearDown() override
        {
            m_server.reset();
            m_logger.reset();
            AZ::Interface<AZ::IConsole>::Unregister(m_console.get());
            m_console.reset();
            UnitTest::TestRunner::Instance().m_suppressPrintf = m_savedSuppressPrintf;
        }
        AZStd::string Http(AZStd::string_view body, AZStd::string_view headers = {})
        {
            auto request = AZStd::string::format("POST /mcp HTTP/1.1\r\nHost: localhost:%u\r\nContent-Type: application/json\r\n"
                "Content-Length: %zu\r\n", m_server->GetPort(), body.size());
            request += headers;
            request += "\r\n";
            request += body;
            return request;
        }
        AZStd::string Handle(AZStd::string_view request)
        {
            return m_server->HandleHttp(request);
        }
        rapidjson::Document Rpc(AZStd::string_view request)
        {
            int status = 200;
            const auto response = m_server->HandleRpc(request, status);
            rapidjson::Document result;
            result.Parse(response.c_str());
            EXPECT_FALSE(result.HasParseError());
            return result;
        }
        rapidjson::Document Call(const char* name, AZStd::string_view args = "{}")
        {
            auto response = Rpc(AZStd::string::format(
                R"({"jsonrpc":"2.0","id":1,"method":"tools/call","params":{"name":"%s","arguments":%.*s}})",
                name, aznumeric_cast<int>(args.size()), args.data()));
            rapidjson::Document data;
            if (response.HasMember("result"))
            {
                data.Parse(response["result"]["content"][0]["text"].GetString());
            }
            else
            {
                data.CopyFrom(response, data.GetAllocator());
            }
            return data;
        }
        void Log(const char* message)
        {
            m_server->OnOutput("test", message);
        }
        void Capacity(size_t lines) { m_server->m_logLines = lines; }
        void ExpectLoopbackBound()
        {
            sockaddr_in address{};
            socklen_t length = sizeof(address);
            ASSERT_EQ(getsockname(static_cast<int>(m_server->m_listener.GetSocketFd()),
                reinterpret_cast<sockaddr*>(&address), &length), 0);
            EXPECT_EQ(ntohl(address.sin_addr.s_addr), INADDR_LOOPBACK);
        }
        AZStd::string WaitRpc(const char* args, uint64_t& id)
        {
            int status = 200;
            return m_server->HandleRpc(AZStd::string::format(
                R"({"jsonrpc":"2.0","id":10,"method":"tools/call","params":{"name":"logs_wait","arguments":%s}})", args), status, &id);
        }
        void Severity(const char* level)
        {
            if (AZStd::string_view(level) == "error")
            {
                m_server->OnPreError("test", "", 0, "", "");
            }
            else if (AZStd::string_view(level) == "warning")
            {
                m_server->OnPreWarning("test", "", 0, "", "");
            }
            else
            {
                m_server->OnPrintf("test", "");
            }
        }
        Config EphemeralConfig()
        {
            Config config;
            config.m_port = 0;
            return config;
        }

        AZStd::string Exchange(const AZStd::string& request, SSL_CTX* context = nullptr, bool* closed = nullptr)
        {
            if (closed)
            {
                *closed = false;
            }
            AzNetworking::TcpSocket socket;
            if (!socket.Connect(AzNetworking::IpAddress(127, 0, 0, 1, m_server->GetPort()), 0))
            {
                ADD_FAILURE() << "Client connect failed";
                return {};
            }
            SSL* ssl = context ? SSL_new(context) : nullptr;
            if (ssl)
            {
                SSL_set_fd(ssl, static_cast<int>(socket.GetSocketFd()));
            }
            AZStd::string response;
            size_t sent = 0;
            const auto deadline = AZStd::chrono::steady_clock::now() + AZStd::chrono::seconds(3);
            while (AZStd::chrono::steady_clock::now() < deadline)
            {
                m_server->Poll();
                if (ssl && !SSL_is_init_finished(ssl))
                {
                    const int result = SSL_connect(ssl);
                    const int error = result == 1 ? SSL_ERROR_NONE : SSL_get_error(ssl, result);
                    if (error != SSL_ERROR_NONE && error != SSL_ERROR_WANT_READ && error != SSL_ERROR_WANT_WRITE)
                    {
                        break;
                    }
                }
                else if (sent < request.size())
                {
                    // Fragment requests to exercise incremental HTTP parsing.
                    const auto count = aznumeric_cast<uint32_t>(AZStd::min<size_t>(17, request.size() - sent));
                    const int result = ssl ? SSL_write(ssl, request.data() + sent, count) :
                        socket.Send(reinterpret_cast<const uint8_t*>(request.data() + sent), count);
                    if (result > 0)
                    {
                        sent += result;
                    }
                    else if (!ssl || (SSL_get_error(ssl, result) != SSL_ERROR_WANT_READ &&
                        SSL_get_error(ssl, result) != SSL_ERROR_WANT_WRITE))
                    {
                        break;
                    }
                }
                else
                {
                    uint8_t buffer[4096];
                    const int result = ssl ? SSL_read(ssl, buffer, sizeof(buffer)) : socket.Receive(buffer, sizeof(buffer));
                    if (result > 0)
                    {
                        response.append(reinterpret_cast<char*>(buffer), result);
                    }
                    else if (ssl ? (SSL_get_error(ssl, result) != SSL_ERROR_WANT_READ &&
                        SSL_get_error(ssl, result) != SSL_ERROR_WANT_WRITE) : result < 0)
                    {
                        if (closed)
                        {
                            *closed = true;
                        }
                        break;
                    }
                }
                AZStd::this_thread::sleep_for(AZStd::chrono::milliseconds(1));
            }
            SSL_free(ssl);
            return response;
        }

        bool m_savedSuppressPrintf = true;
        AZStd::unique_ptr<AZ::Console> m_console;
        AZStd::unique_ptr<AZ::LoggerSystemComponent> m_logger;
        AZStd::unique_ptr<Server> m_server;
    };

    TEST_F(LocalMcpTests, InitializationToolsAndProtocolErrors)
    {
        auto initialized = Rpc(R"({"jsonrpc":"2.0","id":"init","method":"initialize","params":{
            "protocolVersion":"2025-11-25","capabilities":{},"clientInfo":{"name":"test","version":"1"}}})");
        EXPECT_STREQ(initialized["id"].GetString(), "init");
        EXPECT_STREQ(initialized["result"]["protocolVersion"].GetString(), "2025-11-25");
        EXPECT_TRUE(initialized["result"]["capabilities"].HasMember("tools"));
        auto tools = Rpc(R"({"jsonrpc":"2.0","id":2,"method":"tools/list"})");
        EXPECT_EQ(tools["result"]["tools"].Size(), 8);
        EXPECT_GT(initialized["result"]["serverInfo"]["process"]["pid"].GetInt(), 0);
        EXPECT_EQ(Rpc("{")["error"]["code"].GetInt(), -32700);
        EXPECT_EQ(Rpc("[]")["error"]["code"].GetInt(), -32600);
        EXPECT_EQ(Rpc(R"({"jsonrpc":"2.0","id":3,"method":"missing"})")["error"]["code"].GetInt(), -32601);
        EXPECT_EQ(Call("logs_tail", R"({"limit":0})")["error"]["code"].GetInt(), -32602);
        EXPECT_EQ(Call("console_execute", R"({"command":"x\u0000y"})")["error"]["code"].GetInt(), -32602);
        EXPECT_EQ(Call("log_tag", R"({"tag":"x","enabled":1})")["error"]["code"].GetInt(), -32602);
        EXPECT_EQ(Handle(Http(R"({"jsonrpc":"2.0","method":"notifications/initialized"})")).find("202 Accepted"), 9);
    }

    TEST_F(LocalMcpTests, ConsoleCompletionReadWriteAndTagControls)
    {
        AZ_CVAR_SCOPED(int, mcp_testValue, 7, nullptr, AZ::ConsoleFunctorFlags::Null, "Test value");
        AZ_CVAR_SCOPED(int, mcp_readOnly, 9, nullptr, AZ::ConsoleFunctorFlags::ReadOnly, "Read only");
        AZ_CVAR_SCOPED(AZ::CVarFixedString, mcp_string, "keep", nullptr, AZ::ConsoleFunctorFlags::Null, "String");
        m_console->LinkDeferredFunctors(AZ::ConsoleFunctorBase::GetDeferredHead());
        auto matches = Call("console_complete", R"({"prefix":"mcp_test"})");
        ASSERT_EQ(matches["matches"].Size(), 1);
        EXPECT_STREQ(matches["matches"][0]["name"].GetString(), "mcp_testValue");
        EXPECT_STREQ(matches["matches"][0]["value"].GetString(), "7");
        EXPECT_FALSE(Call("console_complete")["matches"].Empty());
        m_console->FindCommand("mcp_testValue")->SetArgumentAutoCompleteCallback(
            [](AZStd::string_view, AZStd::vector<AZStd::string>& arguments) { arguments.emplace_back("42"); });
        EXPECT_STREQ(Call("console_complete", R"({"prefix":"mcp_testValue 4"})")["completion"].GetString(), "mcp_testValue 42");
        EXPECT_STREQ(Call("console_execute", R"({"command":"mcp_testValue 42"})")["value"].GetString(), "42");
        EXPECT_EQ(static_cast<int>(mcp_testValue), 42);
        EXPECT_STREQ(Call("console_execute", R"({"command":"mcp_string"})")["value"].GetString(), "keep");
        EXPECT_EQ(static_cast<AZ::CVarFixedString>(mcp_string), "keep");
        EXPECT_STREQ(Call("console_execute", R"({"command":"mcp_readOnly"})")["value"].GetString(), "9");
        EXPECT_TRUE(Call("console_execute", R"({"command":"mcp_readOnly 10"})").HasMember("error"));
        EXPECT_TRUE(Call("console_execute", R"({"command":"does_not_exist"})").HasMember("error"));
        EXPECT_FALSE(m_console->ExecuteDeferredConsoleCommands());
        AZStd::string oversized = R"({"command":"mcp_testValue)";
        for (int i = 0; i < 70; ++i)
        {
            oversized += " x";
        }
        oversized += "\"}";
        EXPECT_EQ(Call("console_execute", oversized)["error"]["code"].GetInt(), -32602);
        EXPECT_TRUE(Call("log_tag", R"({"tag":"MCP_TEST","enabled":true})")["enabled"].GetBool());
        EXPECT_TRUE(m_logger->IsTagEnabled(AZ::TypeHash32("MCP_TEST")));
        EXPECT_FALSE(Call("log_tag", R"({"tag":"MCP_TEST","enabled":false})")["enabled"].GetBool());
    }

    TEST_F(LocalMcpTests, LogTailCursorsAndBoundedBuffer)
    {
        for (int i = 0; i < 520; ++i)
        {
            Log("line");
        }
        auto first = Call("logs_tail", R"({"after":0,"limit":2})");
        ASSERT_EQ(first["lines"].Size(), 2);
        EXPECT_EQ(first["dropped"].GetUint64(), 8);
        EXPECT_EQ(first["lines"][0]["sequence"].GetUint64(), 9);
        EXPECT_EQ(first["next"].GetUint64(), 10);
        auto second = Call("logs_tail", R"({"after":10,"limit":1})");
        EXPECT_EQ(second["lines"][0]["sequence"].GetUint64(), 11);
        EXPECT_EQ(Call("logs_tail")["lines"].Size(), 100);
        EXPECT_TRUE(Call("logs_tail", R"({"after":9999})").HasMember("error"));
        EXPECT_TRUE(Call("logs_tail", R"({"after":520})")["lines"].Empty());
        AZStd::string longLine(4096, 'x');
        Log(longLine.c_str());
        EXPECT_EQ(Call("logs_tail", R"({"limit":1})")["lines"][0]["message"].GetStringLength(), 2048);
        AZStd::string unicodeLine(2047, 'x');
        unicodeLine += "\xe2\x82\xac";
        Log(unicodeLine.c_str());
        EXPECT_EQ(Call("logs_tail", R"({"limit":1})")["lines"][0]["message"].GetStringLength(), 2047);
    }


    void CaptureOutput(const AZ::ConsoleCommandContainer&)
    {
        AZ_TracePrintf("command", "own line\n");
        AZStd::thread other([] { AZ_TracePrintf("background", "foreign line\n"); });
        other.join();
        for (int i = 0; i < 130; ++i)
        {
            AZ_TracePrintf("command", "repeated\n");
        }
    }
    AZ_CONSOLEFREEFUNC(CaptureOutput, AZ::ConsoleFunctorFlags::Null, "Output fixture. Second sentence.");

    TEST_F(LocalMcpTests, CommandCaptureIsThreadScopedBoundedAndWrittenOnce)
    {
        ASSERT_TRUE(m_server->Start(EphemeralConfig()));
        auto captured = Call("console_execute", R"({"command":"CaptureOutput"})");
        ASSERT_TRUE(captured.HasMember("executed"));
        ASSERT_TRUE(captured["executed"].GetBool());
        EXPECT_EQ(captured["output"].Size(), 128);
        EXPECT_GE(captured["truncated"].GetUint64(), 3);
        for (const auto& line : captured["output"].GetArray())
        {
            EXPECT_STRNE(line["window"].GetString(), "background");
        }
        const auto difference = captured["sequenceAfter"].GetUint64() - captured["sequenceBefore"].GetUint64();
        EXPECT_LT(difference, 140);
        EXPECT_TRUE(Call("console_execute", R"({"command":"CaptureOutput","output":"none"})")["output"].Empty());
        auto filtered = Call("console_execute", R"({"command":"CaptureOutput","grep":"own line"})");
        ASSERT_EQ(filtered["output"].Size(), 1);
        EXPECT_STREQ(filtered["output"][0]["message"].GetString(), "own line\n");
        EXPECT_TRUE(Call("console_execute", R"({"command":"CaptureOutput","grep":"["})").HasMember("error"));
        EXPECT_TRUE(Call("console_execute", R"({"command":"CaptureOutput","output":"bad"})").HasMember("error"));
    }

    TEST_F(LocalMcpTests, FiltersFieldsAndCursorScanAllUnmatchedLines)
    {
        Log("a.b");
        Severity("warning");
        Log("aXb");
        Severity("info");
        Log("noise");
        auto regex = Call("logs_tail", R"({"after":0,"pattern":"a.b"})");
        EXPECT_EQ(regex["lines"].Size(), 2);
        EXPECT_EQ(regex["next"].GetUint64(), 3);
        EXPECT_EQ(Call("logs_tail", R"({"after":0,"pattern":"a.b","regex":false})")["lines"].Size(), 1);
        EXPECT_EQ(Call("logs_tail", R"({"after":0,"windows":["missing"]})")["next"].GetUint64(), 3);
        EXPECT_TRUE(Call("logs_tail", R"({"after":0,"excludeWindows":["test"]})")["lines"].Empty());
        EXPECT_EQ(Call("logs_tail", R"({"after":0,"level":"warning"})")["lines"].Size(), 1);
        EXPECT_STREQ(Call("logs_tail", R"({"after":0,"limit":1,"fields":"message"})")["lines"][0].GetString(), "a.b");
        EXPECT_STREQ(Call("logs_tail", R"({"after":0,"limit":1,"fields":"compact"})")["lines"][0].GetString(), "1 test: a.b");
        for (const char* args : { R"({"pattern":"["})", R"({"windows":3})", R"({"windows":[1]})",
             R"({"level":"bad"})", R"({"regex":2})", R"({"fields":"bad"})" })
        {
            EXPECT_TRUE(Call("logs_tail", args).HasMember("error"));
        }
    }

    TEST_F(LocalMcpTests, SourcesCountLifetimeAndRingAndSeverity)
    {
        Capacity(2);
        Log("old");
        Severity("error");
        Log("error");
        Severity("info");
        Log("last");
        auto result = Call("logs_sources");
        ASSERT_EQ(result["sources"].Size(), 1);
        const auto& source = result["sources"][0];
        EXPECT_EQ(source["total"].GetUint64(), 3);
        EXPECT_EQ(source["ring"].GetUint64(), 2);
        EXPECT_EQ(source["levels"]["error"].GetUint64(), 1);
        EXPECT_GT(source["lastTimeMs"].GetInt64(), 0);
    }

    TEST_F(LocalMcpTests, RegisteredTagsAndPerLineTagsUseTheLoggerContract)
    {
        ASSERT_TRUE(m_server->Start(EphemeralConfig()));
        Call("log_tag", R"({"tag":"MCP_TAG_ENABLED","enabled":true})");
        Call("log_tag", R"({"tag":"MCP_TAG_DISABLED","enabled":false})");
        AZLOG(MCP_TAG_ENABLED, "tagged line");
        Log("untagged line");
        auto tags = Call("log_tags");
        bool enabledSeen = false;
        bool disabledSeen = false;
        for (const auto& tag : tags["tags"].GetArray())
        {
            if (AZStd::string_view(tag["tag"].GetString()) == "MCP_TAG_ENABLED")
            {
                enabledSeen = tag["enabled"].GetBool();
            }
            if (AZStd::string_view(tag["tag"].GetString()) == "MCP_TAG_DISABLED")
            {
                disabledSeen = !tag["enabled"].GetBool();
            }
        }
        EXPECT_TRUE(enabledSeen);
        EXPECT_TRUE(disabledSeen);
        auto selected = Call("logs_tail", R"({"fromStart":true,"tags":["MCP_TAG_ENABLED"]})");
        ASSERT_EQ(selected["lines"].Size(), 1);
        EXPECT_STREQ(selected["lines"][0]["tag"].GetString(), "MCP_TAG_ENABLED");
        EXPECT_NE(AZStd::string_view(selected["lines"][0]["message"].GetString()).find("tagged line"), AZStd::string_view::npos);
        EXPECT_EQ(selected["next"].GetUint64(), Call("logs_tail", R"({"limit":1})")["next"].GetUint64());
        auto untagged = Call("logs_tail", R"({"limit":1})");
        EXPECT_STREQ(untagged["lines"][0]["tag"].GetString(), "");
        auto empty = Call("logs_tail", R"({"fromStart":true,"tags":[]})");
        EXPECT_TRUE(empty["lines"].Empty());
        EXPECT_EQ(empty["next"].GetUint64(), untagged["next"].GetUint64());
        EXPECT_TRUE(Call("logs_tail", R"({"fromStart":true,"tags":["MCP_TAG_DISABLED"]})")["lines"].Empty());
        EXPECT_TRUE(Call("logs_tail", R"({"tags":[42]})").HasMember("error"));
        EXPECT_TRUE(Call("logs_tail", R"({"tags":false})").HasMember("error"));
    }

    TEST_F(LocalMcpTests, CollapseSummaryAndFromStartReportHistory)
    {
        Capacity(2);
        Log("same");
        Log("same");
        Log("same");
        auto data = Call("logs_tail", R"({"fromStart":true,"collapse":true})");
        EXPECT_EQ(data["firstSequence"].GetUint64(), 2);
        EXPECT_EQ(data["dropped"].GetUint64(), 1);
        ASSERT_EQ(data["lines"].Size(), 1);
        EXPECT_EQ(data["lines"][0]["count"].GetUint64(), 2);
        EXPECT_EQ(data["lines"][0]["lastSequence"].GetUint64(), 3);
        auto summary = Call("logs_tail", R"({"fromStart":true,"summary":true})");
        EXPECT_EQ(summary["counts"]["test"]["info"].GetUint64(), 2);
        EXPECT_EQ(summary["lines"][0]["first"]["sequence"].GetUint64(), 2);
        EXPECT_EQ(summary["lines"][0]["last"]["sequence"].GetUint64(), 3);
        Capacity(16384);
        EXPECT_EQ(Call("logs_tail", R"({"fromStart":false,"after":3})")["firstSequence"].GetUint64(), 2);
    }

    TEST_F(LocalMcpTests, ConfiguredRingRetainsHistoryBeyondTheDefaultCapacity)
    {
        auto config = EphemeralConfig();
        config.m_logLines = 16384;
        ASSERT_TRUE(m_server->Start(config));
        for (int i = 0; i < 520; ++i)
        {
            Log("line");
        }
        auto history = Call("logs_tail", R"({"fromStart":true})");
        EXPECT_EQ(history["firstSequence"].GetUint64(), 1);
        EXPECT_EQ(history["dropped"].GetUint64(), 0);
    }

    TEST_F(LocalMcpTests, WaitMatchesOnOutputBeforeOverwriteAndTimesOutWithoutBlocking)
    {
        Capacity(1);
        uint64_t id = 0;
        const char* args = R"({"pattern":"hit","after":0,"windows":["test"],"timeoutMs":1000})";
        EXPECT_TRUE(WaitRpc(args, id).empty());
        EXPECT_NE(id, 0);
        Log("hit");
        Log("overwrites hit");
        auto reply = WaitRpc(args, id);
        EXPECT_NE(reply.find("hit"), AZStd::string::npos);
        EXPECT_EQ(reply.find("overwrites"), AZStd::string::npos);
        EXPECT_EQ(id, 0);
        EXPECT_NE(WaitRpc(R"({"pattern":"absent","timeoutMs":0})", id).find("timedOut"), AZStd::string::npos);
        EXPECT_NE(WaitRpc(R"({"pattern":"x","timeoutMs":10000})", id).find("error"), AZStd::string::npos);
        EXPECT_NE(WaitRpc(R"({"pattern":"["})", id).find("error"), AZStd::string::npos);
    }

    TEST_F(LocalMcpTests, SearchRanksWordsHelpAndSubsequenceWithPaging)
    {
        auto exact = Call("console_search", R"({"query":"CaptureOutput"})");
        ASSERT_FALSE(exact["matches"].Empty());
        EXPECT_STREQ(exact["matches"][0]["name"].GetString(), "CaptureOutput");
        EXPECT_STREQ(exact["matches"][0]["description"].GetString(), "Output fixture.");
        EXPECT_FALSE(Call("console_search", R"({"query":"cptrotpt"})")["matches"].Empty());
        EXPECT_FALSE(Call("console_search", R"({"query":"output fixture","kind":"command"})")["matches"].Empty());
        auto names = Call("console_search", R"({"query":"","fields":"name","limit":1,"after":1})");
        EXPECT_EQ(names.Size(), 1);
        EXPECT_TRUE(names[0].IsString());
        EXPECT_TRUE(Call("console_search", R"({"query":"","limit":101})").HasMember("error"));
        EXPECT_TRUE(Call("console_search", R"({"query":"","kind":"bad"})").HasMember("error"));
    }

    TEST_F(LocalMcpTests, HttpValidationAndFragmentedRequests)
    {
        const auto request = Http(R"({"jsonrpc":"2.0","id":1,"method":"ping"})");
        for (size_t length = 0; length < request.size(); ++length)
        {
            EXPECT_TRUE(Handle(AZStd::string_view(request).substr(0, length)).empty());
        }
        EXPECT_NE(Handle(request).find("200 OK"), AZStd::string::npos);
        EXPECT_NE(Handle(Http("{}", "Origin: https://evil.example\r\n")).find("403 Forbidden"), AZStd::string::npos);
        EXPECT_NE(Handle(Http("{}", "Host: evil.example\r\n")).find("400 Bad Request"), AZStd::string::npos);
        EXPECT_NE(Handle(Http("{}", "Content-Length: 2\r\n")).find("400 Bad Request"), AZStd::string::npos);
        EXPECT_NE(Handle(Http("{}", "Transfer-Encoding: chunked\r\n")).find("400 Bad Request"), AZStd::string::npos);
        EXPECT_NE(Handle(Http("{}", "MCP-Protocol-Version: invalid\r\n")).find("400 Bad Request"), AZStd::string::npos);
        EXPECT_NE(Handle(AZStd::string(8193, 'x')).find("413 Content Too Large"), AZStd::string::npos);
        EXPECT_NE(Handle("POST /mcp HTTP/1.1\r\nHost: localhost:0\r\nContent-Type: application/json\r\n"
            "Content-Length: 999999999999999999999999\r\n\r\n").find("413 Content Too Large"), AZStd::string::npos);
        EXPECT_NE(Handle("GET /mcp HTTP/1.1\r\nHost: localhost:0\r\n\r\n").find("405 Method Not Allowed"), AZStd::string::npos);
    }

    TEST_F(LocalMcpTests, KeepAliveHandlesMultipleRequestsAndDeadlineIsConfigurable)
    {
        auto config = EphemeralConfig();
        config.m_idleTimeoutMs = 30000;
        ASSERT_TRUE(m_server->Start(config));
        const char* ping = R"({"jsonrpc":"2.0","id":1,"method":"ping"})";
        const auto reply = Exchange(Http(ping, "Connection: Keep-Alive\r\n") + Http(ping));
        const auto first = reply.find("HTTP/1.1 200 OK");
        ASSERT_NE(first, AZStd::string::npos);
        EXPECT_NE(reply.find("HTTP/1.1 200 OK", first + 1), AZStd::string::npos);
        EXPECT_NE(reply.find("Connection: keep-alive"), AZStd::string::npos);
        uint64_t id = 0;
        const char* args = R"({"pattern":"hit","timeoutMs":20000})";
        EXPECT_TRUE(WaitRpc(args, id).empty());
        Log("hit");
        EXPECT_FALSE(WaitRpc(args, id).empty());
        config.m_idleTimeoutMs = 999;
        EXPECT_FALSE(m_server->Start(config));
    }

    TEST_F(LocalMcpTests, HttpErrorClosesAPreviouslyPersistentConnection)
    {
        ASSERT_TRUE(m_server->Start(EphemeralConfig()));
        const char* ping = R"({"jsonrpc":"2.0","id":1,"method":"ping"})";
        const auto rejected = AZStd::string::format("GET /mcp HTTP/1.1\r\nHost: localhost:%u\r\n\r\n", m_server->GetPort());
        bool closed = false;
        const auto reply = Exchange(Http(ping, "Connection: keep-alive\r\n") + rejected, nullptr, &closed);
        EXPECT_NE(reply.find("200 OK"), AZStd::string::npos);
        EXPECT_NE(reply.find("405 Method Not Allowed"), AZStd::string::npos);
        EXPECT_TRUE(closed);
    }

    TEST_F(LocalMcpTests, PlainAndBasicAuthenticationOverSocket)
    {
        auto config = EphemeralConfig();
        ASSERT_TRUE(m_server->Start(config));
        EXPECT_NE(Exchange(Http(R"({"jsonrpc":"2.0","id":1,"method":"ping"})")).find("200 OK"), AZStd::string::npos);
        AZ::Debug::TraceMessageBus::Broadcast(&AZ::Debug::TraceMessageBus::Events::OnOutput, "engine", "captured");
        EXPECT_STREQ(Call("logs_tail", R"({"limit":1})")["lines"][0]["message"].GetString(), "captured");
        config.m_auth = "basic";
        config.m_username = "user";
        config.m_password = "pass";
        ASSERT_TRUE(m_server->Start(config));
        const char* ping = R"({"jsonrpc":"2.0","id":1,"method":"ping"})";
        EXPECT_NE(Exchange(Http(ping)).find("401 Unauthorized"), AZStd::string::npos);
        EXPECT_NE(Exchange(Http(ping, "Authorization: Basic dXNlcjp3cm9uZw==\r\n")).find("401 Unauthorized"), AZStd::string::npos);
        EXPECT_NE(Exchange(Http(ping, "Authorization: Basic dXNlcjpwYXNz\r\n")).find("200 OK"), AZStd::string::npos);
        config.m_password.clear();
        EXPECT_FALSE(m_server->Start(config));
        EXPECT_EQ(m_server->GetPort(), 0);
        config.m_auth = "unknown";
        EXPECT_FALSE(m_server->Start(config));
        config.m_auth = "mtls";
        EXPECT_FALSE(m_server->Start(config));
    }

    // Generate temporary identities with the existing OpenSSL dependency; no checked-in private keys.
    bool WriteIdentity(const char* certificate, const char* privateKey)
    {
        EVP_PKEY_CTX* generator = EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, nullptr);
        EVP_PKEY* key = nullptr;
        bool valid = generator && EVP_PKEY_keygen_init(generator) == 1
            && EVP_PKEY_CTX_set_rsa_keygen_bits(generator, 2048) == 1 && EVP_PKEY_keygen(generator, &key) == 1;
        EVP_PKEY_CTX_free(generator);
        X509* cert = X509_new();
        if (valid && cert)
        {
            X509_set_version(cert, 2);
            ASN1_INTEGER_set(X509_get_serialNumber(cert), 1);
            X509_gmtime_adj(X509_get_notBefore(cert), -60);
            X509_gmtime_adj(X509_get_notAfter(cert), 3600);
            X509_set_pubkey(cert, key);
            auto* name = X509_get_subject_name(cert);
            X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC, reinterpret_cast<const unsigned char*>("localhost"), -1, -1, 0);
            X509_set_issuer_name(cert, name);
            auto* extension = X509V3_EXT_conf_nid(nullptr, nullptr, NID_basic_constraints, const_cast<char*>("critical,CA:TRUE"));
            X509_add_ext(cert, extension, -1);
            X509_EXTENSION_free(extension);
            valid = X509_sign(cert, key, EVP_sha256()) > 0;
            BIO* certFile = BIO_new_file(certificate, "w");
            BIO* keyFile = BIO_new_file(privateKey, "w");
            valid = valid && certFile && keyFile && PEM_write_bio_X509(certFile, cert) == 1
                && PEM_write_bio_PrivateKey(keyFile, key, nullptr, nullptr, 0, nullptr, nullptr) == 1;
            BIO_free(certFile);
            BIO_free(keyFile);
        }
        else
        {
            valid = false;
        }
        X509_free(cert);
        EVP_PKEY_free(key);
        return valid;
    }

    TEST_F(LocalMcpTests, MutualTlsRequiresTrustedClientCertificate)
    {
        AZ::Test::ScopedAutoTempDirectory temporary;
        auto config = EphemeralConfig();
        config.m_auth = "mtls";
        config.m_certificate = temporary.Resolve("server.pem").Native();
        config.m_privateKey = temporary.Resolve("server.key").Native();
        config.m_ca = config.m_certificate;
        ASSERT_TRUE(WriteIdentity(config.m_certificate.c_str(), config.m_privateKey.c_str()));
        ASSERT_TRUE(m_server->Start(config));
        SSL_CTX* context = SSL_CTX_new(TLS_client_method());
        ASSERT_NE(context, nullptr);
        SSL_CTX_set_verify(context, SSL_VERIFY_PEER, nullptr);
        ASSERT_EQ(SSL_CTX_load_verify_locations(context, config.m_ca.c_str(), nullptr), 1);
        const auto ping = Http(R"({"jsonrpc":"2.0","id":1,"method":"ping"})");
        EXPECT_TRUE(Exchange(ping, context).empty());
        ASSERT_EQ(SSL_CTX_use_certificate_file(context, config.m_certificate.c_str(), SSL_FILETYPE_PEM), 1);
        ASSERT_EQ(SSL_CTX_use_PrivateKey_file(context, config.m_privateKey.c_str(), SSL_FILETYPE_PEM), 1);
        EXPECT_NE(Exchange(ping, context).find("200 OK"), AZStd::string::npos);
        const auto otherCertificate = temporary.Resolve("other.pem");
        const auto otherKey = temporary.Resolve("other.key");
        ASSERT_TRUE(WriteIdentity(otherCertificate.c_str(), otherKey.c_str()));
        ASSERT_EQ(SSL_CTX_use_certificate_file(context, otherCertificate.c_str(), SSL_FILETYPE_PEM), 1);
        ASSERT_EQ(SSL_CTX_use_PrivateKey_file(context, otherKey.c_str(), SSL_FILETYPE_PEM), 1);
        EXPECT_TRUE(Exchange(ping, context).empty());
        SSL_CTX_free(context);
    }

    TEST_F(LocalMcpTests, SettingsRegistryAndIniConfiguration)
    {
        AZ::SettingsRegistryImpl registry;
        Config config;
        ASSERT_TRUE(ReadConfig(registry, config));
        EXPECT_FALSE(config.m_enabled);
        EXPECT_EQ(config.m_logLines, 512);
        EXPECT_EQ(config.m_idleTimeoutMs, 10000);
        registry.Set("/O3DE/Gems/LocalMcp/LogLines", AZ::s64(16384));
        registry.Set("/O3DE/Gems/LocalMcp/IdleTimeoutMs", AZ::s64(30000));
        ASSERT_TRUE(ReadConfig(registry, config));
        EXPECT_EQ(config.m_logLines, 16384);
        EXPECT_EQ(config.m_idleTimeoutMs, 30000);
        registry.Set("/O3DE/Gems/LocalMcp/LogLines", AZ::s64(0));
        EXPECT_FALSE(ReadConfig(registry, config));
        registry.Remove("/O3DE/Gems/LocalMcp/LogLines");
        AZ::Test::ScopedAutoTempDirectory temporary;
        const auto path = temporary.Resolve("localmcp.ini");
        ASSERT_TRUE(AZ::Test::CreateTestFile(temporary, "localmcp.ini", "Enabled=true\nPort=4700\nAuth=basic\nUsername=test\nPassword=secret\n"));
        registry.Set("/O3DE/Gems/LocalMcp/ConfigFile", path.Native());
        ASSERT_TRUE(ReadConfig(registry, config));
        EXPECT_TRUE(config.m_enabled);
        EXPECT_EQ(config.m_port, 4700);
        EXPECT_EQ(config.m_auth, "basic");
        EXPECT_EQ(config.m_password, "secret");
        registry.Remove("/O3DE/Gems/LocalMcp/ConfigFile");
        registry.Set("/O3DE/Gems/LocalMcp/Port", "bad");
        EXPECT_FALSE(ReadConfig(registry, config));
    }
    TEST_F(LocalMcpTests, OptionalProviderSharesTransportAndStaysOutOfRuntimeTools)
    {
        auto runtime = Rpc(R"({"jsonrpc":"2.0","id":1,"method":"tools/list"})");
        EXPECT_EQ(runtime["result"]["tools"].Size(), 8);
        class Provider final : public ToolProvider
        {
        public:
            const char* GetServerName() const override { return "test-editor"; }
            void AddTools(rapidjson::Value& tools, rapidjson::Document::AllocatorType& allocator) const override
            {
                rapidjson::Value entry(rapidjson::kObjectType);
                entry.AddMember("name", "test_tool", allocator);
                tools.PushBack(entry, allocator);
            }
            Result Invoke(AZStd::string_view name, const rapidjson::Value& args, rapidjson::Value& data,
                rapidjson::Document::AllocatorType& allocator, AZStd::string& detail) override
            {
                if (name != "test_tool") { return Result::Unknown; }
                if (!args.HasMember("value") || !args["value"].IsInt())
                {
                    detail = "Expected integer value";
                    return Result::InvalidArguments;
                }
                data.AddMember("value", args["value"].GetInt(), allocator);
                m_calledOn = AZStd::this_thread::get_id();
                return Result::Complete;
            }
            AZStd::thread::id m_calledOn;
        } provider;
        m_server = AZStd::make_unique<Server>(&provider);
        auto tools = Rpc(R"({"jsonrpc":"2.0","id":1,"method":"tools/list"})");
        EXPECT_EQ(tools["result"]["tools"].Size(), 9);
        auto called = Call("test_tool", R"({"value":42})");
        EXPECT_EQ(called["value"].GetInt(), 42);
        EXPECT_EQ(provider.m_calledOn, AZStd::this_thread::get_id());
        auto bad = Call("test_tool");
        EXPECT_EQ(bad["error"]["code"].GetInt(), -32602);
        auto unknown = Call("unknown_editor_tool");
        EXPECT_EQ(unknown["error"]["code"].GetInt(), -32602);
        auto identity = Rpc(R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{
            "protocolVersion":"2025-11-25","capabilities":{},"clientInfo":{}}})");
        EXPECT_STREQ(identity["result"]["serverInfo"]["name"].GetString(), "test-editor");
        m_server.reset();
    }

    class PendingTestProvider final : public ToolProvider
    {
    public:
        const char* GetServerName() const override { return "pending-test"; }
        void AddTools(rapidjson::Value&, rapidjson::Document::AllocatorType&) const override {}
        Result Invoke(AZStd::string_view, const rapidjson::Value&, rapidjson::Value&,
            rapidjson::Document::AllocatorType&, AZStd::string&) override
        {
            ++m_invocations;
            return Result::Pending;
        }
        Result PollPending(rapidjson::Value& data, rapidjson::Document::AllocatorType& allocator, AZStd::string&) override
        {
            ++m_polls;
            if (!m_complete || m_polls < 3) { return Result::Pending; }
            data.AddMember("value", 42, allocator);
            return Result::Complete;
        }
        void CancelPending() override { ++m_cancellations; }
        bool m_complete = true;
        int m_invocations = 0;
        int m_polls = 0;
        int m_cancellations = 0;
    };

    TEST_F(LocalMcpTests, PendingProviderResumesWithoutRepeatingMutation)
    {
        PendingTestProvider provider;
        m_server = AZStd::make_unique<Server>(&provider);
        EXPECT_TRUE(m_server->Start(EphemeralConfig()));
        const auto reply = Exchange(Http(R"({"jsonrpc":"2.0","id":71,"method":"tools/call","params":{"name":"delayed"}})"));
        const auto body = reply.find("\r\n\r\n");
        EXPECT_NE(body, AZStd::string::npos);
        rapidjson::Document response;
        if (body != AZStd::string::npos)
        {
            response.Parse(reply.c_str() + body + 4);
            EXPECT_FALSE(response.HasParseError());
            EXPECT_EQ(response["id"].GetInt(), 71);
            EXPECT_FALSE(response["result"]["isError"].GetBool());
        }
        EXPECT_EQ(provider.m_invocations, 1);
        EXPECT_GE(provider.m_polls, 3);
        EXPECT_EQ(provider.m_cancellations, 0);
        m_server.reset();
        EXPECT_EQ(provider.m_cancellations, 0);
    }

    TEST_F(LocalMcpTests, PendingProviderConnectionDeadlineCancelsActiveWork)
    {
        PendingTestProvider provider;
        provider.m_complete = false;
        m_server = AZStd::make_unique<Server>(&provider);
        auto config = EphemeralConfig();
        config.m_idleTimeoutMs = 1000;
        EXPECT_TRUE(m_server->Start(config));
        bool closed = false;
        const auto reply = Exchange(Http(R"({"jsonrpc":"2.0","id":72,"method":"tools/call","params":{"name":"delayed"}})"), nullptr, &closed);
        EXPECT_TRUE(closed);
        EXPECT_TRUE(reply.empty());
        EXPECT_EQ(provider.m_invocations, 1);
        EXPECT_GT(provider.m_polls, 0);
        EXPECT_EQ(provider.m_cancellations, 1);
        m_server.reset();
        EXPECT_EQ(provider.m_cancellations, 1);
    }

    TEST_F(LocalMcpTests, PendingProviderWithoutHeldConnectionRefusesAndCancels)
    {
        PendingTestProvider provider;
        m_server = AZStd::make_unique<Server>(&provider);
        auto result = Call("delayed");
        EXPECT_EQ(result["error"]["code"].GetInt(), -32602);
        EXPECT_EQ(provider.m_invocations, 1);
        EXPECT_EQ(provider.m_polls, 0);
        EXPECT_EQ(provider.m_cancellations, 1);
        m_server.reset();
    }

    TEST_F(LocalMcpTests, EditorListenerBindsOnlyLoopback)
    {
        auto config = EphemeralConfig();
        config.m_loopbackOnly = true;
        ASSERT_TRUE(m_server->Start(config));
        ExpectLoopbackBound();
        EXPECT_NE(m_server->GetPort(), 0);
        EXPECT_NE(Exchange(Http(R"({"jsonrpc":"2.0","id":1,"method":"ping"})")).find("200 OK"), AZStd::string::npos);
    }

}
