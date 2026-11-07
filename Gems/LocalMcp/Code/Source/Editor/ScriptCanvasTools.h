/* Copyright (c) Contributors to the Open 3D Engine Project.
 * SPDX-License-Identifier: Apache-2.0 OR MIT */
#pragma once
#include "BindingRecipe.h"

namespace LocalMcp
{
    //! Graph operations use the existing editor request queue and ScriptCanvas's native owner.
    class ScriptCanvasTools
    {
    public:
        explicit ScriptCanvasTools(BehaviorBridge& bridge) : m_bindings(bridge) {}
        void Invoke(AZStd::string_view name, const Json& args, Json& data, Allocator& allocator);
        bool PollSave(Json& data, Allocator& allocator);
        void CancelSave();
    private:
        BindingRecipe m_bindings;
        bool m_saving = false;
    };
}
