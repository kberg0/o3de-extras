/* Copyright (c) Contributors to the Open 3D Engine Project.
 * SPDX-License-Identifier: Apache-2.0 OR MIT */
#include "LocalMcpServer.h"
#include "LocalMcpJson.h"
#include <AzCore/Console/ILogger.h>
#include <AzCore/std/sort.h>
#include <AzCore/std/algorithm.h>

namespace LocalMcp
{
    thread_local Server::Capture* Server::s_capture = nullptr;
    thread_local const char* Server::s_level = "info";

    namespace
    {
        Json LineJson(uint64_t sequence, const AZStd::string& window, const AZStd::string& message,
            const AZStd::string& level, const AZStd::string& tag, Allocator& allocator)
        {
            Json entry(rapidjson::kObjectType);
            entry.AddMember("sequence", sequence, allocator);
            entry.AddMember("window", String(window, allocator), allocator);
            entry.AddMember("message", String(message, allocator), allocator);
            entry.AddMember("level", String(level, allocator), allocator);
            entry.AddMember("tag", String(tag, allocator), allocator);
            return entry;
        }

        bool ListContains(const Json& args, const char* field, const AZStd::string& text, bool absent)
        {
            if (!args.HasMember(field))
            {
                return absent;
            }
            for (const auto& value : args[field].GetArray())
            {
                if (text == value.GetString())
                {
                    return true;
                }
            }
            return false;
        }
    }

    bool Server::OnOutput(const char* window, const char* message)
    {
        auto* logger = AZ::Interface<AZ::ILogger>::Get();
        const char* currentTag = logger ? logger->GetCurrentTag() : nullptr;
        AZStd::scoped_lock lock(m_logMutex);
        if (m_logs.size() == m_logLines)
        {
            --m_sources[m_logs.front().m_window].m_ring;
            m_logs.pop_front();
        }
        m_logs.push_back({ ++m_sequence, AZStd::string(AZStd::string_view(window ? window : "").substr(0, 128)),
            AZStd::string(AZStd::string_view(message ? message : "").substr(0, 2048)), s_level, currentTag ? currentTag : "" });
        const auto& line = m_logs.back();
        auto& source = m_sources[line.m_window];
        ++source.m_total;
        ++source.m_ring;
        ++source.m_levels[line.m_level == "error" ? 0 : line.m_level == "warning" ? 1 : 2];
        source.m_lastTime = AZStd::chrono::duration_cast<AZStd::chrono::milliseconds>(
            AZStd::chrono::system_clock::now().time_since_epoch()).count();
        if (s_capture && s_capture->m_server == this)
        {
            if (s_capture->m_lines.size() < 128)
            {
                s_capture->m_lines.push_back(line);
            }
            else
            {
                ++s_capture->m_truncated;
            }
        }
        for (auto& [id, wait] : m_waits)
        {
            AZ_UNUSED(id);
            if (wait.m_match.m_sequence || AZStd::chrono::steady_clock::now() >= wait.m_until)
            {
                continue;
            }
            wait.m_next = line.m_sequence;
            if ((!wait.m_filterWindows || AZStd::find(wait.m_windows.begin(), wait.m_windows.end(), line.m_window) != wait.m_windows.end())
                && AZStd::regex_search(line.m_message.c_str(), wait.m_pattern))
            {
                wait.m_match = line;
            }
        }
        return false;
    }

    bool Server::HandleLogTool(AZStd::string_view name, const Json& args, Json& data, Allocator& allocator,
        AZStd::string& error, uint64_t* waitId)
    {
        if (name == "log_tags")
        {
            auto* logger = AZ::Interface<AZ::ILogger>::Get();
            if (!logger)
            {
                data.AddMember("error", "AZ logger unavailable", allocator);
                return true;
            }
            AZStd::vector<AZStd::pair<AZStd::string, bool>> tags;
            logger->VisitTags([&](const char* tag, bool enabled) { tags.emplace_back(tag, enabled); });
            AZStd::sort(tags.begin(), tags.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
            Json entries(rapidjson::kArrayType);
            for (const auto& [tag, enabled] : tags)
            {
                Json entry(rapidjson::kObjectType);
                entry.AddMember("tag", String(tag, allocator), allocator);
                entry.AddMember("enabled", enabled, allocator);
                entries.PushBack(entry, allocator);
            }
            data.AddMember("tags", entries, allocator);
            return true;
        }
        for (const char* field : { "windows", "excludeWindows", "tags" })
        {
            if (!args.HasMember(field))
            {
                continue;
            }
            if (!args[field].IsArray() || args[field].Size() > 128)
            {
                error = "Expected an array of at most 128 windows/tags";
                return false;
            }
            for (const auto& value : args[field].GetArray())
            {
                if (!value.IsString() || value.GetStringLength() > (AZStd::string_view(field) == "tags" ? 256u : 128u) || strlen(value.GetString()) != value.GetStringLength())
                {
                    error = "Invalid window/tag";
                    return false;
                }
            }
        }
        for (const char* field : { "regex", "collapse", "summary", "fromStart" })
        {
            if (args.HasMember(field) && !args[field].IsBool())
            {
                error = "Expected boolean regex/collapse/summary/fromStart";
                return false;
            }
        }
        if ((args.HasMember("after") && !args["after"].IsUint64()) ||
            (args.HasMember("limit") && (!args["limit"].IsUint() || !args["limit"].GetUint() || args["limit"].GetUint() > 128)) ||
            (args.HasMember("pattern") && !IsString(args, "pattern", 2048)) ||
            (args.HasMember("level") && (!IsString(args, "level") ||
                (AZStd::string_view(args["level"].GetString()) != "error" && AZStd::string_view(args["level"].GetString()) != "warning" &&
                 AZStd::string_view(args["level"].GetString()) != "info"))) ||
            (args.HasMember("fields") && (!IsString(args, "fields") ||
                (AZStd::string_view(args["fields"].GetString()) != "message" && AZStd::string_view(args["fields"].GetString()) != "compact"))))
        {
            error = "Invalid log cursor, limit [1,128], pattern, level or fields";
            return false;
        }
        const bool regex = !args.HasMember("regex") || args["regex"].GetBool();
        const char* pattern = args.HasMember("pattern") ? args["pattern"].GetString() : "";
        AZStd::regex expression(regex ? pattern : "");
        if (!expression.Valid())
        {
            error = "Invalid pattern regex";
            return false;
        }
        AZStd::scoped_lock lock(m_logMutex);
        if (name == "logs_sources")
        {
            AZStd::vector<AZStd::string> windows;
            for (const auto& [window, source] : m_sources)
            {
                AZ_UNUSED(source);
                windows.push_back(window);
            }
            AZStd::sort(windows.begin(), windows.end(), [&](const auto& a, const auto& b)
            {
                const auto ca = m_sources.at(a).m_ring;
                const auto cb = m_sources.at(b).m_ring;
                return ca != cb ? ca > cb : a < b;
            });
            Json sources(rapidjson::kArrayType);
            for (const auto& window : windows)
            {
                const auto& source = m_sources.at(window);
                Json entry(rapidjson::kObjectType);
                entry.AddMember("window", String(window, allocator), allocator);
                entry.AddMember("total", source.m_total, allocator);
                entry.AddMember("ring", source.m_ring, allocator);
                entry.AddMember("lastTimeMs", source.m_lastTime, allocator);
                Json levels(rapidjson::kObjectType);
                levels.AddMember("error", source.m_levels[0], allocator);
                levels.AddMember("warning", source.m_levels[1], allocator);
                levels.AddMember("info", source.m_levels[2], allocator);
                entry.AddMember("levels", levels, allocator);
                sources.PushBack(entry, allocator);
            }
            data.AddMember("sources", sources, allocator);
            return true;
        }
        const uint64_t first = m_logs.empty() ? m_sequence + 1 : m_logs.front().m_sequence;
        const size_t limit = args.HasMember("limit") ? args["limit"].GetUint() : 100;
        const uint64_t after = args.HasMember("fromStart") && args["fromStart"].GetBool() ? 0 :
            args.HasMember("after") ? args["after"].GetUint64() :
            name == "logs_wait" ? m_sequence : (m_sequence > limit ? m_sequence - limit : 0);
        if (after > m_sequence)
        {
            data.AddMember("error", "Cursor is ahead of this server; omit after to restart tailing", allocator);
            return true;
        }
        const uint64_t dropped = after < first - 1 ? first - 1 - after : 0;
        if (name == "logs_wait")
        {
            if (!args.HasMember("pattern") || !waitId ||
                (args.HasMember("timeoutMs") && (!args["timeoutMs"].IsUint64() ||
                    args["timeoutMs"].GetUint64() > aznumeric_cast<uint64_t>(m_idleTimeoutMs - 100))))
            {
                error = "logs_wait requires pattern, HTTP request, and timeoutMs below IdleTimeoutMs minus 100";
                return false;
            }
            if (!*waitId)
            {
                Wait wait;
                wait.m_pattern = AZStd::move(expression);
                wait.m_next = after;
                wait.m_dropped = dropped;
                wait.m_until = AZStd::chrono::steady_clock::now() + AZStd::chrono::milliseconds(
                    args.HasMember("timeoutMs") ? args["timeoutMs"].GetUint64() : aznumeric_cast<uint64_t>(m_idleTimeoutMs - 100));
                wait.m_filterWindows = args.HasMember("windows");
                if (wait.m_filterWindows)
                {
                    for (const auto& window : args["windows"].GetArray())
                    {
                        wait.m_windows.emplace_back(window.GetString());
                    }
                }
                for (const auto& line : m_logs)
                {
                    if (line.m_sequence <= after)
                    {
                        continue;
                    }
                    wait.m_next = line.m_sequence;
                    if (ListContains(args, "windows", line.m_window, true) && AZStd::regex_search(line.m_message.c_str(), wait.m_pattern))
                    {
                        wait.m_match = line;
                        break;
                    }
                }
                *waitId = ++m_nextWait;
                m_waits.emplace(*waitId, AZStd::move(wait));
            }
            const auto& wait = m_waits.at(*waitId);
            if (!wait.m_match.m_sequence && AZStd::chrono::steady_clock::now() < wait.m_until)
            {
                return true;
            }
            data.AddMember("timedOut", !wait.m_match.m_sequence, allocator);
            data.AddMember("next", wait.m_next, allocator);
            data.AddMember("dropped", wait.m_dropped, allocator);
            if (wait.m_match.m_sequence)
            {
                const auto& line = wait.m_match;
                data.AddMember("line", LineJson(line.m_sequence, line.m_window, line.m_message, line.m_level, line.m_tag, allocator), allocator);
            }
            m_waits.erase(*waitId);
            *waitId = 0;
            return true;
        }
        const bool collapse = args.HasMember("collapse") && args["collapse"].GetBool();
        const bool summary = args.HasMember("summary") && args["summary"].GetBool();
        Json lines(rapidjson::kArrayType);
        Json counts(rapidjson::kObjectType);
        uint64_t next = after;
        const LogLine* previous = nullptr;
        for (const auto& line : m_logs)
        {
            if (line.m_sequence <= after)
            {
                continue;
            }
            const bool matches = ListContains(args, "windows", line.m_window, true) &&
                !ListContains(args, "excludeWindows", line.m_window, false) &&
                ListContains(args, "tags", line.m_tag, true) &&
                (!args.HasMember("level") || line.m_level == args["level"].GetString()) &&
                (!args.HasMember("pattern") || (regex ? AZStd::regex_search(line.m_message.c_str(), expression) :
                    line.m_message.find(pattern) != AZStd::string::npos));
            if (!matches)
            {
                next = line.m_sequence;
                previous = nullptr;
                continue;
            }
            const bool repeat = (collapse || summary) && previous && previous->m_window == line.m_window &&
                previous->m_message == line.m_message && previous->m_tag == line.m_tag && (!summary || previous->m_level == line.m_level);
            if (!repeat && lines.Size() == limit)
            {
                break;
            }
            next = line.m_sequence;
            if (summary)
            {
                if (!counts.HasMember(line.m_window.c_str()))
                {
                    counts.AddMember(String(line.m_window, allocator), Json(rapidjson::kObjectType), allocator);
                }
                auto& levels = counts[line.m_window.c_str()];
                if (!levels.HasMember(line.m_level.c_str()))
                {
                    auto levelName = String(line.m_level, allocator);
                    levels.AddMember(levelName, uint64_t(0), allocator);
                }
                levels[line.m_level.c_str()].SetUint64(levels[line.m_level.c_str()].GetUint64() + 1);
            }
            if (repeat)
            {
                auto& entry = lines[lines.Size() - 1];
                entry["count"].SetUint64(entry["count"].GetUint64() + 1);
                entry["lastSequence"].SetUint64(line.m_sequence);
                if (summary)
                {
                    entry["last"].CopyFrom(LineJson(line.m_sequence, line.m_window, line.m_message, line.m_level, line.m_tag, allocator), allocator);
                }
            }
            else if (summary)
            {
                Json entry(rapidjson::kObjectType);
                entry.AddMember("first", LineJson(line.m_sequence, line.m_window, line.m_message, line.m_level, line.m_tag, allocator), allocator);
                entry.AddMember("last", LineJson(line.m_sequence, line.m_window, line.m_message, line.m_level, line.m_tag, allocator), allocator);
                entry.AddMember("count", uint64_t(1), allocator);
                entry.AddMember("lastSequence", line.m_sequence, allocator);
                lines.PushBack(entry, allocator);
            }
            else
            {
                auto entry = LineJson(line.m_sequence, line.m_window, line.m_message, line.m_level, line.m_tag, allocator);
                if (collapse)
                {
                    entry.AddMember("count", uint64_t(1), allocator);
                    entry.AddMember("lastSequence", line.m_sequence, allocator);
                }
                lines.PushBack(entry, allocator);
            }
            previous = &line;
        }
        if (!summary && args.HasMember("fields"))
        {
            for (auto& line : lines.GetArray())
            {
                const auto text = AZStd::string_view(args["fields"].GetString()) == "message" ? AZStd::string(line["message"].GetString()) :
                    AZStd::string::format("%llu %s: %s", line["sequence"].GetUint64(), line["window"].GetString(), line["message"].GetString());
                line = String(text, allocator);
            }
        }
        data.AddMember("lines", lines, allocator);
        data.AddMember("next", next, allocator);
        data.AddMember("dropped", dropped, allocator);
        data.AddMember("firstSequence", first, allocator);
        if (summary)
        {
            data.AddMember("counts", counts, allocator);
        }
        return true;
    }
}
