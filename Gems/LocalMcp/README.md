# LocalMcp

Small, optional Windows gem exposing the **AZ console** over MCP. Uses AzNetworking's
TCP socket and the engine's existing RapidJSON/OpenSSL dependencies. No MCP SDK,
HTTP library, worker thread, or engine source changes.

## Enable and configure

Enable `LocalMcp` in the desired project's `gem_names`, then reconfigure and build.
The gem serves clients, servers, unified launchers and editor/tools, but not asset
builders. It is **disabled by default**. Each process needs its own port.

Put this in your project's `Registry/localmcp.setreg`:

```json
{
    "O3DE": {
        "Gems": {
            "LocalMcp": {
                "Enabled": true,
                "Port": 4600,
                "Auth": "none"
            }
        }
    }
}
```

Or use a cfg/ini file with `key=value` entries and no section header (see
`localmcp.ini.example`), and point to it in settings:

```json
{"O3DE":{"Gems":{"LocalMcp":{"ConfigFile":"C:/private/localmcp.ini"}}}}
```

The cfg/ini is merged over the registry once during activation. Relative config
paths resolve against the project root. Use absolute PEM paths. Restart the process
to change configuration. Configuration is read through the engine's existing
settings parser; quote numeric-looking string values such as passwords in cfg/ini.

| Setting | Default | Meaning |
| --- | --- | --- |
| `Enabled` | `false` | Start the listener |
| `Port` | `4600` | TCP port, 1–65535 |
| `Auth` | `none` | `none`, `basic`, or `mtls` |
| `Username`, `Password` | empty | Both required for basic auth; username cannot contain `:` |
| `Certificate`, `PrivateKey` | empty | Server PEM certificate chain and unencrypted PEM private key for mTLS |
| `CA` | empty | PEM CA bundle used to verify client certificates; required for mTLS |
| `ConfigFile` | empty | Optional cfg/ini path |
| `LogLines` | `512` | Ring capacity, 1–1048576 lines |
| `IdleTimeoutMs` | `10000` | Per-connection idle deadline, 1000–3600000 ms; receive/send progress resets it |

Invalid auth/certificates fail closed. Basic auth uses HTTP `Authorization: Basic`
and runs over plaintext HTTP; `none` permits any local process. mTLS uses HTTPS,
requires a valid client certificate signed by `CA`, and verifies it before handling
HTTP. Clients must trust the server CA and verify its hostname. Include `localhost`
and/or `127.0.0.1` in the server certificate's SANs. Every certificate trusted by
the configured client CA gets full console access.

## Connect

Endpoint: `http://127.0.0.1:4600/mcp` (`https://...` for mTLS).

Implements the JSON response form of [MCP Streamable HTTP](https://modelcontextprotocol.io/specification/2025-11-25/basic/transports):
`initialize`, `notifications/initialized`, `ping`, `tools/list`, and `tools/call`.
Negotiates protocol versions `2025-03-26`, `2025-06-18`, and `2025-11-25`.
Send one JSON-RPC message per POST with `Content-Type: application/json`,
`Content-Length`, and `Accept: application/json, text/event-stream`. Use the
negotiated `MCP-Protocol-Version` header on subsequent requests.

For example, initialize with:

```sh
curl http://127.0.0.1:4600/mcp -H 'Content-Type: application/json' \
  -H 'Accept: application/json, text/event-stream' \
  -d '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-11-25","capabilities":{},"clientInfo":{"name":"local","version":"1"}}}'
```

Add `-u developer:password` for basic auth, or
`--cacert server-ca.pem --cert client.pem --key client.key` for mTLS.

Call a tool with:

```json
{"jsonrpc":"2.0","id":2,"method":"tools/call","params":{"name":"console_complete","arguments":{"prefix":"r_"}}}
```

| Tool | Arguments | Result |
| --- | --- | --- |
| `console_complete` | optional `prefix` | Completion and matching names, help, kinds and cvar values. Empty prefix lists visible functors; argument completion uses registered callbacks. |
| `console_execute` | `command` | Executes a command or reads/sets a cvar; returns execution status/value. Read-only variables cannot be changed. |
| `logs_tail` | optional `after`, `limit` (1–128; default 100) | Recent lines with sequence, window, message, `next` cursor and overwritten `dropped` count. Poll with `after=next`. |
| `log_tag` | `tag`, optional `enabled` | Queries or sets an AZLOG tag through existing logger console commands. |

Tool data is JSON in MCP text content. Command output is also available through
`logs_tail`. For logging level, use `console_execute` with
`LoggerSystemComponent.SetLevel 1` (Trace) through `7` (Fatal).

## Extended tools

`initialize` reports `serverInfo.process.pid` and `idleTimeoutMs`. Tool arguments are advertised in
`Code/Source/LocalMcpTools.h`; existing arguments remain valid.

- `console_execute`: optional `output: capture|none` (capture by default), `grep` (ECMAScript regex).
  Returns `output` (window/message), `truncated` (lines beyond the 128-line capture cap), and
  `sequenceBefore`/`sequenceAfter`. Capture includes only synchronous output on the executing thread;
  messages are capped at 2048 bytes and each line enters the shared ring once.
- `logs_tail`: optional `pattern`, `regex` (default true, false for substring), `windows`,
  `excludeWindows`, `tags`, `level` (error/warning/info), `fields` (message/compact), `collapse`, `summary`,
  `fromStart`. `next` includes every scanned line; `firstSequence` and `dropped` expose overwritten
  history. `collapse` returns count/lastSequence per identical consecutive window/message run.
  `summary` returns per-window/level counts and first/last lines per run for the returned cursor range.
  `fields` returns strings and omits collapse metadata. Limit (1–128) counts returned entries/runs.
- `console_search`: required `query`, optional `kind` (command/cvar), `limit` (20 by default, max 100),
  `after` (result index), `fields: name`. Words match names/help case-insensitively, including name
  subsequences. Exact names rank before prefixes, substrings, help matches and subsequences.
- `logs_sources`: trace windows since listener startup, sorted by current ring count, with lifetime
  `total`, current `ring`, `lastTimeMs` (Unix milliseconds), and lifetime error/warning/info counts.
- `logs_wait`: required `pattern`, optional `after`, `windows`, `timeoutMs`. The HTTP request stays
  pending between main-thread polls; `OnOutput` retains the first match even if the ring overwrites it.
  Returns `timedOut`, `next`, `dropped`, and `line` on a hit. Timeout must be at least 100 ms below
  `IdleTimeoutMs`; omitting it uses that maximum. No streaming/subscriptions are implemented.

Severity follows trace warning/error/assert callbacks; other output is info. `log_tags` uses ILogger's
VisitTags to list registered names and enabled states. Ring entries record GetCurrentTag while OnOutput
runs (`tag` is empty for untagged output); `logs_tail.tags` filters the field. The existing `log_tag`
query/set tool is unchanged. Shutdown prints the ring retention audit before discarding it.

## Deliberate limits

- Windows only in this minimal implementation; other platforms are not built.
- Stateless JSON responses; send `Connection: keep-alive` to reuse a connection. GET
  and DELETE return 405; no SSE, sessions, OAuth, legacy SSE transport or batching.
- Nonblocking sockets serviced on `SystemTick`; engine console work stays on the
  main thread. A slow console command can still stall that thread.
- Eight connections, configurable idle deadline (ten seconds by default), 8 KiB headers, 64 KiB JSON body.
  No chunked request bodies or `Expect: 100-continue`. Console input is at most
  4096 bytes, 64 whitespace-separated tokens, 256 bytes per token, matching the
  AZ console's token semantics.
- Logs are a thread-safe configurable in-memory tail (512 lines by default), truncated at 2048 bytes per
  message; incomplete/invalid UTF-8 tails are omitted. Captures `TraceMessageBus::OnOutput`, including AZLOG output routed
  through Trace; respects existing trace/logger filtering and compile settings.
  No disk history; tag discovery covers names registered with ILogger.
- AZ console functors only; legacy Cry console entries that have no AZ functor
  are outside this gem.
- As requested, the AzNetworking listener retains its existing wildcard bind.
  Non-loopback IPv4 peers are closed immediately after accept. HTTP Host and
  Origin are restricted to localhost/127.0.0.1 on the configured port. This does
  not prevent remote peers from reaching the TCP accept backlog. No CORS.

## Engine support and source installation

This source gem lives in **o3de-extras/Gems/LocalMcp**. It requires the companion
O3DE engine changes on `codex/local-mcp-script-defined-multiplayer`: console
autocomplete/logger metadata and the native Python/editor/Script Canvas automation APIs.
See [EDITOR.md](EDITOR.md) for the editor tools and supporting owner changes.

Enable `LocalMcp` in your project's `gem_names` and add its absolute gem directory
to the project's `external_subdirectories`, for example:

```json
"external_subdirectories": ["D:/o3de-extras/Gems/LocalMcp"]
```

The gem is Windows-only and disabled by default. Runtime console/log access does
not enable the editor endpoint. It uses the engine's existing AzNetworking,
OpenSSL, Qt and EditorPythonBindings targets; no additional libraries are fetched.

Tests cover JSON-RPC, cvar read/write/autocomplete, log cursors, fragmented HTTP,
Host/Origin checks, basic authentication, mTLS certificates and cfg/ini settings.
Editor and Script Canvas tools also have native owner tests and a headless workflow.
