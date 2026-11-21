/*
 * Copyright (c) Contributors to the Open 3D Engine Project.
 * For complete copyright and license terms please see the LICENSE at the root of this distribution.
 *
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 *
 */

#include "LocalMcpSearch.h"
#include "LocalMcpJson.h"
#include "LocalMcpServer.h"

#include <AzCore/Console/IConsole.h>
#include <AzCore/StringFunc/StringFunc.h>
#include <AzCore/std/sort.h>
#include <AzCore/std/string/conversions.h>

namespace LocalMcp
{
    namespace
    {
        // Rank() score contributions.
        constexpr int ExactNameScore = 1000;
        constexpr int NamePrefixScore = 100;
        constexpr int NameContainsScore = 75;
        constexpr int HelpContainsScore = 50;
        constexpr int NameSubsequenceScore = 10;

        //! console_search page size when the request gives no limit.
        constexpr size_t DefaultSearchLimit = 20;

        AZStd::string ToLower(AZStd::string_view text)
        {
            AZStd::string lower(text);
            AZStd::to_lower(lower.begin(), lower.end());
            return lower;
        }

        //! True when every character of word appears in name in order.
        bool IsSubsequence(AZStd::string_view word, AZStd::string_view name)
        {
            size_t matched = 0;
            for (const char character : name)
            {
                if (matched < word.size() && character == word[matched])
                {
                    ++matched;
                }
            }
            return matched == word.size();
        }

        bool IsInvisible(const AZ::ConsoleFunctorBase& functor)
        {
            return (functor.GetFlags() & AZ::ConsoleFunctorFlags::IsInvisible) != AZ::ConsoleFunctorFlags::Null;
        }

        //! First sentence of a help string, including its terminator.
        AZStd::string_view FirstSentence(AZStd::string_view help)
        {
            const size_t end = help.find_first_of(".!?\n");
            return end == AZStd::string_view::npos ? help : help.substr(0, end + 1);
        }
    } // namespace

    SearchTerms::SearchTerms(AZStd::string_view query)
        : m_query(ToLower(query))
    {
        AZ::StringFunc::Tokenize(m_query.c_str(), m_words, " \t\r\n");
    }

    int SearchTerms::Rank(AZStd::string_view nameText, AZStd::string_view helpText) const
    {
        const AZStd::string name = ToLower(nameText);
        const AZStd::string help = ToLower(helpText);

        int score = name == m_query ? ExactNameScore : 0;
        for (const AZStd::string& word : m_words)
        {
            const size_t position = name.find(word);
            if (position != AZStd::string::npos)
            {
                score += position == 0 ? NamePrefixScore : NameContainsScore;
            }
            else if (help.find(word) != AZStd::string::npos)
            {
                score += HelpContainsScore;
            }
            else if (IsSubsequence(word, name))
            {
                score += NameSubsequenceScore;
            }
            else
            {
                return NoMatch;
            }
        }
        return score;
    }

    const char* GetFunctorKind(const AZ::ConsoleFunctorBase& functor)
    {
        // Commands have no value type; cvars do.
        return functor.GetTypeId().IsNull() ? "command" : "cvar";
    }

    bool Server::HandleConsoleSearch(const ConsoleSearchArgs& args, ToolCall& call)
    {
        AZ::IConsole* console = AZ::Interface<AZ::IConsole>::Get();
        if (!console)
        {
            call.m_data.AddMember("error", "AZ console unavailable", call.m_allocator);
            return true;
        }

        // Collect the visible functors that match, filtered by kind.
        struct Match
        {
            AZ::ConsoleFunctorBase* m_functor;
            int m_score;
        };
        const SearchTerms terms(args.m_query);
        AZStd::vector<Match> matches;
        console->VisitRegisteredFunctors(
            [&](AZ::ConsoleFunctorBase* functor)
            {
                if (IsInvisible(*functor) || (args.m_kind && *args.m_kind != GetFunctorKind(*functor)))
                {
                    return;
                }

                const int score = terms.Rank(functor->GetName(), functor->GetDesc());
                if (score != SearchTerms::NoMatch)
                {
                    matches.push_back({ functor, score });
                }
            });

        // Best score first, then by name.
        AZStd::sort(
            matches.begin(),
            matches.end(),
            [](const Match& left, const Match& right)
            {
                if (left.m_score != right.m_score)
                {
                    return left.m_score > right.m_score;
                }
                return AZStd::string_view(left.m_functor->GetName()) < right.m_functor->GetName();
            });

        // Page through the sorted matches.
        const bool namesOnly = args.m_fields.has_value();
        const size_t after = aznumeric_cast<size_t>(args.m_after.value_or(0));
        const size_t limit = args.m_limit ? aznumeric_cast<size_t>(*args.m_limit) : DefaultSearchLimit;
        Json list(rapidjson::kArrayType);
        size_t next = AZStd::min(after, matches.size());
        for (; next < matches.size() && list.Size() < limit; ++next)
        {
            const AZ::ConsoleFunctorBase* functor = matches[next].m_functor;
            if (namesOnly)
            {
                list.PushBack(String(functor->GetName(), call.m_allocator), call.m_allocator);
                continue;
            }

            Json entry(rapidjson::kObjectType);
            entry.AddMember("name", String(functor->GetName(), call.m_allocator), call.m_allocator);
            entry.AddMember("kind", String(GetFunctorKind(*functor), call.m_allocator), call.m_allocator);
            entry.AddMember("description", String(FirstSentence(functor->GetDesc()), call.m_allocator), call.m_allocator);

            AZ::CVarFixedString value;
            if (functor->GetValue(value) == AZ::GetValueResult::Success)
            {
                entry.AddMember("value", String(value, call.m_allocator), call.m_allocator);
            }
            list.PushBack(entry, call.m_allocator);
        }

        // fields=name replies with the bare array of names.
        if (namesOnly)
        {
            call.m_data.Swap(list);
            return true;
        }

        call.m_data.AddMember("matches", list, call.m_allocator);
        call.m_data.AddMember("next", aznumeric_cast<uint64_t>(next), call.m_allocator);
        call.m_data.AddMember("total", aznumeric_cast<uint64_t>(matches.size()), call.m_allocator);
        return true;
    }
} // namespace LocalMcp
