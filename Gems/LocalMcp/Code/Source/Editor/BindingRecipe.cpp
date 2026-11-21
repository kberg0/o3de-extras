/*
 * Copyright (c) Contributors to the Open 3D Engine Project.
 * For complete copyright and license terms please see the LICENSE at the root of this distribution.
 *
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 *
 */

#include "BindingRecipe.h"

namespace LocalMcp
{
    namespace
    {
        //! "<binding>: <reason>", the message of a recipe step that failed.
        AZStd::string StepFailure(AZStd::string_view binding, AZStd::string_view reason)
        {
            return AZStd::string(binding) + ": " + AZStd::string(reason);
        }
    } // namespace

    BindingRecipe::BindingRecipe(BehaviorBridge& bridge)
        : m_bridge(bridge)
    {
    }

    BridgeResult<Json> BindingRecipe::Call(
        AZStd::string_view binding,
        Allocator& allocator,
        AZStd::initializer_list<const Json*> args,
        const Json* address,
        const Json* target)
    {
        Json values(rapidjson::kArrayType);
        for (const Json* value : args)
        {
            values.PushBack(Json(*value, allocator), allocator);
        }

        Json request(rapidjson::kObjectType);
        request.AddMember("binding", String(binding, allocator), allocator);
        request.AddMember("args", values, allocator);
        if (address)
        {
            request.AddMember("address", Json(*address, allocator), allocator);
        }
        if (target)
        {
            request.AddMember("target", Json(*target, allocator), allocator);
        }

        Json data(rapidjson::kObjectType);
        AZStd::string detail;
        const ToolProvider::Result result = m_bridge.Invoke(BindingCallTool, request, data, allocator, detail);
        if (data.HasMember("error"))
        {
            return AZ::Failure(StepFailure(binding, Serialize(data["error"])));
        }
        if (result != ToolProvider::Result::Complete)
        {
            return AZ::Failure(StepFailure(binding, detail));
        }
        if (!data.HasMember("value"))
        {
            return AZ::Failure(AZStd::string("Binding result is missing its value"));
        }

        const Json* value = &data["value"];
        const bool outcome =
            value->IsObject() && IsString(*value, "$type") && AZStd::string_view((*value)["$type"].GetString()) == "Outcome";
        if (outcome)
        {
            if (!(*value)["success"].GetBool())
            {
                return AZ::Failure(StepFailure(binding, Serialize((*value)["error"])));
            }
            value = &(*value)["value"];
        }
        return AZ::Success(Json(*value, allocator));
    }

    BridgeResult<void> BindingRecipe::Release(const Json& handle, Allocator& allocator)
    {
        Json handles(rapidjson::kArrayType);
        handles.PushBack(Json(handle["$handle"], allocator), allocator);

        Json request(rapidjson::kObjectType);
        request.AddMember("handles", handles, allocator);

        Json data(rapidjson::kObjectType);
        AZStd::string detail;
        const ToolProvider::Result result = m_bridge.Invoke(BindingReleaseTool, request, data, allocator, detail);
        if (result != ToolProvider::Result::Complete || data.HasMember("error"))
        {
            return AZ::Failure(AZStd::string("Could not release the recipe's temporary binding handle"));
        }
        return AZ::Success();
    }

    BridgeResult<Json> BindingRecipe::Normalize(const Json& value, const AZ::TypeId& type, Allocator& allocator)
    {
        return m_bridge.NormalizeValue(value, type, allocator);
    }
} // namespace LocalMcp
