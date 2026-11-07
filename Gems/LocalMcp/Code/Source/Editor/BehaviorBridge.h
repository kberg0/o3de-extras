/* Copyright (c) Contributors to the Open 3D Engine Project.
 * SPDX-License-Identifier: Apache-2.0 OR MIT */
#pragma once
#include "LocalMcpJson.h"
#include "BridgeValues.h"
#include "BehaviorBindings.h"
#include <AzToolsFramework/API/EntityCompositionNotificationBus.h>
#include "LocalMcpToolProvider.h"
#include <EditorPythonBindings/PythonUtility.h>
#include <EditorPythonBindings/EditorPythonBindingsBus.h>
#include <AzToolsFramework/Entity/EditorEntityContextBus.h>
#include <AzCore/std/containers/unordered_map.h>
#include <AzCore/std/functional.h>
#include <AzCore/std/parallel/thread.h>

namespace LocalMcp
{
    class BehaviorBridge final : private EditorPythonBindings::EditorPythonBindingsNotificationBus::Handler,
        private AzToolsFramework::EditorEntityContextNotificationBus::Handler,
        private AzToolsFramework::EntityCompositionNotificationBus::Handler, private AZ::BehaviorContextBus::Handler
    {
    public:
        void Activate();
        void Deactivate();
        void ClearHandles();
        static void AddTools(Json& tools, Allocator& allocator);
        ToolProvider::Result Invoke(AZStd::string_view tool, const Json& args, Json& data, Allocator& allocator,
            AZStd::string& detail);
        //! Recipes prevalidate/canonicalize values through the same codec; no native mutation is invoked.
        Json NormalizeValue(const Json& value, const AZ::TypeId& type, Allocator& allocator);
    private:
        using Kind = Bindings::Kind;
        using BindingView = Bindings::BindingView;
        static Json Metadata(const BindingView& binding, Allocator& allocator);
        void Describe(const BindingView& binding, Json& data, Allocator& allocator);
        void Execute(const BindingView& binding, AZStd::string_view tool, const Json& args, Json& data, Allocator& allocator);
        void OnPreFinalize() override;
        void OnEntityCompositionChanging(const AzToolsFramework::EntityIdList&) override { ClearHandles(); }
        void OnRemoveClass(const char*, AZ::BehaviorClass*) override { ClearHandles(); }
        void OnPrepareForContextReset() override { ClearHandles(); }
        void OnEditorEntityDeleted(const AZ::EntityId&) override { ClearHandles(); }
        BridgeValues m_values;
        AZStd::thread_id m_mainThread;
    };
}
