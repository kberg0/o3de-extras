/*
 * Copyright (c) Contributors to the Open 3D Engine Project.
 * For complete copyright and license terms please see the LICENSE at the root of this distribution.
 *
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 *
 */

#pragma once

#include <AzCore/Casting/numeric_cast.h>
#include <AzCore/JSON/document.h>
#include <AzCore/JSON/stringbuffer.h>
#include <AzCore/JSON/writer.h>
#include <AzCore/std/algorithm.h>
#include <AzCore/std/string/string.h>
#include <AzCore/std/string/string_view.h>
#include <AzCore/std/string/utf8/core.h>

namespace LocalMcp
{
    //! JSON value type used by every tool request and reply.
    using Json = rapidjson::Value;

    //! Allocator of the document that owns a reply.
    using Allocator = rapidjson::Document::AllocatorType;

    //! Default upper bound, in bytes, for a string argument checked by IsString().
    inline constexpr size_t MaxStringArgumentLength = 65536;

    //! Copies text into a JSON string, cut at the first invalid UTF-8 sequence.
    //! Log lines and cvar values are truncated by byte count, so they can end inside a multi-byte character.
    inline Json String(AZStd::string_view text, Allocator& allocator)
    {
        if (text.empty())
        {
            return Json("", 0, allocator);
        }

        const size_t validSize = Utf8::find_invalid(text.begin(), text.end()) - text.begin();
        return Json(text.data(), aznumeric_cast<rapidjson::SizeType>(validSize), allocator);
    }

    //! Writes value as compact JSON text.
    inline AZStd::string Serialize(const Json& value)
    {
        rapidjson::StringBuffer buffer;
        rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
        value.Accept(writer);
        return { buffer.GetString(), buffer.GetSize() };
    }

    //! True when value is a string of minLength to maxLength bytes with no embedded NUL.
    inline bool IsBoundedString(const Json& value, size_t minLength, size_t maxLength)
    {
        if (!value.IsString())
        {
            return false;
        }

        const size_t length = value.GetStringLength();
        return length >= minLength && length <= maxLength && strlen(value.GetString()) == length;
    }

    //! True when object has a string member name of at most maxLength bytes with no embedded NUL.
    inline bool IsString(const Json& object, const char* name, size_t maxLength = MaxStringArgumentLength)
    {
        return object.HasMember(name) && IsBoundedString(object[name], 0, maxLength);
    }

    //! True when value equals one of options.
    inline bool IsOneOf(AZStd::string_view value, AZStd::initializer_list<AZStd::string_view> options)
    {
        return AZStd::find(options.begin(), options.end(), value) != options.end();
    }

    //! Reply that one tools/call request builds; passed to the generated tool handlers.
    struct ToolCall
    {
        Json& m_data;
        Allocator& m_allocator;
        //! Why the arguments were rejected, when a handler returns false.
        AZStd::string m_error;
        //! The connection's held logs_wait id, or nullptr when the request has no connection that can be held.
        uint64_t* m_waitId = nullptr;
        //! Set by a handler whose call continues over later polls; Dispatch() then returns ToolProvider::Result::Pending.
        bool m_pending = false;
    };
} // namespace LocalMcp
