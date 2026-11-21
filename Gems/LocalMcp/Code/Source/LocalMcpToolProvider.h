/*
 * Copyright (c) Contributors to the Open 3D Engine Project.
 * For complete copyright and license terms please see the LICENSE at the root of this distribution.
 *
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 *
 */

#pragma once

#include <AzCore/JSON/document.h>
#include <AzCore/base.h>
#include <AzCore/std/string/string.h>

namespace LocalMcp
{
    //! Member of a completed tool reply that carries an image: an object with the base64 "data" and its "mimeType".
    //! The Server removes it from the reply and sends it as an MCP image content block after the text block.
    inline constexpr const char* ImageReplyMember = "$image";

    //! Process-specific tools served next to the runtime tools by one Server.
    //! The component that owns the Server owns the provider and destroys the Server first.
    //! Every call is made synchronously on the main thread, from Server::Poll().
    class ToolProvider
    {
    public:
        //! Outcome of a tool call.
        enum class Result
        {
            //! The provider has no tool with this name.
            Unknown,
            //! The call finished; a tool failure is reported as data.error.
            Complete,
            //! The arguments were malformed; detail says why.
            InvalidArguments,
            //! The call continues over later polls; see PollPending().
            Pending
        };

        virtual ~ToolProvider() = default;

        //! Name reported as serverInfo.name by initialize.
        virtual const char* GetServerName() const = 0;

        //! Appends this provider's tool schemas to the tools/list array.
        virtual void AddTools(rapidjson::Value& tools, rapidjson::Document::AllocatorType& allocator) const = 0;

        //! Revision of the tool list; it changes whenever AddTools() would append a different set of tools.
        //! The Server compares it before and after each request, and tells the client when a request changed it.
        virtual AZ::u64 GetToolsRevision() const
        {
            return 0;
        }

        //! Runs tool name with args, writing the reply into data.
        //! A completed tool failure uses data.error; malformed arguments return InvalidArguments with detail set.
        //! At most one provider operation is active: Invoke must refuse a second mutation while one is pending.
        virtual Result Invoke(
            AZStd::string_view name,
            const rapidjson::Value& args,
            rapidjson::Value& data,
            rapidjson::Document::AllocatorType& allocator,
            AZStd::string& detail) = 0;

        //! Resumes the pending operation without invoking the mutation again.
        //! The Server holds the original connection and request id until this returns something other than Pending.
        virtual Result PollPending(
            [[maybe_unused]] rapidjson::Value& data, [[maybe_unused]] rapidjson::Document::AllocatorType& allocator, AZStd::string& detail)
        {
            detail = "Provider has no pending operation";
            return Result::InvalidArguments;
        }

        //! Called on disconnect, deadline or shutdown to drop queued work.
        //! Native effects that already completed are not rolled back.
        virtual void CancelPending()
        {
        }
    };
} // namespace LocalMcp
