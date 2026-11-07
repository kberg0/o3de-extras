/* Copyright (c) Contributors to the Open 3D Engine Project.
 * SPDX-License-Identifier: Apache-2.0 OR MIT */
#pragma once
#include "BehaviorBridge.h"

namespace LocalMcp
{
    //! Convenience recipes use the same invocation, validation and lifetime rules as binding_call.
    class BindingRecipe
    {
    public:
        explicit BindingRecipe(BehaviorBridge& bridge) : m_bridge(bridge) {}
        Json Call(AZStd::string_view binding, Allocator& allocator,
            AZStd::initializer_list<const Json*> args = {}, const Json* address = nullptr, const Json* target = nullptr);
        void Release(const Json& handle, Allocator& allocator);
        Json Normalize(const Json& value, const AZ::TypeId& type, Allocator& allocator)
        { return m_bridge.NormalizeValue(value, type, allocator); }
    private:
        BehaviorBridge& m_bridge;
    };
}
