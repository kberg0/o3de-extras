/*
 * Copyright (c) Contributors to the Open 3D Engine Project.
 * For complete copyright and license terms please see the LICENSE at the root of this distribution.
 *
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 *
 */

#pragma once

#include "BehaviorBridge.h"

namespace LocalMcp
{
    //! Runs reflected bindings for an editor tool, with the same invocation, validation and lifetime rules as binding_call.
    //! Every function returns the reason it failed as its error.
    class BindingRecipe
    {
    public:
        explicit BindingRecipe(BehaviorBridge& bridge);

        //! Calls binding with copies of args, and the address and target when given, and returns its value.
        //! An Outcome result is unwrapped to its value; a failed Outcome is an error.
        BridgeResult<Json> Call(
            AZStd::string_view binding,
            Allocator& allocator,
            AZStd::initializer_list<const Json*> args = {},
            const Json* address = nullptr,
            const Json* target = nullptr);

        //! Releases the object handle that a Call() returned.
        BridgeResult<void> Release(const Json& handle, Allocator& allocator);

        //! Converts value to type and back without calling a binding; see BehaviorBridge::NormalizeValue().
        BridgeResult<Json> Normalize(const Json& value, const AZ::TypeId& type, Allocator& allocator);

    private:
        BehaviorBridge& m_bridge;
    };
} // namespace LocalMcp
