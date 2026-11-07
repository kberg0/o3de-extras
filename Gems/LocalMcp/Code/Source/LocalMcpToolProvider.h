/* Copyright (c) Contributors to the Open 3D Engine Project.
 * SPDX-License-Identifier: Apache-2.0 OR MIT */
#pragma once
#include <AzCore/JSON/document.h>
#include <AzCore/std/string/string.h>

namespace LocalMcp
{
    //! Optional process-specific tools; owned by the component which owns Server.
    //! Called synchronously on the same main thread as Server::Poll().
    class ToolProvider
    {
    public:
        enum class Result { Unknown, Complete, InvalidArguments, Pending };
        virtual ~ToolProvider() = default;
        virtual const char* GetServerName() const = 0;
        virtual void AddTools(rapidjson::Value& tools, rapidjson::Document::AllocatorType& allocator) const = 0;
        //! A completed tool failure uses data.error; malformed arguments return InvalidArguments with detail.
        virtual Result Invoke(AZStd::string_view name, const rapidjson::Value& args, rapidjson::Value& data,
            rapidjson::Document::AllocatorType& allocator, AZStd::string& detail) = 0;
        //! At most one active provider operation. Invoke must refuse a second mutation while busy.
        //! Resume never invokes the mutation again. Server holds the original connection/id until completion.
        virtual Result PollPending(rapidjson::Value&, rapidjson::Document::AllocatorType&, AZStd::string& detail)
        {
            detail = "Provider has no pending operation";
            return Result::InvalidArguments;
        }
        //! Disconnect/deadline/shutdown cancels queued work; already completed native effects are not rolled back.
        virtual void CancelPending() {}
    };
}
