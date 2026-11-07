/* Copyright (c) Contributors to the Open 3D Engine Project.
 * SPDX-License-Identifier: Apache-2.0 OR MIT */
#include "BindingRecipe.h"

namespace LocalMcp
{
    Json BindingRecipe::Call(AZStd::string_view binding, Allocator& allocator,
        AZStd::initializer_list<const Json*> inputs, const Json* address, const Json* target)
    {
        Json request(rapidjson::kObjectType);
        request.AddMember("binding", String(binding, allocator), allocator);
        Json args(rapidjson::kArrayType);
        for (const auto* input : inputs) { args.PushBack(Json(*input, allocator), allocator); }
        request.AddMember("args", args, allocator);
        if (address) { request.AddMember("address", Json(*address, allocator), allocator); }
        if (target) { request.AddMember("target", Json(*target, allocator), allocator); }
        Json data(rapidjson::kObjectType);
        AZStd::string detail;
        const auto result = m_bridge.Invoke("binding_call", request, data, allocator, detail);
        if (result != ToolProvider::Result::Complete || data.HasMember("error"))
        {
            throw BridgeError(AZStd::string::format("%.*s: %s", aznumeric_cast<int>(binding.size()), binding.data(),
                data.HasMember("error") ? Serialize(data["error"]).c_str() : detail.c_str()).c_str());
        }
        if (!data.HasMember("value")) { throw BridgeError("Binding result is missing its value"); }
        const Json* value = &data["value"];
        if (value->IsObject() && IsString(*value, "$type") && AZStd::string_view((*value)["$type"].GetString()) == "Outcome")
        {
            if (!(*value)["success"].GetBool())
            {
                throw BridgeError(AZStd::string::format("%.*s: %s", aznumeric_cast<int>(binding.size()), binding.data(),
                    Serialize((*value)["error"]).c_str()).c_str());
            }
            value = &(*value)["value"];
        }
        return Json(*value, allocator);
    }

    void BindingRecipe::Release(const Json& handle, Allocator& allocator)
    {
        Json request(rapidjson::kObjectType);
        Json handles(rapidjson::kArrayType);
        handles.PushBack(Json(handle["$handle"], allocator), allocator);
        request.AddMember("handles", handles, allocator);
        Json data(rapidjson::kObjectType);
        AZStd::string detail;
        const auto result = m_bridge.Invoke("binding_release", request, data, allocator, detail);
        if (result != ToolProvider::Result::Complete || data.HasMember("error"))
        {
            throw BridgeError("Could not release the recipe's temporary binding handle");
        }
    }
}
