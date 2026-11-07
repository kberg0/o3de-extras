/* Copyright (c) Contributors to the Open 3D Engine Project.
 * SPDX-License-Identifier: Apache-2.0 OR MIT */
#include "LocalMcpServer.h"
#include "LocalMcpJson.h"
#include "LocalMcpSearch.h"
#include <AzCore/Console/IConsole.h>
#include <AzCore/std/sort.h>
#include <AzCore/StringFunc/StringFunc.h>

namespace LocalMcp
{
    AZStd::string SearchTerms::Lower(AZStd::string text)
    {
        for (auto& ch : text)
        {
            if (ch >= 'A' && ch <= 'Z')
            {
                ch += 'a' - 'A';
            }
        }
        return text;
    }

    SearchTerms::SearchTerms(AZStd::string_view query) : m_query(Lower(AZStd::string(query)))
    {
        AZ::StringFunc::Tokenize(m_query.c_str(), m_words, " \t\r\n");
    }

    int SearchTerms::Rank(AZStd::string_view nameText, AZStd::string_view helpText) const
    {
        const auto name = Lower(AZStd::string(nameText));
        const auto help = Lower(AZStd::string(helpText));
        int score = name == m_query ? 1000 : 0;
        for (const auto& word : m_words)
        {
            const auto pos = name.find(word);
            if (pos != AZStd::string::npos)
            {
                score += pos == 0 ? 100 : 75;
            }
            else if (help.find(word) != AZStd::string::npos)
            {
                score += 50;
            }
            else
            {
                size_t cursor = 0;
                for (const char ch : name)
                {
                    if (cursor < word.size() && ch == word[cursor])
                    {
                        ++cursor;
                    }
                }
                if (cursor != word.size()) { return -1; }
                score += 10;
            }
        }
        return score;
    }


    void Server::SearchConsole(const Json& args, Json& data, Allocator& allocator)
    {
        struct Match
        {
            AZ::ConsoleFunctorBase* m_functor;
            int m_score;
        };
        const SearchTerms terms(args["query"].GetString());
        AZStd::vector<Match> matches;
        AZ::Interface<AZ::IConsole>::Get()->VisitRegisteredFunctors([&](AZ::ConsoleFunctorBase* functor)
        {
            if ((functor->GetFlags() & AZ::ConsoleFunctorFlags::IsInvisible) != AZ::ConsoleFunctorFlags::Null ||
                (args.HasMember("kind") && AZStd::string_view(args["kind"].GetString()) !=
                    (functor->GetTypeId().IsNull() ? "command" : "cvar")))
            {
                return;
            }
            const int score = terms.Rank(functor->GetName(), functor->GetDesc());
            if (score < 0)
            {
                return;
            }
            matches.push_back({ functor, score });
        });
        AZStd::sort(matches.begin(), matches.end(), [](const auto& a, const auto& b)
        {
            return a.m_score != b.m_score ? a.m_score > b.m_score :
                AZStd::string_view(a.m_functor->GetName()) < b.m_functor->GetName();
        });
        const size_t after = args.HasMember("after") ? args["after"].GetUint() : 0;
        const size_t limit = args.HasMember("limit") ? args["limit"].GetUint() : 20;
        Json list(rapidjson::kArrayType);
        size_t next = AZStd::min(after, matches.size());
        for (; next < matches.size() && list.Size() < limit; ++next)
        {
            auto* functor = matches[next].m_functor;
            if (args.HasMember("fields"))
            {
                list.PushBack(String(functor->GetName(), allocator), allocator);
                continue;
            }
            Json entry(rapidjson::kObjectType);
            entry.AddMember("name", String(functor->GetName(), allocator), allocator);
            entry.AddMember("kind", String(functor->GetTypeId().IsNull() ? "command" : "cvar", allocator), allocator);
            const AZStd::string_view description(functor->GetDesc());
            const auto end = description.find_first_of(".!?\n");
            entry.AddMember("description", String(description.substr(0, end == AZStd::string_view::npos ? end : end + 1), allocator), allocator);
            AZ::CVarFixedString value;
            if (functor->GetValue(value) == AZ::GetValueResult::Success)
            {
                entry.AddMember("value", String(value, allocator), allocator);
            }
            list.PushBack(entry, allocator);
        }
        if (args.HasMember("fields"))
        {
            data.Swap(list);
            return;
        }
        data.AddMember("matches", list, allocator);
        data.AddMember("next", aznumeric_cast<uint64_t>(next), allocator);
        data.AddMember("total", aznumeric_cast<uint64_t>(matches.size()), allocator);
    }
}
