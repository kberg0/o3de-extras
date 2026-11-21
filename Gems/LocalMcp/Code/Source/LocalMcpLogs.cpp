/*
 * Copyright (c) Contributors to the Open 3D Engine Project.
 * For complete copyright and license terms please see the LICENSE at the root of this distribution.
 *
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 *
 */

#include "LocalMcpJson.h"
#include "LocalMcpServer.h"

#include <AzCore/Console/ILogger.h>
#include <AzCore/std/algorithm.h>
#include <AzCore/std/sort.h>

namespace LocalMcp
{
    thread_local Server::Capture* Server::s_capture = nullptr;
    thread_local const char* Server::s_level = Server::InfoLevel;

    namespace
    {
        // Stored lines are cut to these sizes so one noisy trace cannot exhaust the ring's memory.
        constexpr size_t MaxWindowLength = 128;
        constexpr size_t MaxMessageLength = 2048;

        //! Lines a console_execute reply carries; the rest are counted as truncated.
        constexpr size_t MaxCapturedLines = 128;

        //! logs_tail page size when the request gives no limit.
        constexpr size_t DefaultTailLimit = 100;

        //! logs_wait must answer this long before the connection's idle timeout closes it.
        constexpr AZ::s64 WaitReplyMarginMs = 100;

        // Values of the logs_tail fields argument.
        constexpr const char* FieldsMessage = "message";

        //! True when list is absent and whenAbsent is set, or when list holds text.
        bool MatchesList(const AZStd::optional<AZStd::vector<AZStd::string>>& list, const AZStd::string& text, bool whenAbsent)
        {
            if (!list)
            {
                return whenAbsent;
            }
            return AZStd::find(list->begin(), list->end(), text) != list->end();
        }

        int64_t GetUnixTimeMs()
        {
            const auto sinceEpoch = AZStd::chrono::system_clock::now().time_since_epoch();
            return AZStd::chrono::duration_cast<AZStd::chrono::milliseconds>(sinceEpoch).count();
        }
    } // namespace

    bool Server::OnPreWarning(
        [[maybe_unused]] const char* window,
        [[maybe_unused]] const char* fileName,
        [[maybe_unused]] int line,
        [[maybe_unused]] const char* func,
        [[maybe_unused]] const char* message)
    {
        s_level = WarningLevel;
        return false;
    }

    bool Server::OnWarning([[maybe_unused]] const char* window, [[maybe_unused]] const char* message)
    {
        s_level = InfoLevel;
        return false;
    }

    bool Server::OnPreError(
        [[maybe_unused]] const char* window,
        [[maybe_unused]] const char* fileName,
        [[maybe_unused]] int line,
        [[maybe_unused]] const char* func,
        [[maybe_unused]] const char* message)
    {
        s_level = ErrorLevel;
        return false;
    }

    bool Server::OnError([[maybe_unused]] const char* window, [[maybe_unused]] const char* message)
    {
        s_level = InfoLevel;
        return false;
    }

    bool Server::OnPreAssert(
        [[maybe_unused]] const char* fileName,
        [[maybe_unused]] int line,
        [[maybe_unused]] const char* func,
        [[maybe_unused]] const char* message)
    {
        s_level = ErrorLevel;
        return false;
    }

    bool Server::OnAssert([[maybe_unused]] const char* message)
    {
        s_level = InfoLevel;
        return false;
    }

    bool Server::OnPrintf([[maybe_unused]] const char* window, [[maybe_unused]] const char* message)
    {
        s_level = InfoLevel;
        return false;
    }

    bool Server::OnOutput(const char* window, const char* message)
    {
        AZ::ILogger* logger = AZ::Interface<AZ::ILogger>::Get();
        const char* currentTag = logger ? logger->GetCurrentTag() : nullptr;

        AZStd::scoped_lock lock(m_logMutex);

        // Append to the ring, dropping the oldest line when it is full.
        if (m_logs.size() == m_logLines)
        {
            --m_sources[m_logs.front().m_window].m_ring;
            m_logs.pop_front();
        }

        LogLine newLine;
        newLine.m_sequence = ++m_sequence;
        newLine.m_window = AZStd::string(AZStd::string_view(window ? window : "").substr(0, MaxWindowLength));
        newLine.m_message = AZStd::string(AZStd::string_view(message ? message : "").substr(0, MaxMessageLength));
        newLine.m_level = s_level;
        newLine.m_tag = currentTag ? currentTag : "";
        m_logs.push_back(AZStd::move(newLine));
        const LogLine& line = m_logs.back();

        // Count it against its window.
        Source& source = m_sources[line.m_window];
        ++source.m_total;
        ++source.m_ring;
        if (line.m_level == ErrorLevel)
        {
            ++source.m_errorCount;
        }
        else if (line.m_level == WarningLevel)
        {
            ++source.m_warningCount;
        }
        else
        {
            ++source.m_infoCount;
        }
        source.m_lastTimeMs = GetUnixTimeMs();

        // Capture it for the console command running on this thread, if any.
        if (s_capture && s_capture->m_server == this)
        {
            if (s_capture->m_lines.size() < MaxCapturedLines)
            {
                s_capture->m_lines.push_back(line);
            }
            else
            {
                ++s_capture->m_truncated;
            }
        }

        // Match it against the held waits, before a later line can overwrite it in the ring.
        const auto now = AZStd::chrono::steady_clock::now();
        for (auto& entry : m_waits)
        {
            Wait& wait = entry.second;
            if (wait.m_match.m_sequence || now >= wait.m_until)
            {
                continue;
            }

            wait.m_next = line.m_sequence;
            const bool windowMatches =
                !wait.m_filterWindows || AZStd::find(wait.m_windows.begin(), wait.m_windows.end(), line.m_window) != wait.m_windows.end();
            if (windowMatches && AZStd::regex_search(line.m_message.c_str(), wait.m_pattern))
            {
                wait.m_match = line;
            }
        }
        return false;
    }

    rapidjson::Value Server::LineToJson(const LogLine& line, Allocator& allocator)
    {
        Json entry(rapidjson::kObjectType);
        entry.AddMember("sequence", line.m_sequence, allocator);
        entry.AddMember("window", String(line.m_window, allocator), allocator);
        entry.AddMember("message", String(line.m_message, allocator), allocator);
        entry.AddMember("level", String(line.m_level, allocator), allocator);
        entry.AddMember("tag", String(line.m_tag, allocator), allocator);
        return entry;
    }

    uint64_t Server::GetFirstSequence() const
    {
        return m_logs.empty() ? m_sequence + 1 : m_logs.front().m_sequence;
    }

    bool Server::HandleLogTags([[maybe_unused]] const LogTagsArgs& args, ToolCall& call)
    {
        AZ::ILogger* logger = AZ::Interface<AZ::ILogger>::Get();
        if (!logger)
        {
            call.m_data.AddMember("error", "AZ logger unavailable", call.m_allocator);
            return true;
        }

        AZStd::vector<AZStd::pair<AZStd::string, bool>> tags;
        logger->VisitTags(
            [&tags](const char* tag, bool enabled)
            {
                tags.emplace_back(tag, enabled);
            });
        AZStd::sort(tags.begin(), tags.end());

        Json entries(rapidjson::kArrayType);
        for (const auto& [tag, enabled] : tags)
        {
            Json entry(rapidjson::kObjectType);
            entry.AddMember("tag", String(tag, call.m_allocator), call.m_allocator);
            entry.AddMember("enabled", enabled, call.m_allocator);
            entries.PushBack(entry, call.m_allocator);
        }
        call.m_data.AddMember("tags", entries, call.m_allocator);
        return true;
    }

    bool Server::HandleLogsSources([[maybe_unused]] const LogsSourcesArgs& args, ToolCall& call)
    {
        AZStd::scoped_lock lock(m_logMutex);

        // Busiest window in the ring first, then by name.
        AZStd::vector<AZStd::string> windows;
        for (const auto& entry : m_sources)
        {
            windows.push_back(entry.first);
        }
        AZStd::sort(
            windows.begin(),
            windows.end(),
            [this](const AZStd::string& left, const AZStd::string& right)
            {
                const uint64_t leftRing = m_sources.at(left).m_ring;
                const uint64_t rightRing = m_sources.at(right).m_ring;
                if (leftRing != rightRing)
                {
                    return leftRing > rightRing;
                }
                return left < right;
            });

        Json sources(rapidjson::kArrayType);
        for (const AZStd::string& window : windows)
        {
            const Source& source = m_sources.at(window);
            Json levels(rapidjson::kObjectType);
            levels.AddMember(rapidjson::StringRef(ErrorLevel), source.m_errorCount, call.m_allocator);
            levels.AddMember(rapidjson::StringRef(WarningLevel), source.m_warningCount, call.m_allocator);
            levels.AddMember(rapidjson::StringRef(InfoLevel), source.m_infoCount, call.m_allocator);

            Json entry(rapidjson::kObjectType);
            entry.AddMember("window", String(window, call.m_allocator), call.m_allocator);
            entry.AddMember("total", source.m_total, call.m_allocator);
            entry.AddMember("ring", source.m_ring, call.m_allocator);
            entry.AddMember("lastTimeMs", source.m_lastTimeMs, call.m_allocator);
            entry.AddMember("levels", levels, call.m_allocator);
            sources.PushBack(entry, call.m_allocator);
        }
        call.m_data.AddMember("sources", sources, call.m_allocator);
        return true;
    }

    bool Server::HandleLogsTail(const LogsTailArgs& args, ToolCall& call)
    {
        const bool useRegex = args.m_regex.value_or(true);
        const AZStd::string pattern = args.m_pattern.value_or("");
        const AZStd::regex expression(useRegex ? pattern.c_str() : "");
        if (!expression.Valid())
        {
            call.m_error = "Invalid pattern regex";
            return false;
        }

        AZStd::scoped_lock lock(m_logMutex);

        // Start after the cursor: from the beginning, from the given cursor, or one page before the newest line.
        const uint64_t first = GetFirstSequence();
        const size_t limit = args.m_limit ? aznumeric_cast<size_t>(*args.m_limit) : DefaultTailLimit;
        uint64_t after = 0;
        if (args.m_fromStart.value_or(false))
        {
            after = 0;
        }
        else if (args.m_after)
        {
            after = *args.m_after;
        }
        else
        {
            after = m_sequence > limit ? m_sequence - limit : 0;
        }

        if (after > m_sequence)
        {
            call.m_data.AddMember("error", "Cursor is ahead of this server; omit after to restart tailing", call.m_allocator);
            return true;
        }
        const uint64_t dropped = after < first - 1 ? first - 1 - after : 0;

        // collapse folds runs of identical lines into one entry with a count; summary also counts lines per window and level.
        const bool collapse = args.m_collapse.value_or(false);
        const bool summary = args.m_summary.value_or(false);
        Json lines(rapidjson::kArrayType);
        Json counts(rapidjson::kObjectType);
        uint64_t next = after;
        const LogLine* previous = nullptr;
        for (const LogLine& line : m_logs)
        {
            if (line.m_sequence <= after)
            {
                continue;
            }

            const bool patternMatches = !args.m_pattern ||
                (useRegex ? AZStd::regex_search(line.m_message.c_str(), expression) : line.m_message.find(pattern) != AZStd::string::npos);
            const bool matches = MatchesList(args.m_windows, line.m_window, true) &&
                !MatchesList(args.m_excludeWindows, line.m_window, false) && MatchesList(args.m_tags, line.m_tag, true) &&
                (!args.m_level || line.m_level == *args.m_level) && patternMatches;
            if (!matches)
            {
                // Unmatched lines still advance the cursor, so a filtered poll never rescans them.
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
                    counts.AddMember(String(line.m_window, call.m_allocator), Json(rapidjson::kObjectType), call.m_allocator);
                }
                Json& levels = counts[line.m_window.c_str()];
                if (!levels.HasMember(line.m_level.c_str()))
                {
                    Json levelName = String(line.m_level, call.m_allocator);
                    levels.AddMember(levelName, uint64_t{ 0 }, call.m_allocator);
                }
                Json& levelCount = levels[line.m_level.c_str()];
                levelCount.SetUint64(levelCount.GetUint64() + 1);
            }

            if (repeat)
            {
                Json& entry = lines[lines.Size() - 1];
                entry["count"].SetUint64(entry["count"].GetUint64() + 1);
                entry["lastSequence"].SetUint64(line.m_sequence);
                if (summary)
                {
                    entry["last"].CopyFrom(LineToJson(line, call.m_allocator), call.m_allocator);
                }
            }
            else if (summary)
            {
                Json entry(rapidjson::kObjectType);
                entry.AddMember("first", LineToJson(line, call.m_allocator), call.m_allocator);
                entry.AddMember("last", LineToJson(line, call.m_allocator), call.m_allocator);
                entry.AddMember("count", uint64_t{ 1 }, call.m_allocator);
                entry.AddMember("lastSequence", line.m_sequence, call.m_allocator);
                lines.PushBack(entry, call.m_allocator);
            }
            else
            {
                Json entry = LineToJson(line, call.m_allocator);
                if (collapse)
                {
                    entry.AddMember("count", uint64_t{ 1 }, call.m_allocator);
                    entry.AddMember("lastSequence", line.m_sequence, call.m_allocator);
                }
                lines.PushBack(entry, call.m_allocator);
            }
            previous = &line;
        }

        // fields replaces each line object with a plain string.
        if (!summary && args.m_fields)
        {
            const bool messageOnly = *args.m_fields == FieldsMessage;
            for (Json& line : lines.GetArray())
            {
                const AZStd::string text = messageOnly
                    ? AZStd::string(line["message"].GetString())
                    : AZStd::string::format(
                          "%llu %s: %s", line["sequence"].GetUint64(), line["window"].GetString(), line["message"].GetString());
                line = String(text, call.m_allocator);
            }
        }

        call.m_data.AddMember("lines", lines, call.m_allocator);
        call.m_data.AddMember("next", next, call.m_allocator);
        call.m_data.AddMember("dropped", dropped, call.m_allocator);
        call.m_data.AddMember("firstSequence", first, call.m_allocator);
        if (summary)
        {
            call.m_data.AddMember("counts", counts, call.m_allocator);
        }
        return true;
    }

    bool Server::HandleLogsWait(const LogsWaitArgs& args, ToolCall& call)
    {
        // The wait must reply before the idle timeout closes its connection.
        const uint64_t maxTimeoutMs = aznumeric_cast<uint64_t>(m_idleTimeoutMs - WaitReplyMarginMs);
        if (!call.m_waitId || (args.m_timeoutMs && *args.m_timeoutMs > maxTimeoutMs))
        {
            call.m_error = "logs_wait requires an HTTP connection, and timeoutMs below IdleTimeoutMs minus 100";
            return false;
        }

        AZStd::regex expression(args.m_pattern.c_str());
        if (!expression.Valid())
        {
            call.m_error = "Invalid pattern regex";
            return false;
        }

        AZStd::scoped_lock lock(m_logMutex);
        const uint64_t after = args.m_after.value_or(m_sequence);
        if (after > m_sequence)
        {
            call.m_data.AddMember("error", "Cursor is ahead of this server; omit after to restart tailing", call.m_allocator);
            return true;
        }

        // On the first call, scan the ring for an existing match and register the wait.
        // OnOutput() then matches new lines until the request is answered.
        uint64_t& waitId = *call.m_waitId;
        if (!waitId)
        {
            const uint64_t first = GetFirstSequence();
            Wait wait;
            wait.m_pattern = AZStd::move(expression);
            wait.m_next = after;
            wait.m_dropped = after < first - 1 ? first - 1 - after : 0;
            wait.m_until = AZStd::chrono::steady_clock::now() + AZStd::chrono::milliseconds(args.m_timeoutMs.value_or(maxTimeoutMs));
            wait.m_filterWindows = args.m_windows.has_value();
            if (args.m_windows)
            {
                wait.m_windows = *args.m_windows;
            }

            for (const LogLine& line : m_logs)
            {
                if (line.m_sequence <= after)
                {
                    continue;
                }

                wait.m_next = line.m_sequence;
                if (MatchesList(args.m_windows, line.m_window, true) && AZStd::regex_search(line.m_message.c_str(), wait.m_pattern))
                {
                    wait.m_match = line;
                    break;
                }
            }

            waitId = ++m_nextWait;
            m_waits.emplace(waitId, AZStd::move(wait));
        }

        // Keep holding until a match or the deadline.
        const Wait& wait = m_waits.at(waitId);
        if (!wait.m_match.m_sequence && AZStd::chrono::steady_clock::now() < wait.m_until)
        {
            return true;
        }

        call.m_data.AddMember("timedOut", !wait.m_match.m_sequence, call.m_allocator);
        call.m_data.AddMember("next", wait.m_next, call.m_allocator);
        call.m_data.AddMember("dropped", wait.m_dropped, call.m_allocator);
        if (wait.m_match.m_sequence)
        {
            call.m_data.AddMember("line", LineToJson(wait.m_match, call.m_allocator), call.m_allocator);
        }
        m_waits.erase(waitId);
        waitId = 0;
        return true;
    }
} // namespace LocalMcp
