/*
 * Copyright (c) Contributors to the Open 3D Engine Project.
 * For complete copyright and license terms please see the LICENSE at the root of this distribution.
 *
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 *
 */

#include "LocalMcpServer.h"
#include "LocalMcpJson.h"
#include "LocalMcpSearch.h"

#include <AzCore/Console/IConsole.h>
#include <AzCore/Console/ILogger.h>
#include <AzCore/Platform.h>
#include <AzCore/Settings/SettingsRegistryMergeUtils.h>
#include <AzCore/StringFunc/StringFunc.h>
#include <AzCore/std/algorithm.h>
#include <AzCore/std/chrono/chrono.h>
#include <AzCore/std/containers/array.h>
#include <AzCore/std/containers/unordered_map.h>
#include <AzCore/std/sort.h>
#include <AzCore/std/string/conversions.h>
#include <AzCore/std/utility/charconv.h>
#include <AzNetworking/Utilities/CidrAddress.h>
#include <AzNetworking/Utilities/NetworkIncludes.h>

#include <openssl/crypto.h>
#include <openssl/err.h>
#include <openssl/ssl.h>

#include <algorithm>

namespace LocalMcp
{
    namespace
    {
        // HTTP framing.
        constexpr AZStd::string_view LineTerminator = "\r\n";
        constexpr AZStd::string_view HeaderTerminator = "\r\n\r\n";
        //! Largest valid request; a connection never buffers more unparsed input than this.
        constexpr size_t MaxBufferedInput = MaxHeaderSize + HeaderTerminator.size() + MaxBodySize;

        // The only accepted request target; anything else is 404.
        constexpr AZStd::string_view PostRequestLine = "POST /mcp HTTP/1.1";
        constexpr AZStd::string_view GetRequestLine = "GET /mcp HTTP/1.1";
        constexpr AZStd::string_view DeleteRequestLine = "DELETE /mcp HTTP/1.1";

        // MCP protocol versions: the latest is the default, the older ones are echoed back when a client asks for them.
        constexpr AZStd::string_view ProtocolVersion = "2025-11-25";
        constexpr AZStd::array<AZStd::string_view, 2> OlderProtocolVersions = { "2025-03-26", "2025-06-18" };

        // Response content types. An event stream is sent only for a request that changed the provider's tool list.
        constexpr AZStd::string_view JsonContentType = "application/json";
        constexpr AZStd::string_view EventStreamContentType = "text/event-stream";

        // Server-sent event framing. Serialized JSON has no raw line breaks, so each message fits one data line.
        constexpr AZStd::string_view EventPrefix = "event: message\ndata: ";
        constexpr AZStd::string_view EventTerminator = "\n\n";

        //! The notification that tells a client to list the tools again.
        constexpr AZStd::string_view ToolsListChangedNotification = R"({"jsonrpc":"2.0","method":"notifications/tools/list_changed"})";

        constexpr const char* JsonRpcVersion = "2.0";
        constexpr const char* DefaultServerName = "o3de-local-console";
        constexpr const char* ServerVersion = "0.2.0";

        // Connection handling per Poll().
        constexpr int MaxAcceptsPerPoll = 8;
        constexpr size_t MaxClients = 8;
        //! Bytes read from a connection per Poll().
        constexpr size_t ReceiveChunkSize = 4096;
        //! Bytes written to a connection per Poll().
        constexpr size_t MaxSendChunk = 16384;

        //! Only peers on this network may connect, whatever interface the listener is bound to.
        constexpr const char* LoopbackNetwork = "127.0.0.0/8";

        //! Upper bound on username plus password for basic authentication.
        constexpr size_t MaxCredentialsLength = 1024;

        //! console_execute output value that captures the command's printed lines.
        constexpr const char* OutputCapture = "capture";

        constexpr const char* EnableLogCommand = "LoggerSystemComponent.EnableLog";
        constexpr const char* DisableLogCommand = "LoggerSystemComponent.DisableLog";

        constexpr const char* Whitespace = " \t\r\n";

        AZStd::string_view ReasonPhrase(HttpStatus status)
        {
            switch (status)
            {
            case HttpStatus::Ok:
                return "OK";
            case HttpStatus::Accepted:
                return "Accepted";
            case HttpStatus::Unauthorized:
                return "Unauthorized";
            case HttpStatus::Forbidden:
                return "Forbidden";
            case HttpStatus::NotFound:
                return "Not Found";
            case HttpStatus::MethodNotAllowed:
                return "Method Not Allowed";
            case HttpStatus::ContentTooLarge:
                return "Content Too Large";
            case HttpStatus::UnsupportedMediaType:
                return "Unsupported Media Type";
            case HttpStatus::BadRequest:
            default:
                return "Bad Request";
            }
        }

        AZStd::string HttpResponse(
            HttpStatus status, AZStd::string_view body = {}, bool keepAlive = false, AZStd::string_view contentType = JsonContentType)
        {
            const AZStd::string_view reason = ReasonPhrase(status);
            AZStd::string response = AZStd::string::format(
                "HTTP/1.1 %d %.*s\r\nContent-Type: %.*s\r\nContent-Length: %zu\r\nConnection: %s\r\n",
                static_cast<int>(status),
                aznumeric_cast<int>(reason.size()),
                reason.data(),
                aznumeric_cast<int>(contentType.size()),
                contentType.data(),
                body.size(),
                keepAlive ? "keep-alive" : "close");

            if (status == HttpStatus::Unauthorized)
            {
                response += "WWW-Authenticate: Basic realm=\"LocalMcp\", charset=\"UTF-8\"\r\n";
            }
            if (status == HttpStatus::MethodNotAllowed)
            {
                response += "Allow: POST\r\n";
            }

            response += LineTerminator;
            response.append(body.data(), body.size());
            return response;
        }

        //! The list_changed notification followed by the JSON-RPC response, as two server-sent events.
        AZStd::string ToolsChangedEventStream(AZStd::string_view response)
        {
            AZStd::string stream(EventPrefix);
            stream += ToolsListChangedNotification;
            stream += EventTerminator;
            stream += EventPrefix;
            stream += response;
            stream += EventTerminator;
            return stream;
        }

        bool IsOlderProtocolVersion(AZStd::string_view version)
        {
            return AZStd::find(OlderProtocolVersions.begin(), OlderProtocolVersions.end(), version) != OlderProtocolVersions.end();
        }

        //! Header names are matched in lower case and limited to letters, digits and '-'.
        bool IsHeaderName(AZStd::string_view name)
        {
            for (const char character : name)
            {
                const bool valid = (character >= 'a' && character <= 'z') || (character >= '0' && character <= '9') || character == '-';
                if (!valid)
                {
                    return false;
                }
            }
            return true;
        }

        //! Returns the console, or reports it as unavailable in the tool reply and returns nullptr.
        AZ::IConsole* GetConsoleOrReportError(ToolCall& call)
        {
            AZ::IConsole* console = AZ::Interface<AZ::IConsole>::Get();
            if (!console)
            {
                call.m_data.AddMember("error", "AZ console unavailable", call.m_allocator);
            }
            return console;
        }

        //! Creates the server context for mutual TLS, or returns nullptr when a file fails to load.
        SSL_CTX* CreateMutualTlsContext(const Config& config)
        {
            SSL_CTX* context = SSL_CTX_new(TLS_server_method());
            if (!context)
            {
                return nullptr;
            }

            // Encrypted private keys are not supported: refuse to prompt for a passphrase.
            SSL_CTX_set_default_passwd_cb(
                context,
                [](char*, int, int, void*)
                {
                    return 0;
                });

            const bool loaded = SSL_CTX_set_min_proto_version(context, TLS1_2_VERSION) == 1 &&
                SSL_CTX_use_certificate_chain_file(context, config.m_certificate.c_str()) == 1 &&
                SSL_CTX_use_PrivateKey_file(context, config.m_privateKey.c_str(), SSL_FILETYPE_PEM) == 1 &&
                SSL_CTX_check_private_key(context) == 1 && SSL_CTX_load_verify_locations(context, config.m_ca.c_str(), nullptr) == 1;
            if (!loaded)
            {
                SSL_CTX_free(context);
                return nullptr;
            }

            SSL_CTX_set_verify(context, SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT, nullptr);
            SSL_CTX_set_session_cache_mode(context, SSL_SESS_CACHE_OFF);
            SSL_CTX_set_options(context, SSL_OP_NO_TICKET);
            return context;
        }

        //! Accepted connection, optionally wrapped in TLS.
        class ClientSocket final : public AzNetworking::TcpSocket
        {
        public:
            enum class Handshake
            {
                Failed,
                InProgress,
                Done
            };

            //! Takes ownership of fd. With a TLS context, the socket is closed if the TLS session cannot be created.
            ClientSocket(AzNetworking::SocketFd fd, SSL_CTX* tlsContext)
                : TcpSocket(fd)
            {
                if (tlsContext && IsOpen())
                {
                    m_ssl = SSL_new(tlsContext);
                    if (!m_ssl || SSL_set_fd(m_ssl, static_cast<int>(fd)) != 1)
                    {
                        Close();
                    }
                }
            }

            ~ClientSocket() override
            {
                SSL_free(m_ssl);
            }

            //! Advances the TLS handshake without blocking. A plain connection is Done at once.
            Handshake ContinueHandshake()
            {
                if (!IsOpen())
                {
                    return Handshake::Failed;
                }
                if (!m_ssl || SSL_is_init_finished(m_ssl))
                {
                    return Handshake::Done;
                }

                ERR_clear_error();
                const int result = SSL_accept(m_ssl);
                if (result == 1)
                {
                    return Handshake::Done;
                }
                return TranslateSslResult(result) == 0 ? Handshake::InProgress : Handshake::Failed;
            }

        protected:
            // AzNetworking::TcpSocket overrides
            int32_t SendInternal(const uint8_t* data, uint32_t size) const override
            {
                if (!m_ssl)
                {
                    return TcpSocket::SendInternal(data, size);
                }

                ERR_clear_error();
                const int result = SSL_write(m_ssl, data, aznumeric_cast<int>(size));
                return result > 0 ? result : TranslateSslResult(result);
            }

            int32_t ReceiveInternal(uint8_t* data, uint32_t size) const override
            {
                if (!m_ssl)
                {
                    return TcpSocket::ReceiveInternal(data, size);
                }

                ERR_clear_error();
                const int result = SSL_read(m_ssl, data, aznumeric_cast<int>(size));
                return result > 0 ? result : TranslateSslResult(result);
            }

        private:
            //! Maps a failed SSL call to the TcpSocket convention: 0 to retry later, -1 on error.
            int32_t TranslateSslResult(int result) const
            {
                const int error = SSL_get_error(m_ssl, result);
                return error == SSL_ERROR_WANT_READ || error == SSL_ERROR_WANT_WRITE ? 0 : -1;
            }

            SSL* m_ssl = nullptr;
        };
    } // namespace

    bool ReadConfig(AZ::SettingsRegistryInterface& registry, Config& config, AZStd::string_view settingsRoot, const Config& defaults)
    {
        config = defaults;
        const AZStd::string root(settingsRoot);

        // An optional .ini file is merged under root before the individual settings are read.
        AZStd::string file;
        const AZStd::string configFileKey = root + "/ConfigFile";
        if (registry.GetType(configFileKey) != AZ::SettingsRegistryInterface::Type::NoType && !registry.Get(file, configFileKey))
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

        // A missing setting keeps its default; a setting of the wrong type fails.
        auto read = [&](auto& value, const char* key)
        {
            const AZStd::string path = root + "/" + key;
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
        const bool typesValid = read(config.m_enabled, "Enabled") && read(config.m_port, "Port") && read(config.m_auth, "Auth") &&
            read(config.m_username, "Username") && read(config.m_password, "Password") && read(config.m_certificate, "Certificate") &&
            read(config.m_privateKey, "PrivateKey") && read(config.m_ca, "CA") && read(config.m_logLines, "LogLines") &&
            read(config.m_idleTimeoutMs, "IdleTimeoutMs");

        const bool rangesValid = config.m_port > 0 && config.m_port <= MaxPort && config.m_logLines >= MinLogLines &&
            config.m_logLines <= MaxLogLines && config.m_idleTimeoutMs >= MinIdleTimeoutMs && config.m_idleTimeoutMs <= MaxIdleTimeoutMs;

        const bool valid = typesValid && rangesValid;
        AZ_Warning("LocalMcp", valid, "Invalid configuration type, port, LogLines or IdleTimeoutMs.");
        return valid;
    }

    //! One HTTP connection and the request it is working on.
    struct Server::Client
    {
        Client(AzNetworking::SocketFd fd, SSL_CTX* tlsContext, AZ::s64 idleTimeoutMs)
            : m_socket(fd, tlsContext)
            , m_receiveBuffer(ReceiveChunkSize)
        {
            ExtendDeadline(idleTimeoutMs);
        }

        void ExtendDeadline(AZ::s64 idleTimeoutMs)
        {
            m_deadline = AZStd::chrono::steady_clock::now() + AZStd::chrono::milliseconds(idleTimeoutMs);
        }

        ClientSocket m_socket;
        //! Reused for every read, so network data never lands in a stack buffer.
        AZStd::vector<uint8_t> m_receiveBuffer;
        //! Received bytes not yet consumed by a complete request; at most MaxBufferedInput.
        AZStd::string m_input;
        //! Response being sent, and how much of it has been sent.
        AZStd::string m_output;
        size_t m_sent = 0;
        //! Bytes of m_input used by the request that produced m_output.
        size_t m_consumed = 0;
        bool m_keepAlive = false;
        //! Held logs_wait, or 0.
        uint64_t m_waitId = 0;
        //! A provider tool call is held on this connection.
        bool m_providerPending = false;
        AZStd::chrono::steady_clock::time_point m_deadline;
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
        const bool listening = ::bind(fd, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 0 &&
            ::listen(fd, SOMAXCONN) == 0 && AzNetworking::SetSocketNonBlocking(GetSocketFd()) &&
            AzNetworking::SetSocketNoDelay(GetSocketFd());
        if (!listening)
        {
            Close();
            return false;
        }
        return true;
    }

    Server::Server(ToolProvider* provider)
        : m_provider(provider)
    {
    }

    Server::~Server()
    {
        Stop();
    }

    bool Server::Start(const Config& config)
    {
        Stop();

        // Port 0 is allowed here so that tests can ask for a free port.
        const bool rangesValid = config.m_port >= 0 && config.m_port <= MaxPort && config.m_logLines >= MinLogLines &&
            config.m_logLines <= MaxLogLines && config.m_idleTimeoutMs >= MinIdleTimeoutMs && config.m_idleTimeoutMs <= MaxIdleTimeoutMs;
        const bool authKnown = config.m_auth == AuthNone || config.m_auth == AuthBasic || config.m_auth == AuthMutualTls;
        if (!rangesValid || !authKnown)
        {
            return false;
        }

        if (config.m_auth == AuthBasic)
        {
            // The username cannot hold ':' because it separates the username from the password.
            const bool credentialsValid = !config.m_username.empty() && !config.m_password.empty() &&
                config.m_username.find(':') == AZStd::string::npos &&
                config.m_username.size() + config.m_password.size() <= MaxCredentialsLength;
            if (!credentialsValid)
            {
                return false;
            }

            const AZStd::string credentials = config.m_username + ":" + config.m_password;
            m_authorization =
                "Basic " + AZ::StringFunc::Base64::Encode(reinterpret_cast<const AZ::u8*>(credentials.data()), credentials.size());
        }

        if (config.m_auth == AuthMutualTls)
        {
            m_tlsContext = CreateMutualTlsContext(config);
            if (!m_tlsContext)
            {
                Stop();
                return false;
            }
        }

        m_socketLayerInitialized = AzNetworking::SocketLayerInit();
        const uint16_t port = aznumeric_cast<uint16_t>(config.m_port);
        const bool listening =
            m_socketLayerInitialized && (config.m_loopbackOnly ? m_listener.ListenLoopback(port) : m_listener.Listen(port));
        if (!listening)
        {
            Stop();
            return false;
        }

        // Read back the bound port, which differs from config.m_port when that is 0.
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

        for (auto& client : m_clients)
        {
            ReleasePending(*client);
        }
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
            // Shutdown is the last opportunity to audit retention before this volatile history is released.
            const uint64_t first = m_logs.empty() ? m_sequence + 1 : m_logs.front().m_sequence;
            AZ_TracePrintf(
                "LocalMcp",
                "History at shutdown: logLines=%zu sequence=%llu firstSequence=%llu dropped=%llu\n",
                m_logLines,
                m_sequence,
                first,
                first - 1);
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

        for (int attempt = 0; attempt < MaxAcceptsPerPoll && m_clients.size() < MaxClients; ++attempt)
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
            const AzNetworking::CidrAddress loopback(LoopbackNetwork);
            if (!loopback.IsMatch(ntohl(peer.sin_addr.s_addr)))
            {
                AzNetworking::CloseSocket(fd);
                continue;
            }

            m_clients.push_back(AZStd::make_unique<Client>(fd, m_tlsContext, m_idleTimeoutMs));
        }

        // AZStd::remove_if copies elements (it is built on remove_copy_if), so move-only clients need std::remove_if.
        const auto finished = std::remove_if(
            m_clients.begin(),
            m_clients.end(),
            [this](const AZStd::unique_ptr<Client>& client)
            {
                return !ServiceClient(*client);
            });
        m_clients.erase(finished, m_clients.end());
    }

    bool Server::ServiceClient(Client& client)
    {
        if (AZStd::chrono::steady_clock::now() >= client.m_deadline)
        {
            ReleasePending(client);
            return false;
        }

        const ClientSocket::Handshake handshake = client.m_socket.ContinueHandshake();
        if (handshake == ClientSocket::Handshake::Failed)
        {
            ReleasePending(client);
            return false;
        }
        if (handshake == ClientSocket::Handshake::InProgress)
        {
            return true;
        }

        if (client.m_output.empty())
        {
            // A held logs_wait is not answered until it matches or times out.
            if (client.m_waitId)
            {
                AZStd::scoped_lock lock(m_logMutex);
                const Wait& wait = m_waits.at(client.m_waitId);
                if (!wait.m_match.m_sequence && AZStd::chrono::steady_clock::now() < wait.m_until)
                {
                    return true;
                }
            }

            const int32_t received =
                client.m_socket.Receive(client.m_receiveBuffer.data(), aznumeric_cast<uint32_t>(client.m_receiveBuffer.size()));
            if (received < 0)
            {
                ReleasePending(client);
                return false;
            }

            const size_t receivedSize = aznumeric_cast<size_t>(received);
            if (client.m_input.size() + receivedSize > MaxBufferedInput)
            {
                // Never buffer more than one largest request: answer and close the connection.
                ReleasePending(client);
                client.m_input.clear();
                client.m_keepAlive = false;
                client.m_output = HttpResponse(HttpStatus::ContentTooLarge);
            }
            else
            {
                client.m_input.append(reinterpret_cast<const char*>(client.m_receiveBuffer.data()), receivedSize);
                if (receivedSize > 0)
                {
                    client.ExtendDeadline(m_idleTimeoutMs);
                }

                // Re-run the request parser over everything buffered; a held request is re-dispatched to resume it.
                client.m_consumed = 0;
                client.m_keepAlive = false;
                client.m_output = HandleHttp(client.m_input, &client);
                if (!client.m_output.empty())
                {
                    client.m_input.erase(0, client.m_consumed);
                    client.ExtendDeadline(m_idleTimeoutMs);
                }
            }
        }

        if (client.m_output.empty())
        {
            return true;
        }

        const size_t size = AZStd::min(client.m_output.size() - client.m_sent, MaxSendChunk);
        const int32_t sent =
            client.m_socket.Send(reinterpret_cast<const uint8_t*>(client.m_output.data() + client.m_sent), aznumeric_cast<uint32_t>(size));
        if (sent < 0)
        {
            ReleasePending(client);
            return false;
        }

        client.m_sent += aznumeric_cast<size_t>(sent);
        if (sent > 0)
        {
            client.ExtendDeadline(m_idleTimeoutMs);
        }

        if (client.m_sent == client.m_output.size())
        {
            if (!client.m_keepAlive)
            {
                return false;
            }
            client.m_output.clear();
            client.m_sent = 0;
        }
        return true;
    }

    AZStd::string Server::HandleHttp(AZStd::string_view request, Client* client)
    {
        // Wait for the whole header block.
        const size_t headerEnd = request.find(HeaderTerminator);
        if (headerEnd == AZStd::string_view::npos)
        {
            return request.size() > MaxHeaderSize ? HttpResponse(HttpStatus::ContentTooLarge) : AZStd::string{};
        }
        if (headerEnd > MaxHeaderSize)
        {
            return HttpResponse(HttpStatus::ContentTooLarge);
        }

        const size_t firstLineEnd = request.find(LineTerminator);
        const AZStd::string_view firstLine = request.substr(0, firstLineEnd);
        if (firstLine != PostRequestLine && firstLine != GetRequestLine && firstLine != DeleteRequestLine)
        {
            return HttpResponse(HttpStatus::NotFound);
        }

        // Parse the headers into a map keyed by lower-case name. Duplicate headers are rejected.
        AZStd::unordered_map<AZStd::string, AZStd::string_view> headers;
        for (size_t position = firstLineEnd + LineTerminator.size(); position < headerEnd;)
        {
            const size_t end = request.find(LineTerminator, position);
            const AZStd::string_view line = request.substr(position, end - position);
            const size_t colon = line.find(':');
            if (colon == AZStd::string_view::npos || colon == 0)
            {
                return HttpResponse(HttpStatus::BadRequest);
            }

            AZStd::string key(line.substr(0, colon));
            AZStd::to_lower(key.begin(), key.end());
            if (!IsHeaderName(key))
            {
                return HttpResponse(HttpStatus::BadRequest);
            }

            const AZStd::string_view value = AZ::StringFunc::StripEnds(line.substr(colon + 1), " \t");
            const bool hasControlCharacter = value.find_first_of("\r\n\0", 0, 3) != AZStd::string_view::npos;
            if (hasControlCharacter || !headers.emplace(key, value).second)
            {
                return HttpResponse(HttpStatus::BadRequest);
            }
            position = end + LineTerminator.size();
        }

        // Host and Origin must name this server, which blocks DNS rebinding and cross-site browser requests.
        const AZStd::string port = AZStd::string::format(":%u", m_port);
        const AZStd::string_view host = headers["host"];
        if (host != "127.0.0.1" + port && host != "localhost" + port)
        {
            return HttpResponse(HttpStatus::Forbidden);
        }
        if (headers.contains("origin"))
        {
            const AZStd::string scheme = m_tlsContext ? "https://" : "http://";
            const AZStd::string_view origin = headers["origin"];
            if (origin != scheme + "127.0.0.1" + port && origin != scheme + "localhost" + port)
            {
                return HttpResponse(HttpStatus::Forbidden);
            }
        }

        if (firstLine != PostRequestLine)
        {
            return HttpResponse(HttpStatus::MethodNotAllowed);
        }

        const AZStd::string_view version = headers["mcp-protocol-version"];
        if (!version.empty() && version != ProtocolVersion && !IsOlderProtocolVersion(version))
        {
            return HttpResponse(HttpStatus::BadRequest);
        }

        const AZStd::string_view contentType = headers["content-type"];
        if (contentType != "application/json" && !contentType.starts_with("application/json;"))
        {
            return HttpResponse(HttpStatus::UnsupportedMediaType);
        }

        // Only a plain Content-Length body is supported.
        if (headers.contains("transfer-encoding") || headers.contains("expect") || !headers.contains("content-length"))
        {
            return HttpResponse(HttpStatus::BadRequest);
        }

        const AZStd::string_view length = headers["content-length"];
        const char* lengthEnd = length.data() + length.size();
        size_t bodySize = 0;
        const AZStd::from_chars_result parsed = AZStd::from_chars(length.data(), lengthEnd, bodySize);
        if (parsed.ec == AZStd::errc::result_out_of_range || (parsed.ec == AZStd::errc{} && bodySize > MaxBodySize))
        {
            return HttpResponse(HttpStatus::ContentTooLarge);
        }
        if (parsed.ec != AZStd::errc{} || parsed.ptr != lengthEnd)
        {
            return HttpResponse(HttpStatus::BadRequest);
        }

        // Wait for the whole body.
        const size_t bodyStart = headerEnd + HeaderTerminator.size();
        if (request.size() - bodyStart < bodySize)
        {
            return {};
        }

        // Compare in constant time so the response time does not reveal how much of the credential matched.
        if (!m_authorization.empty())
        {
            const AZStd::string_view authorization = headers["authorization"];
            if (authorization.size() != m_authorization.size() ||
                CRYPTO_memcmp(authorization.data(), m_authorization.data(), authorization.size()) != 0)
            {
                return HttpResponse(HttpStatus::Unauthorized);
            }
        }

        HttpStatus status = HttpStatus::Ok;
        const AZ::u64 toolsRevision = m_provider ? m_provider->GetToolsRevision() : 0;
        const AZStd::string body = HandleRpc(
            request.substr(bodyStart, bodySize),
            status,
            client ? &client->m_waitId : nullptr,
            client ? &client->m_providerPending : nullptr);

        // A held request is answered by a later Poll().
        if (client && (client->m_waitId || client->m_providerPending))
        {
            return {};
        }

        const bool keepAlive = client && AZ::StringFunc::Equal(headers["connection"], "keep-alive");
        if (client)
        {
            client->m_consumed = bodyStart + bodySize;
            client->m_keepAlive = keepAlive;
        }

        // A request that changed the tool list is answered with the list_changed notification before its response, when
        // the client accepts an event stream. Any other client lists the tools again on its own.
        const bool toolsChanged = m_provider && m_provider->GetToolsRevision() != toolsRevision;
        if (toolsChanged && !body.empty() && AZ::StringFunc::Contains(headers["accept"], EventStreamContentType))
        {
            return HttpResponse(status, ToolsChangedEventStream(body), keepAlive, EventStreamContentType);
        }
        return HttpResponse(status, body, keepAlive);
    }

    AZStd::string Server::HandleRpc(AZStd::string_view body, HttpStatus& status, uint64_t* waitId, bool* providerPending)
    {
        rapidjson::Document request;
        request.Parse<rapidjson::kParseValidateEncodingFlag | rapidjson::kParseIterativeFlag>(body.data(), body.size());

        rapidjson::Document response(rapidjson::kObjectType);
        auto& allocator = response.GetAllocator();
        response.AddMember("jsonrpc", rapidjson::StringRef(JsonRpcVersion), allocator);
        response.AddMember("id", Json(), allocator);

        auto error = [&](JsonRpcError code, const char* message)
        {
            Json detail(rapidjson::kObjectType);
            detail.AddMember("code", static_cast<int>(code), allocator);
            detail.AddMember("message", String(message, allocator), allocator);
            response.AddMember("error", detail, allocator);
            return Serialize(response);
        };

        // Validate the JSON-RPC envelope.
        if (request.HasParseError())
        {
            return error(JsonRpcError::ParseError, "Parse error");
        }
        if (!request.IsObject() || !IsString(request, "jsonrpc") || AZStd::string_view(request["jsonrpc"].GetString()) != JsonRpcVersion ||
            !IsString(request, "method"))
        {
            return error(JsonRpcError::InvalidRequest, "Invalid request");
        }

        // A notification has no id and gets no reply body.
        if (!request.HasMember("id"))
        {
            status = HttpStatus::Accepted;
            return {};
        }

        const Json& id = request["id"];
        if (!id.IsString() && !id.IsInt64() && !id.IsUint64())
        {
            return error(JsonRpcError::InvalidRequest, "Invalid request id");
        }
        response["id"].CopyFrom(id, allocator);

        const AZStd::string_view method(request["method"].GetString());
        const Json empty(rapidjson::kObjectType);
        const Json& params = request.HasMember("params") ? request["params"] : empty;
        if (!params.IsObject())
        {
            return error(JsonRpcError::InvalidParams, "Expected object params");
        }

        Json result(rapidjson::kObjectType);
        if (method == "initialize")
        {
            const bool paramsValid = IsString(params, "protocolVersion") && params.HasMember("capabilities") &&
                params["capabilities"].IsObject() && params.HasMember("clientInfo") && params["clientInfo"].IsObject();
            if (!paramsValid)
            {
                return error(JsonRpcError::InvalidParams, "Expected protocolVersion, capabilities and clientInfo");
            }

            const AZStd::string_view requestedVersion(params["protocolVersion"].GetString());
            const AZStd::string_view version = IsOlderProtocolVersion(requestedVersion) ? requestedVersion : ProtocolVersion;
            result.AddMember("protocolVersion", String(version, allocator), allocator);

            Json tools(rapidjson::kObjectType);
            tools.AddMember("listChanged", true, allocator);
            Json capabilities(rapidjson::kObjectType);
            capabilities.AddMember("tools", tools, allocator);
            result.AddMember("capabilities", capabilities, allocator);

            Json info(rapidjson::kObjectType);
            info.AddMember("name", String(m_provider ? m_provider->GetServerName() : DefaultServerName, allocator), allocator);
            info.AddMember("version", rapidjson::StringRef(ServerVersion), allocator);
            Json process(rapidjson::kObjectType);
            process.AddMember("pid", AZ::Platform::GetCurrentProcessId(), allocator);
            info.AddMember("process", process, allocator);

            result.AddMember("idleTimeoutMs", m_idleTimeoutMs, allocator);
            result.AddMember("serverInfo", info, allocator);
        }
        else if (method == "ping")
        {
            // The reply is an empty result.
        }
        else if (method == "tools/list")
        {
            rapidjson::Document tools;
            tools.Parse(RuntimeToolsSchemas);
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
                return error(JsonRpcError::InvalidParams, "Expected tool name and object arguments");
            }

            const AZStd::string_view name(params["name"].GetString());
            const Json& args = params.HasMember("arguments") ? params["arguments"] : empty;
            Json data(rapidjson::kObjectType);
            ToolCall call{ data, allocator, {}, waitId };
            ToolProvider::Result invoked = Dispatch(name, args, call);
            if (invoked == ToolProvider::Result::Complete && waitId && *waitId)
            {
                // A held logs_wait replies when the connection is polled again.
                return {};
            }

            if (invoked == ToolProvider::Result::Unknown && m_provider)
            {
                // Every other tool belongs to the provider. A pending call is resumed instead of invoked again.
                const bool resuming = providerPending && *providerPending;
                invoked = resuming ? m_provider->PollPending(data, allocator, call.m_error)
                                   : m_provider->Invoke(name, args, data, allocator, call.m_error);
                if (invoked == ToolProvider::Result::Pending)
                {
                    if (!providerPending)
                    {
                        m_provider->CancelPending();
                        return error(JsonRpcError::InvalidParams, "Pending tools require a live HTTP connection");
                    }
                    *providerPending = true;
                    return {};
                }
                if (providerPending)
                {
                    *providerPending = false;
                }
            }

            if (invoked == ToolProvider::Result::Unknown)
            {
                return error(JsonRpcError::InvalidParams, "Unknown tool");
            }
            if (invoked == ToolProvider::Result::InvalidArguments)
            {
                return error(JsonRpcError::InvalidParams, call.m_error.c_str());
            }

            // An image in the reply travels as its own content item after the text.
            Json image;
            if (data.IsObject() && data.HasMember(ImageReplyMember))
            {
                image = data[ImageReplyMember];
                data.RemoveMember(ImageReplyMember);
            }

            // The tool reply travels as JSON text in a single text content item. fields=name replies are arrays.
            const bool failed = data.IsObject() && data.HasMember("error");
            Json text(rapidjson::kObjectType);
            text.AddMember("type", "text", allocator);
            text.AddMember("text", String(Serialize(data), allocator), allocator);
            Json content(rapidjson::kArrayType);
            content.PushBack(text, allocator);

            const bool imageValid = image.IsObject() && image.HasMember("data") && image["data"].IsString() &&
                image.HasMember("mimeType") && image["mimeType"].IsString();
            if (imageValid)
            {
                Json item(rapidjson::kObjectType);
                item.AddMember("type", "image", allocator);
                item.AddMember("data", image["data"], allocator);
                item.AddMember("mimeType", image["mimeType"], allocator);
                content.PushBack(item, allocator);
            }
            result.AddMember("content", content, allocator);
            result.AddMember("isError", failed, allocator);
        }
        else
        {
            return error(JsonRpcError::MethodNotFound, "Method not found");
        }

        response.AddMember("result", result, allocator);
        return Serialize(response);
    }

    bool Server::HandleConsoleComplete(const ConsoleCompleteArgs& args, ToolCall& call)
    {
        AZ::IConsole* console = GetConsoleOrReportError(call);
        if (!console)
        {
            return true;
        }

        // An empty prefix lists every visible functor; otherwise the console completes the command and its arguments.
        const AZStd::string prefix = args.m_prefix.value_or("");
        AZStd::vector<AZStd::string> matches;
        AZStd::string completion(prefix);
        if (prefix.empty())
        {
            console->VisitRegisteredFunctors(
                [&matches](AZ::ConsoleFunctorBase* functor)
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

        // Describe each match whose first token names a functor.
        Json list(rapidjson::kArrayType);
        for (const AZStd::string& match : matches)
        {
            Json entry(rapidjson::kObjectType);
            entry.AddMember("name", String(match, call.m_allocator), call.m_allocator);

            const AZStd::string_view commandName = AZStd::string_view(match).substr(0, match.find_first_of(Whitespace));
            AZ::ConsoleFunctorBase* functor = commandName.size() <= AZ::MaxCVarStringLength ? console->FindCommand(commandName) : nullptr;
            if (functor)
            {
                entry.AddMember("description", String(functor->GetDesc(), call.m_allocator), call.m_allocator);
                entry.AddMember("kind", String(GetFunctorKind(*functor), call.m_allocator), call.m_allocator);

                AZ::CVarFixedString value;
                if (functor->GetValue(value) == AZ::GetValueResult::Success)
                {
                    entry.AddMember("value", String(value, call.m_allocator), call.m_allocator);
                }
            }
            list.PushBack(entry, call.m_allocator);
        }

        call.m_data.AddMember("completion", String(completion, call.m_allocator), call.m_allocator);
        call.m_data.AddMember("matches", list, call.m_allocator);
        return true;
    }

    bool Server::HandleConsoleExecute(const ConsoleExecuteArgs& args, ToolCall& call)
    {
        if (AZ::StringFunc::StripEnds(args.m_command, Whitespace).empty())
        {
            call.m_error = "Expected a nonempty command";
            return false;
        }

        AZStd::regex grep(args.m_grep.value_or("").c_str());
        if (!grep.Valid())
        {
            call.m_error = "Invalid grep regex";
            return false;
        }

        // Split the command line into the console's fixed-capacity token list.
        AZ::ConsoleCommandContainer tokens;
        bool tokensFit = true;
        AZ::StringFunc::TokenizeVisitor(
            args.m_command,
            [&](AZStd::string_view token)
            {
                if (tokens.size() == tokens.capacity() || token.size() > AZ::MaxCVarStringLength)
                {
                    tokensFit = false;
                }
                else
                {
                    tokens.push_back(token);
                }
            },
            Whitespace);
        if (!tokensFit)
        {
            call.m_error =
                AZStd::string::format("Command exceeds %zu tokens or %zu bytes per token", tokens.capacity(), AZ::MaxCVarStringLength);
            return false;
        }

        AZ::IConsole* console = GetConsoleOrReportError(call);
        if (!console)
        {
            return true;
        }

        Capture capture;
        capture.m_server = this;
        uint64_t sequenceBefore;
        {
            AZStd::scoped_lock lock(m_logMutex);
            sequenceBefore = m_sequence;
        }

        // Look the command up first: unknown commands would otherwise enter the console's deferred execution queue.
        AZ::ConsoleFunctorBase* functor = console->FindCommand(tokens.front(), AZ::ConsoleFunctorFlags::Null);
        const bool readOnly = functor && (functor->GetFlags() & AZ::ConsoleFunctorFlags::ReadOnly) != AZ::ConsoleFunctorFlags::Null;
        if (!functor)
        {
            call.m_data.AddMember("error", "Unknown command", call.m_allocator);
        }
        else if (readOnly && tokens.size() > 1)
        {
            call.m_data.AddMember("error", "Read-only cvar", call.m_allocator);
        }
        else
        {
            // A bare cvar name only reads it: invoking a string or char cvar with no arguments can clear its value.
            bool failed = false;
            if (tokens.size() > 1 || functor->GetTypeId().IsNull())
            {
                const bool captureOutput = args.m_output.value_or(OutputCapture) == OutputCapture;
                Capture* previous = s_capture;
                s_capture = captureOutput ? &capture : nullptr;
                const AZ::PerformCommandResult outcome =
                    console->PerformCommand(tokens, AZ::ConsoleSilentMode::NotSilent, AZ::ConsoleInvokedFrom::AzNetworking);
                s_capture = previous;

                failed = !outcome.IsSuccess();
                if (failed)
                {
                    call.m_data.AddMember("error", String(outcome.GetError(), call.m_allocator), call.m_allocator);
                }
            }
            call.m_data.AddMember("executed", !failed, call.m_allocator);

            AZ::CVarFixedString value;
            if (functor->GetValue(value) == AZ::GetValueResult::Success)
            {
                call.m_data.AddMember("value", String(value, call.m_allocator), call.m_allocator);
            }
        }

        Json output(rapidjson::kArrayType);
        for (const LogLine& line : capture.m_lines)
        {
            if (!args.m_grep || AZStd::regex_search(line.m_message.c_str(), grep))
            {
                Json entry(rapidjson::kObjectType);
                entry.AddMember("window", String(line.m_window, call.m_allocator), call.m_allocator);
                entry.AddMember("message", String(line.m_message, call.m_allocator), call.m_allocator);
                output.PushBack(entry, call.m_allocator);
            }
        }
        call.m_data.AddMember("output", output, call.m_allocator);
        call.m_data.AddMember("truncated", capture.m_truncated, call.m_allocator);
        call.m_data.AddMember("sequenceBefore", sequenceBefore, call.m_allocator);

        AZStd::scoped_lock lock(m_logMutex);
        call.m_data.AddMember("sequenceAfter", m_sequence, call.m_allocator);
        return true;
    }

    bool Server::HandleLogTag(const LogTagArgs& args, ToolCall& call)
    {
        AZ::IConsole* console = GetConsoleOrReportError(call);
        if (!console)
        {
            return true;
        }

        AZ::ILogger* logger = AZ::Interface<AZ::ILogger>::Get();
        if (!logger)
        {
            call.m_data.AddMember("error", "AZ logger unavailable", call.m_allocator);
            return true;
        }

        // Without enabled this only reports the tag's state.
        if (args.m_enabled)
        {
            const AZ::ConsoleCommandContainer arguments{ args.m_tag };
            const AZ::PerformCommandResult outcome = console->PerformCommand(
                *args.m_enabled ? EnableLogCommand : DisableLogCommand,
                arguments,
                AZ::ConsoleSilentMode::NotSilent,
                AZ::ConsoleInvokedFrom::AzNetworking);
            if (!outcome.IsSuccess())
            {
                call.m_data.AddMember("error", String(outcome.GetError(), call.m_allocator), call.m_allocator);
            }
        }

        call.m_data.AddMember("enabled", logger->IsTagEnabled(AZ::TypeHash32(args.m_tag.c_str())), call.m_allocator);
        return true;
    }
} // namespace LocalMcp
