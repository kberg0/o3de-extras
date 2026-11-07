/*
 * Copyright (c) Contributors to the Open 3D Engine Project.
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 */
#include "LocalMcpServer.h"
#include "LocalMcpJson.h"
#include "LocalMcpTools.h"
#include <process.h>
#include <AzCore/Console/IConsole.h>
#include <AzCore/Console/ILogger.h>
#include <AzCore/Settings/SettingsRegistryMergeUtils.h>
#include <AzCore/StringFunc/StringFunc.h>
#include <AzCore/std/algorithm.h>
#include <AzCore/std/chrono/chrono.h>
#include <AzCore/std/containers/unordered_map.h>
#include <AzCore/std/sort.h>
#include <AzCore/std/string/utf8/core.h>
#include <AzNetworking/Utilities/NetworkIncludes.h>
#include <AzCore/JSON/document.h>
#include <AzCore/JSON/stringbuffer.h>
#include <AzCore/JSON/writer.h>
#include <openssl/ssl.h>
#include <openssl/crypto.h>
#include <openssl/err.h>
#include <algorithm>

namespace LocalMcp
{
    namespace
    {
        constexpr size_t MaxHeader = 8192;
        constexpr size_t MaxBody = 65536;
        constexpr AZStd::string_view ProtocolVersion = "2025-11-25";

        AZStd::string HttpResponse(int status, AZStd::string_view body = {})
        {
            const char* reason = "Bad Request";
            switch (status)
            {
            case 200: reason = "OK"; break;
            case 202: reason = "Accepted"; break;
            case 401: reason = "Unauthorized"; break;
            case 403: reason = "Forbidden"; break;
            case 404: reason = "Not Found"; break;
            case 405: reason = "Method Not Allowed"; break;
            case 413: reason = "Content Too Large"; break;
            case 415: reason = "Unsupported Media Type"; break;
            }
            AZStd::string response = AZStd::string::format(
                "HTTP/1.1 %d %s\r\nContent-Type: application/json\r\nContent-Length: %zu\r\nConnection: close\r\n",
                status, reason, body.size());
            if (status == 401)
            {
                response += "WWW-Authenticate: Basic realm=\"LocalMcp\", charset=\"UTF-8\"\r\n";
            }
            if (status == 405)
            {
                response += "Allow: POST\r\n";
            }
            response += "\r\n";
            response.append(body.data(), body.size());
            return response;
        }

        AZStd::string_view Trim(AZStd::string_view text)
        {
            const size_t start = text.find_first_not_of(" \t");
            return start == AZStd::string_view::npos ? AZStd::string_view{} :
                text.substr(start, text.find_last_not_of(" \t") - start + 1);
        }
    }

    bool ReadConfig(AZ::SettingsRegistryInterface& registry, Config& config, AZStd::string_view settingsRoot,
        const Config& defaults)
    {
        config = defaults;
        AZStd::string file;
        const AZStd::string root(settingsRoot);
        if (registry.GetType(root + "/ConfigFile") != AZ::SettingsRegistryInterface::Type::NoType
            && !registry.Get(file, root + "/ConfigFile"))
        {
            AZ_Warning("LocalMcp", false, "ConfigFile must be a string.");
            return false;
        }
        if (!file.empty())
        {
            AZ::SettingsRegistryMergeUtils::ConfigParserSettings parser;
            parser.m_registryRootPointerPath = root;
            if (!AZ::SettingsRegistryMergeUtils::MergeSettingsToRegistry_ConfigFile(registry, file, parser))
            {
                AZ_Warning("LocalMcp", false, "Unable to load ConfigFile.");
                return false;
            }
        }
        auto read = [&](auto& value, const char* key)
        {
            const auto path = root + "/" + key;
            if (registry.GetType(path) == AZ::SettingsRegistryInterface::Type::NoType)
            {
                return true;
            }
            AZStd::remove_reference_t<decltype(value)> parsed{};
            if (!registry.Get(parsed, path))
            {
                return false;
            }
            value = AZStd::move(parsed);
            return true;
        };
        const bool valid = read(config.m_enabled, "Enabled") && read(config.m_port, "Port") && read(config.m_auth, "Auth")
            && read(config.m_username, "Username") && read(config.m_password, "Password")
            && read(config.m_certificate, "Certificate") && read(config.m_privateKey, "PrivateKey") && read(config.m_ca, "CA")
            && read(config.m_logLines, "LogLines") && read(config.m_idleTimeoutMs, "IdleTimeoutMs")
            && config.m_port > 0 && config.m_port <= 65535 && config.m_logLines >= 1 && config.m_logLines <= 1048576
            && config.m_idleTimeoutMs >= 1000 && config.m_idleTimeoutMs <= 3600000;
        AZ_Warning("LocalMcp", valid, "Invalid configuration type, port, LogLines or IdleTimeoutMs.");
        return valid;
    }

    struct Server::Client : AzNetworking::TcpSocket
    {
        Client(AzNetworking::SocketFd fd, SSL_CTX* context) : TcpSocket(fd)
        {
            if (context && IsOpen())
            {
                m_ssl = SSL_new(context);
                if (!m_ssl || SSL_set_fd(m_ssl, static_cast<int>(fd)) != 1)
                {
                    Close();
                }
            }
        }
        ~Client() override
        {
            SSL_free(m_ssl);
        }
        int Ready()
        {
            if (!IsOpen())
            {
                return -1;
            }
            if (!m_ssl || SSL_is_init_finished(m_ssl))
            {
                return 1;
            }
            ERR_clear_error();
            const int result = SSL_accept(m_ssl);
            return result == 1 ? 1 : Translate(result);
        }
        int Translate(int result) const
        {
            const int error = SSL_get_error(m_ssl, result);
            return error == SSL_ERROR_WANT_READ || error == SSL_ERROR_WANT_WRITE ? 0 : -1;
        }
        int32_t SendInternal(const uint8_t* data, uint32_t size) const override
        {
            if (!m_ssl)
            {
                return TcpSocket::SendInternal(data, size);
            }
            ERR_clear_error();
            const int result = SSL_write(m_ssl, data, aznumeric_cast<int>(size));
            return result > 0 ? result : Translate(result);
        }
        int32_t ReceiveInternal(uint8_t* data, uint32_t size) const override
        {
            if (!m_ssl)
            {
                return TcpSocket::ReceiveInternal(data, size);
            }
            ERR_clear_error();
            const int result = SSL_read(m_ssl, data, aznumeric_cast<int>(size));
            return result > 0 ? result : Translate(result);
        }

        SSL* m_ssl = nullptr;
        AZStd::string m_input;
        AZStd::string m_output;
        size_t m_sent = 0;
        size_t m_consumed = 0;
        bool m_keepAlive = false;
        uint64_t m_waitId = 0;
        bool m_providerPending = false;
        AZStd::chrono::steady_clock::time_point m_deadline = AZStd::chrono::steady_clock::now() + AZStd::chrono::seconds(10);
    };

    bool Server::Listener::ListenLoopback(uint16_t port)
    {
        Close();
        if (!SocketCreateInternal())
        {
            return false;
        }
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = htons(port);
        const auto fd = static_cast<int>(GetSocketFd());
        if (::bind(fd, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0 ||
            ::listen(fd, SOMAXCONN) != 0 || !AzNetworking::SetSocketNonBlocking(GetSocketFd()) ||
            !AzNetworking::SetSocketNoDelay(GetSocketFd()))
        {
            Close();
            return false;
        }
        return true;
    }

    Server::Server(ToolProvider* provider) : m_provider(provider) {}
    Server::~Server()
    {
        Stop();
    }

    bool Server::Start(const Config& config)
    {
        Stop();
        if (config.m_port < 0 || config.m_port > 65535 || config.m_logLines < 1 || config.m_logLines > 1048576 ||
            config.m_idleTimeoutMs < 1000 || config.m_idleTimeoutMs > 3600000 ||
            (config.m_auth != "none" && config.m_auth != "basic" && config.m_auth != "mtls"))
        {
            return false;
        }
        if (config.m_auth == "basic")
        {
            if (config.m_username.empty() || config.m_password.empty() || config.m_username.find(':') != AZStd::string::npos
                || config.m_username.size() + config.m_password.size() > 1024)
            {
                return false;
            }
            const auto credentials = config.m_username + ":" + config.m_password;
            AZStd::string encoded(4 * ((credentials.size() + 2) / 3) + 1, '\0');
            const int length = EVP_EncodeBlock(reinterpret_cast<unsigned char*>(encoded.data()),
                reinterpret_cast<const unsigned char*>(credentials.data()), aznumeric_cast<int>(credentials.size()));
            encoded.resize(length);
            m_authorization = "Basic " + encoded;
        }
        if (config.m_auth == "mtls")
        {
            m_tlsContext = SSL_CTX_new(TLS_server_method());
            if (m_tlsContext)
            {
                SSL_CTX_set_default_passwd_cb(m_tlsContext, [](char*, int, int, void*) { return 0; });
            }
            if (!m_tlsContext || SSL_CTX_set_min_proto_version(m_tlsContext, TLS1_2_VERSION) != 1
                || SSL_CTX_use_certificate_chain_file(m_tlsContext, config.m_certificate.c_str()) != 1
                || SSL_CTX_use_PrivateKey_file(m_tlsContext, config.m_privateKey.c_str(), SSL_FILETYPE_PEM) != 1
                || SSL_CTX_check_private_key(m_tlsContext) != 1
                || SSL_CTX_load_verify_locations(m_tlsContext, config.m_ca.c_str(), nullptr) != 1)
            {
                Stop();
                return false;
            }
            SSL_CTX_set_verify(m_tlsContext, SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT, nullptr);
            SSL_CTX_set_session_cache_mode(m_tlsContext, SSL_SESS_CACHE_OFF);
            SSL_CTX_set_options(m_tlsContext, SSL_OP_NO_TICKET);
        }
        m_socketLayerInitialized = AzNetworking::SocketLayerInit();
        if (!m_socketLayerInitialized || !(config.m_loopbackOnly ?
            m_listener.ListenLoopback(aznumeric_cast<uint16_t>(config.m_port)) :
            m_listener.Listen(aznumeric_cast<uint16_t>(config.m_port))))
        {
            Stop();
            return false;
        }
        sockaddr_in address{};
        socklen_t size = sizeof(address);
        if (getsockname(static_cast<int>(m_listener.GetSocketFd()), reinterpret_cast<sockaddr*>(&address), &size) != 0)
        {
            Stop();
            return false;
        }
        m_port = ntohs(address.sin_port);
        m_logLines = aznumeric_cast<size_t>(config.m_logLines);
        m_idleTimeoutMs = config.m_idleTimeoutMs;
        AZ::Debug::TraceMessageBus::Handler::BusConnect();
        return true;
    }

    void Server::Stop()
    {
        AZ::Debug::TraceMessageBus::Handler::BusDisconnect();
        for (auto& client : m_clients) { ReleasePending(*client); }
        m_clients.clear();
        m_listener.Close();
        SSL_CTX_free(m_tlsContext);
        m_tlsContext = nullptr;
        m_authorization.clear();
        m_port = 0;
        if (m_socketLayerInitialized)
        {
            AzNetworking::SocketLayerShutdown();
            m_socketLayerInitialized = false;
        }
        AZStd::scoped_lock lock(m_logMutex);
        if (m_sequence)
        {
            const uint64_t first = m_logs.empty() ? m_sequence + 1 : m_logs.front().m_sequence;
            // Shutdown is the last opportunity to audit retention before this volatile history is released.
            AZ_TracePrintf("LocalMcp", "History at shutdown: logLines=%zu sequence=%llu firstSequence=%llu dropped=%llu\n",
                m_logLines, m_sequence, first, first - 1);
        }
        m_logs.clear();
        m_sources.clear();
        m_waits.clear();
        m_sequence = 0;
    }

    uint16_t Server::GetPort() const
    {
        return m_port;
    }

    void Server::ReleasePending(Client& client)
    {
        if (client.m_providerPending && m_provider)
        {
            m_provider->CancelPending();
            client.m_providerPending = false;
        }
        AZStd::scoped_lock lock(m_logMutex);
        m_waits.erase(client.m_waitId);
        client.m_waitId = 0;
    }

    void Server::Poll()
    {
        if (!m_listener.IsOpen())
        {
            return;
        }
        for (int attempt = 0; attempt < 8 && m_clients.size() < 8; ++attempt)
        {
            sockaddr_in peer{};
            socklen_t size = sizeof(peer);
            const auto fd = static_cast<AzNetworking::SocketFd>(
                accept(static_cast<int>(m_listener.GetSocketFd()), reinterpret_cast<sockaddr*>(&peer), &size));
            if (fd == AzNetworking::InvalidSocketFd)
            {
                break;
            }
            // Keep AzNetworking's existing bind behavior; only local peers get console access.
            if ((ntohl(peer.sin_addr.s_addr) >> 24) != 127)
            {
                AzNetworking::CloseSocket(fd);
                continue;
            }
            m_clients.push_back(AZStd::make_unique<Client>(fd, m_tlsContext));
            m_clients.back()->m_deadline = AZStd::chrono::steady_clock::now() + AZStd::chrono::milliseconds(m_idleTimeoutMs);
        }
        m_clients.erase(std::remove_if(m_clients.begin(), m_clients.end(), [this](auto& client)
        {
            if (AZStd::chrono::steady_clock::now() >= client->m_deadline)
            {
                ReleasePending(*client);
                return true;
            }
            const int ready = client->Ready();
            if (ready <= 0)
            {
                if (ready < 0) { ReleasePending(*client); }
                return ready < 0;
            }
            if (client->m_output.empty())
            {
                if (client->m_waitId)
                {
                    AZStd::scoped_lock lock(m_logMutex);
                    const auto& wait = m_waits.at(client->m_waitId);
                    if (!wait.m_match.m_sequence && AZStd::chrono::steady_clock::now() < wait.m_until)
                    {
                        return false;
                    }
                }
                uint8_t buffer[4096];
                const int received = client->Receive(buffer, sizeof(buffer));
                if (received < 0)
                {
                    ReleasePending(*client);
                    return true;
                }
                client->m_input.append(reinterpret_cast<const char*>(buffer), received);
                if (received > 0)
                {
                    client->m_deadline = AZStd::chrono::steady_clock::now() + AZStd::chrono::milliseconds(m_idleTimeoutMs);
                }
                client->m_consumed = 0;
                client->m_keepAlive = false;
                client->m_output = HandleHttp(client->m_input, client.get());
                if (!client->m_output.empty())
                {
                    client->m_input.erase(0, client->m_consumed);
                    client->m_deadline = AZStd::chrono::steady_clock::now() + AZStd::chrono::milliseconds(m_idleTimeoutMs);
                }
            }
            if (!client->m_output.empty())
            {
                const size_t size = AZStd::min<size_t>(client->m_output.size() - client->m_sent, 16384);
                const int sent = client->Send(reinterpret_cast<const uint8_t*>(client->m_output.data() + client->m_sent),
                    aznumeric_cast<uint32_t>(size));
                if (sent < 0)
                {
                    ReleasePending(*client);
                    return true;
                }
                client->m_sent += sent;
                if (sent > 0)
                {
                    client->m_deadline = AZStd::chrono::steady_clock::now() + AZStd::chrono::milliseconds(m_idleTimeoutMs);
                }
                if (client->m_sent == client->m_output.size())
                {
                    if (!client->m_keepAlive)
                    {
                        return true;
                    }
                    client->m_output.clear();
                    client->m_sent = 0;
                }
            }
            return false;
        }), m_clients.end());
    }

    AZStd::string Server::HandleHttp(AZStd::string_view request, Client* client)
    {
        const size_t headerEnd = request.find("\r\n\r\n");
        if (headerEnd == AZStd::string_view::npos)
        {
            return request.size() > MaxHeader ? HttpResponse(413) : AZStd::string{};
        }
        if (headerEnd > MaxHeader)
        {
            return HttpResponse(413);
        }
        const size_t firstLineEnd = request.find("\r\n");
        const auto firstLine = request.substr(0, firstLineEnd);
        if (firstLine != "POST /mcp HTTP/1.1" && firstLine != "GET /mcp HTTP/1.1" && firstLine != "DELETE /mcp HTTP/1.1")
        {
            return HttpResponse(404);
        }
        AZStd::unordered_map<AZStd::string, AZStd::string_view> headers;
        for (size_t pos = firstLineEnd + 2; pos < headerEnd;)
        {
            const size_t end = request.find("\r\n", pos);
            const auto line = request.substr(pos, end - pos);
            const size_t colon = line.find(':');
            if (colon == AZStd::string_view::npos || colon == 0)
            {
                return HttpResponse(400);
            }
            AZStd::string key(line.substr(0, colon));
            for (char& character : key)
            {
                if (character >= 'A' && character <= 'Z')
                {
                    character += 'a' - 'A';
                }
                if (!(character >= 'a' && character <= 'z') && !(character >= '0' && character <= '9') && character != '-')
                {
                    return HttpResponse(400);
                }
            }
            const auto value = Trim(line.substr(colon + 1));
            if (value.find_first_of("\r\n\0", 0, 3) != AZStd::string_view::npos || !headers.emplace(key, value).second)
            {
                return HttpResponse(400);
            }
            pos = end + 2;
        }
        const auto port = AZStd::string::format(":%u", m_port);
        const auto host = headers["host"];
        if (host != "127.0.0.1" + port && host != "localhost" + port)
        {
            return HttpResponse(403);
        }
        if (headers.contains("origin"))
        {
            const AZStd::string scheme = m_tlsContext ? "https://" : "http://";
            const auto origin = headers["origin"];
            if (origin != scheme + "127.0.0.1" + port && origin != scheme + "localhost" + port)
            {
                return HttpResponse(403);
            }
        }
        if (firstLine != "POST /mcp HTTP/1.1")
        {
            return HttpResponse(405);
        }
        const auto version = headers["mcp-protocol-version"];
        if (!version.empty() && version != ProtocolVersion && version != "2025-03-26" && version != "2025-06-18")
        {
            return HttpResponse(400);
        }
        const auto contentType = headers["content-type"];
        if (contentType != "application/json" && !contentType.starts_with("application/json;"))
        {
            return HttpResponse(415);
        }
        if (headers.contains("transfer-encoding") || headers.contains("expect") || !headers.contains("content-length"))
        {
            return HttpResponse(400);
        }
        const auto length = headers["content-length"];
        size_t bodySize = 0;
        if (length.empty())
        {
            return HttpResponse(400);
        }
        for (char digit : length)
        {
            if (digit < '0' || digit > '9')
            {
                return HttpResponse(400);
            }
            bodySize = bodySize * 10 + (digit - '0');
            if (bodySize > MaxBody)
            {
                return HttpResponse(413);
            }
        }
        if (request.size() - headerEnd - 4 < bodySize)
        {
            return {};
        }
        if (!m_authorization.empty())
        {
            const auto authorization = headers["authorization"];
            if (authorization.size() != m_authorization.size()
                || CRYPTO_memcmp(authorization.data(), m_authorization.data(), authorization.size()) != 0)
            {
                return HttpResponse(401);
            }
        }
        int status = 200;
        const auto body = HandleRpc(request.substr(headerEnd + 4, bodySize), status, client ? &client->m_waitId : nullptr,
            client ? &client->m_providerPending : nullptr);
        if (client && (client->m_waitId || client->m_providerPending))
        {
            return {};
        }
        auto response = HttpResponse(status, body);
        if (client)
        {
            client->m_consumed = headerEnd + 4 + bodySize;
            client->m_keepAlive = AZ::StringFunc::Equal(headers["connection"], AZStd::string_view("keep-alive"));
            if (client->m_keepAlive)
            {
                const auto pos = response.find("Connection: close");
                response.replace(pos, strlen("Connection: close"), "Connection: keep-alive");
            }
        }
        return response;
    }

    AZStd::string Server::HandleRpc(AZStd::string_view body, int& status, uint64_t* waitId, bool* providerPending)
    {
        rapidjson::Document request;
        request.Parse<rapidjson::kParseValidateEncodingFlag | rapidjson::kParseIterativeFlag>(body.data(), body.size());
        rapidjson::Document response(rapidjson::kObjectType);
        auto& allocator = response.GetAllocator();
        response.AddMember("jsonrpc", "2.0", allocator);
        response.AddMember("id", Json(), allocator);
        auto error = [&](int code, const char* message)
        {
            Json detail(rapidjson::kObjectType);
            detail.AddMember("code", code, allocator);
            detail.AddMember("message", String(message, allocator), allocator);
            response.AddMember("error", detail, allocator);
            return Serialize(response);
        };
        if (request.HasParseError())
        {
            return error(-32700, "Parse error");
        }
        if (!request.IsObject() || !IsString(request, "jsonrpc") || AZStd::string_view(request["jsonrpc"].GetString()) != "2.0"
            || !IsString(request, "method"))
        {
            return error(-32600, "Invalid request");
        }
        if (!request.HasMember("id"))
        {
            status = 202;
            return {};
        }
        const auto& id = request["id"];
        if (!id.IsString() && !id.IsInt64() && !id.IsUint64())
        {
            return error(-32600, "Invalid request id");
        }
        response["id"].CopyFrom(id, allocator);
        Json result(rapidjson::kObjectType);
        const AZStd::string_view method(request["method"].GetString());
        const Json empty(rapidjson::kObjectType);
        const Json& params = request.HasMember("params") ? request["params"] : empty;
        if (!params.IsObject())
        {
            return error(-32602, "Expected object params");
        }
        if (method == "initialize")
        {
            if (!IsString(params, "protocolVersion") || !params.HasMember("capabilities") || !params["capabilities"].IsObject()
                || !params.HasMember("clientInfo") || !params["clientInfo"].IsObject())
            {
                return error(-32602, "Expected protocolVersion, capabilities and clientInfo");
            }
            const AZStd::string_view version(params["protocolVersion"].GetString());
            result.AddMember("protocolVersion", String(version == "2025-03-26" || version == "2025-06-18" ? version : ProtocolVersion,
                allocator), allocator);
            Json capabilities(rapidjson::kObjectType);
            capabilities.AddMember("tools", Json(rapidjson::kObjectType), allocator);
            result.AddMember("capabilities", capabilities, allocator);
            Json info(rapidjson::kObjectType);
            info.AddMember("name", String(m_provider ? m_provider->GetServerName() : "o3de-local-console", allocator), allocator);
            info.AddMember("version", "0.2.0", allocator);
            Json process(rapidjson::kObjectType);
            process.AddMember("pid", _getpid(), allocator);
            info.AddMember("process", process, allocator);
            result.AddMember("idleTimeoutMs", m_idleTimeoutMs, allocator);
            result.AddMember("serverInfo", info, allocator);
        }
        else if (method == "ping")
        {
        }
        else if (method == "tools/list")
        {
            // Tool schemas live in LocalMcpTools.h.
            rapidjson::Document tools;
            tools.Parse(ToolSchemas);
            result.CopyFrom(tools, allocator);
            if (m_provider)
            {
                m_provider->AddTools(result["tools"], allocator);
            }
        }
        else if (method == "tools/call")
        {
            if (!IsString(params, "name") || (params.HasMember("arguments") && !params["arguments"].IsObject()))
            {
                return error(-32602, "Expected tool name and object arguments");
            }
            const AZStd::string_view name(params["name"].GetString());
            const Json& args = params.HasMember("arguments") ? params["arguments"] : empty;
            Json data(rapidjson::kObjectType);
            bool failed = false;
            auto* console = AZ::Interface<AZ::IConsole>::Get();
            if (name == "logs_tail" || name == "logs_sources" || name == "logs_wait" || name == "log_tags")
            {
                AZStd::string detail;
                if (!HandleLogTool(name, args, data, allocator, detail, waitId))
                {
                    return error(-32602, detail.c_str());
                }
                if (waitId && *waitId)
                {
                    return {};
                }
                failed = data.HasMember("error");
            }
            else if (name != "console_complete" && name != "console_execute" && name != "log_tag" && name != "console_search")
            {
                AZStd::string detail;
                const auto invoked = m_provider ?
                    (providerPending && *providerPending ? m_provider->PollPending(data, allocator, detail) :
                        m_provider->Invoke(name, args, data, allocator, detail)) : ToolProvider::Result::Unknown;
                if (invoked == ToolProvider::Result::Pending)
                {
                    if (!providerPending)
                    {
                        m_provider->CancelPending();
                        return error(-32602, "Pending tools require a live HTTP connection");
                    }
                    *providerPending = true;
                    return {};
                }
                if (providerPending) { *providerPending = false; }
                if (invoked == ToolProvider::Result::Unknown)
                {
                    return error(-32602, "Unknown tool");
                }
                if (invoked == ToolProvider::Result::InvalidArguments)
                {
                    return error(-32602, detail.c_str());
                }
                failed = data.HasMember("error");
            }
            else if (!console)
            {
                failed = true;
                data.AddMember("error", "AZ console unavailable", allocator);
            }
            else if (name == "console_complete")
            {
                if (args.HasMember("prefix") && !IsString(args, "prefix", AZ::MaxCVarStringLength))
                {
                    return error(-32602, "Invalid prefix");
                }
                const AZStd::string_view prefix = args.HasMember("prefix") ? args["prefix"].GetString() : "";
                AZStd::vector<AZStd::string> matches;
                AZStd::string completion(prefix);
                if (prefix.empty())
                {
                    console->VisitRegisteredFunctors([&](AZ::ConsoleFunctorBase* functor)
                    {
                        if ((functor->GetFlags() & AZ::ConsoleFunctorFlags::IsInvisible) == AZ::ConsoleFunctorFlags::Null)
                        {
                            matches.emplace_back(functor->GetName());
                        }
                    });
                }
                else
                {
                    completion = console->AutoCompleteCommand(prefix, &matches);
                }
                AZStd::sort(matches.begin(), matches.end());
                Json list(rapidjson::kArrayType);
                for (const auto& match : matches)
                {
                    Json entry(rapidjson::kObjectType);
                    entry.AddMember("name", String(match, allocator), allocator);
                    const auto commandName = AZStd::string_view(match).substr(0, match.find_first_of(" \t\r\n"));
                    if (auto* functor = commandName.size() <= AZ::MaxCVarStringLength ? console->FindCommand(commandName) : nullptr)
                    {
                        entry.AddMember("description", String(functor->GetDesc(), allocator), allocator);
                        entry.AddMember("kind", String(functor->GetTypeId().IsNull() ? "command" : "cvar", allocator), allocator);
                        AZ::CVarFixedString value;
                        if (functor->GetValue(value) == AZ::GetValueResult::Success)
                        {
                            entry.AddMember("value", String(value, allocator), allocator);
                        }
                    }
                    list.PushBack(entry, allocator);
                }
                data.AddMember("completion", String(completion, allocator), allocator);
                data.AddMember("matches", list, allocator);
            }
            else if (name == "console_search")
            {
                if (!IsString(args, "query", 256) || (args.HasMember("kind") && (!IsString(args, "kind") ||
                    (AZStd::string_view(args["kind"].GetString()) != "command" && AZStd::string_view(args["kind"].GetString()) != "cvar")))
                    || (args.HasMember("limit") && (!args["limit"].IsUint() || !args["limit"].GetUint() || args["limit"].GetUint() > 100))
                    || (args.HasMember("after") && !args["after"].IsUint())
                    || (args.HasMember("fields") && (!IsString(args, "fields") || AZStd::string_view(args["fields"].GetString()) != "name")))
                {
                    return error(-32602, "Expected query, optional kind, limit [1,100], nonnegative after, fields=name");
                }
                SearchConsole(args, data, allocator);
            }
            else if (name == "console_execute")
            {
                if (!IsString(args, "command", 4096) || Trim(args["command"].GetString()).empty())
                {
                    return error(-32602, "Expected a nonempty command of at most 4096 bytes");
                }
                if ((args.HasMember("output") && (!IsString(args, "output") ||
                    (AZStd::string_view(args["output"].GetString()) != "none" && AZStd::string_view(args["output"].GetString()) != "capture")))
                    || (args.HasMember("grep") && !IsString(args, "grep", 2048)))
                {
                    return error(-32602, "Expected output=none|capture and optional grep regex");
                }
                AZStd::regex grep(args.HasMember("grep") ? args["grep"].GetString() : "");
                if (!grep.Valid())
                {
                    return error(-32602, "Invalid grep regex");
                }
                Capture capture;
                capture.m_server = this;
                uint64_t sequenceBefore;
                {
                    AZStd::scoped_lock lock(m_logMutex);
                    sequenceBefore = m_sequence;
                }
                AZ::ConsoleCommandContainer tokens;
                bool valid = true;
                AZ::StringFunc::TokenizeVisitor(args["command"].GetString(), [&](AZStd::string_view token)
                {
                    if (tokens.size() == tokens.capacity() || token.size() > AZ::MaxCVarStringLength)
                    {
                        valid = false;
                    }
                    else
                    {
                        tokens.push_back(token);
                    }
                }, " \t\r\n");
                if (!valid || tokens.empty())
                {
                    return error(-32602, "Command exceeds 64 tokens or 256 bytes per token");
                }
                const auto commandName = tokens.front();
                // Unknown commands would otherwise enter the console's deferred execution queue.
                auto* functor = console->FindCommand(commandName, AZ::ConsoleFunctorFlags::Null);
                if (!functor)
                {
                    failed = true;
                    data.AddMember("error", "Unknown command", allocator);
                }
                else if ((functor->GetFlags() & AZ::ConsoleFunctorFlags::ReadOnly) != AZ::ConsoleFunctorFlags::Null
                    && tokens.size() > 1)
                {
                    failed = true;
                    data.AddMember("error", "Read-only cvar", allocator);
                }
                else
                {
                    // Invoking a string/char cvar with no arguments can clear its value.
                    if (tokens.size() > 1 || functor->GetTypeId().IsNull())
                    {
                        auto* previous = s_capture;
                        s_capture = args.HasMember("output") && AZStd::string_view(args["output"].GetString()) == "none" ? nullptr : &capture;
                        const auto outcome = console->PerformCommand(tokens, AZ::ConsoleSilentMode::NotSilent,
                            AZ::ConsoleInvokedFrom::AzNetworking);
                        s_capture = previous;
                        failed = !outcome.IsSuccess();
                        if (failed)
                        {
                            data.AddMember("error", String(outcome.GetError(), allocator), allocator);
                        }
                    }
                    data.AddMember("executed", !failed, allocator);
                    AZ::CVarFixedString value;
                    if (functor->GetValue(value) == AZ::GetValueResult::Success)
                    {
                        data.AddMember("value", String(value, allocator), allocator);
                    }
                }
                Json output(rapidjson::kArrayType);
                for (const auto& line : capture.m_lines)
                {
                    if (!args.HasMember("grep") || AZStd::regex_search(line.m_message.c_str(), grep))
                    {
                        Json entry(rapidjson::kObjectType);
                        entry.AddMember("window", String(line.m_window, allocator), allocator);
                        entry.AddMember("message", String(line.m_message, allocator), allocator);
                        output.PushBack(entry, allocator);
                    }
                }
                data.AddMember("output", output, allocator);
                data.AddMember("truncated", capture.m_truncated, allocator);
                data.AddMember("sequenceBefore", sequenceBefore, allocator);
                AZStd::scoped_lock lock(m_logMutex);
                data.AddMember("sequenceAfter", m_sequence, allocator);
            }
            else if (name == "log_tag")
            {
                if (!IsString(args, "tag", AZ::MaxCVarStringLength) || args["tag"].GetStringLength() == 0
                    || (args.HasMember("enabled") && !args["enabled"].IsBool()))
                {
                    return error(-32602, "Expected tag and optional boolean enabled");
                }
                auto* logger = AZ::Interface<AZ::ILogger>::Get();
                if (!logger)
                {
                    failed = true;
                    data.AddMember("error", "AZ logger unavailable", allocator);
                }
                else
                {
                    if (args.HasMember("enabled"))
                    {
                        const AZ::ConsoleCommandContainer arguments{ args["tag"].GetString() };
                        const auto outcome = console->PerformCommand(args["enabled"].GetBool() ?
                            "LoggerSystemComponent.EnableLog" : "LoggerSystemComponent.DisableLog", arguments,
                            AZ::ConsoleSilentMode::NotSilent, AZ::ConsoleInvokedFrom::AzNetworking);
                        failed = !outcome.IsSuccess();
                        if (failed)
                        {
                            data.AddMember("error", String(outcome.GetError(), allocator), allocator);
                        }
                    }
                    data.AddMember("enabled", logger->IsTagEnabled(AZ::TypeHash32(args["tag"].GetString())), allocator);
                }
            }
            Json content(rapidjson::kArrayType);
            Json text(rapidjson::kObjectType);
            text.AddMember("type", "text", allocator);
            text.AddMember("text", String(Serialize(data), allocator), allocator);
            content.PushBack(text, allocator);
            result.AddMember("content", content, allocator);
            result.AddMember("isError", failed, allocator);
        }
        else
        {
            return error(-32601, "Method not found");
        }
        response.AddMember("result", result, allocator);
        return Serialize(response);
    }
}
