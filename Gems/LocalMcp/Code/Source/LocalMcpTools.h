/* Copyright (c) Contributors to the Open 3D Engine Project.
 * SPDX-License-Identifier: Apache-2.0 OR MIT */
#pragma once
namespace LocalMcp
{
    inline constexpr const char* ToolSchemas = R"({
  "tools": [
    {
      "name": "console_complete",
      "description": "List AZ cvars/commands, or autocomplete a command including arguments.",
      "inputSchema": {
        "type": "object",
        "properties": {
          "prefix": {
            "type": "string",
            "maxLength": 256
          }
        }
      }
    },
    {
      "name": "console_execute",
      "description": "Execute an AZ console command or read/set a cvar. Runs on the main thread.",
      "inputSchema": {
        "type": "object",
        "properties": {
          "command": {
            "type": "string",
            "maxLength": 4096
          },
          "output": {
            "type": "string",
            "enum": [
              "none",
              "capture"
            ]
          },
          "grep": {
            "type": "string",
            "maxLength": 2048
          }
        },
        "required": [
          "command"
        ]
      }
    },
    {
      "name": "logs_tail",
      "description": "Read buffered engine logs. Poll with the returned next cursor as after; dropped reports overwritten lines.",
      "inputSchema": {
        "type": "object",
        "properties": {
          "after": {
            "type": "integer",
            "minimum": 0
          },
          "limit": {
            "type": "integer",
            "minimum": 1,
            "maximum": 128
          },
          "pattern": {
            "type": "string",
            "maxLength": 2048
          },
          "regex": {
            "type": "boolean"
          },
          "windows": {
            "type": "array",
            "items": {
              "type": "string"
            }
          },
          "excludeWindows": {
            "type": "array",
            "items": {
              "type": "string"
            }
          },
          "level": {
            "type": "string",
            "enum": [
              "error",
              "warning",
              "info"
            ]
          },
          "fields": {
            "type": "string",
            "enum": [
              "message",
              "compact"
            ]
          },
          "collapse": {
            "type": "boolean"
          },
          "summary": {
            "type": "boolean"
          },
          "fromStart": {
            "type": "boolean"
          },
          "tags": {
            "type": "array",
            "maxItems": 128,
            "items": {
              "type": "string",
              "maxLength": 256
            }
          }
        }
      }
    },
    {
      "name": "log_tag",
      "description": "Enable or disable an AZLOG tag, or query it by omitting enabled.",
      "inputSchema": {
        "type": "object",
        "properties": {
          "tag": {
            "type": "string",
            "minLength": 1,
            "maxLength": 256
          },
          "enabled": {
            "type": "boolean"
          }
        },
        "required": [
          "tag"
        ]
      }
    },
    {
      "name": "console_search",
      "description": "Rank visible commands/cvars by name and help words or name subsequence.",
      "inputSchema": {
        "type": "object",
        "required": [
          "query"
        ],
        "properties": {
          "query": {
            "type": "string",
            "maxLength": 256
          },
          "kind": {
            "type": "string",
            "enum": [
              "command",
              "cvar"
            ]
          },
          "limit": {
            "type": "integer",
            "minimum": 1,
            "maximum": 100
          },
          "after": {
            "type": "integer",
            "minimum": 0
          },
          "fields": {
            "type": "string",
            "enum": [
              "name"
            ]
          }
        }
      }
    },
    {
      "name": "logs_sources",
      "description": "Trace windows since listener startup, lifetime/ring counts, Unix lastTimeMs and lifetime level counts.",
      "inputSchema": {
        "type": "object",
        "properties": {}
      }
    },
    {
      "name": "logs_wait",
      "description": "Hold for first log pattern match without blocking the engine. timeoutMs must be at least 100 ms below IdleTimeoutMs.",
      "inputSchema": {
        "type": "object",
        "required": [
          "pattern"
        ],
        "properties": {
          "pattern": {
            "type": "string",
            "maxLength": 2048
          },
          "after": {
            "type": "integer",
            "minimum": 0
          },
          "timeoutMs": {
            "type": "integer",
            "minimum": 0
          },
          "windows": {
            "type": "array",
            "items": {
              "type": "string"
            }
          }
        }
      }
    },
    {
      "name": "log_tags",
      "description": "List registered AZLOG tag names and their enabled states.",
      "inputSchema": {
        "type": "object",
        "properties": {}
      }
    }
  ]
})";
}
