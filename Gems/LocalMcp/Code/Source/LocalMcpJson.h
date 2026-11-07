/* Copyright (c) Contributors to the Open 3D Engine Project.
 * SPDX-License-Identifier: Apache-2.0 OR MIT */
#pragma once
#include <AzCore/JSON/document.h>
#include <AzCore/JSON/stringbuffer.h>
#include <AzCore/JSON/writer.h>
#include <AzCore/std/string/utf8/core.h>
namespace LocalMcp
{
    using Json = rapidjson::Value;
    using Allocator = rapidjson::Document::AllocatorType;
    inline Json String(AZStd::string_view text, Allocator& allocator)
        {
            // Truncated log/cvar strings must still produce valid UTF-8 JSON.
            const size_t size = text.empty() ? 0 : Utf8::find_invalid(text.begin(), text.end()) - text.begin();
            return Json(text.empty() ? "" : text.data(), aznumeric_cast<rapidjson::SizeType>(size), allocator);
        }

    inline AZStd::string Serialize(const Json& value)
        {
            rapidjson::StringBuffer buffer;
            rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
            value.Accept(writer);
            return { buffer.GetString(), buffer.GetSize() };
        }

    inline bool IsString(const Json& object, const char* name, size_t maxLength = 65536)
    {
        return object.HasMember(name) && object[name].IsString() && object[name].GetStringLength() <= maxLength
            && strlen(object[name].GetString()) == object[name].GetStringLength();
    }
}
