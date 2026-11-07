/* Copyright (c) Contributors to the Open 3D Engine Project.
 * SPDX-License-Identifier: Apache-2.0 OR MIT */
#pragma once
#include <AzCore/std/string/string.h>
#include <AzCore/std/string/string_view.h>
#include <AzCore/std/containers/vector.h>
namespace LocalMcp
{
    //! Shared name/help ranking for console and reflected-binding discovery.
    class SearchTerms
    {
    public:
        explicit SearchTerms(AZStd::string_view query);
        int Rank(AZStd::string_view name, AZStd::string_view help) const;
    private:
        static AZStd::string Lower(AZStd::string text);
        AZStd::string m_query;
        AZStd::vector<AZStd::string> m_words;
    };
}
