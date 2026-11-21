/*
 * Copyright (c) Contributors to the Open 3D Engine Project.
 * For complete copyright and license terms please see the LICENSE at the root of this distribution.
 *
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 *
 */

#pragma once

#include <AzCore/std/containers/vector.h>
#include <AzCore/std/string/string.h>
#include <AzCore/std/string/string_view.h>

namespace AZ
{
    class ConsoleFunctorBase;
}

namespace LocalMcp
{
    //! Case-insensitive ranking of names and help text against a search query.
    //! Shared by console search and reflected-binding discovery so both order results the same way.
    class SearchTerms
    {
    public:
        //! Splits query into whitespace-separated words.
        explicit SearchTerms(AZStd::string_view query);

        //! Scores name and help against the query; higher is better.
        //! Returns NoMatch when some word is found in neither the name, the help, nor as a subsequence of the name.
        int Rank(AZStd::string_view name, AZStd::string_view help) const;

        //! Rank() result for a candidate that does not match.
        static constexpr int NoMatch = -1;

    private:
        AZStd::string m_query;
        AZStd::vector<AZStd::string> m_words;
    };

    //! "command" for a console command, "cvar" for a console variable.
    const char* GetFunctorKind(const AZ::ConsoleFunctorBase& functor);
} // namespace LocalMcp
