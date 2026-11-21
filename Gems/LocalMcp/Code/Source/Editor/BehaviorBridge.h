/*
 * Copyright (c) Contributors to the Open 3D Engine Project.
 * For complete copyright and license terms please see the LICENSE at the root of this distribution.
 *
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 *
 */

#pragma once

#include "BehaviorBindings.h"
#include "BindingModules.h"
#include "BridgeValues.h"
#include "LocalMcpJson.h"
#include "LocalMcpToolProvider.h"

#include <AzCore/std/functional.h>
#include <AzCore/std/optional.h>
#include <AzCore/std/parallel/thread.h>
#include <AzToolsFramework/API/EntityCompositionNotificationBus.h>
#include <AzToolsFramework/Entity/EditorEntityContextBus.h>
#include <EditorPythonBindings/EditorPythonBindingsBus.h>
#include <Source/Editor/AutoGen/LocalMcpBindingTools.McpTools.h>

namespace LocalMcp
{
    //! Serves the binding tools: search, browse, describe, call, get and set any BehaviorContext entry that is scoped for
    //! editor automation, through the EditorPythonBindings marshaller, and the typed tools of the modules an agent opened.
    //! Object handles are released whenever entities, classes or the Python interpreter that they refer to change.
    class BehaviorBridge final
        : private BindingToolsHandler
        , private EditorPythonBindings::EditorPythonBindingsNotificationBus::Handler
        , private AzToolsFramework::EditorEntityContextNotificationBus::Handler
        , private AzToolsFramework::EntityCompositionNotificationBus::Handler
        , private AZ::BehaviorContextBus::Handler
    {
    public:
        //! Records the calling thread as the main thread and connects to the buses that invalidate handles.
        void Activate();

        void Deactivate();

        //! Releases every object handle, with the Python lock held.
        void ClearHandles();

        //! Appends the binding tool schemas and the typed tools of the open modules to the tools/list array.
        void AddTools(Json& tools, Allocator& allocator) const;

        //! Changes whenever AddTools() would append a different set of tools.
        AZ::u64 GetToolsRevision() const;

        //! Runs a binding tool or a typed tool of an open module. Returns Unknown when tool is neither.
        ToolProvider::Result Invoke(AZStd::string_view tool, const Json& args, Json& data, Allocator& allocator, AZStd::string& detail);

        //! Converts value to type and back through the same codec the tools use, without invoking any native method.
        //! Recipes use it to validate and canonicalize values.
        BridgeResult<Json> NormalizeValue(const Json& value, const AZ::TypeId& type, Allocator& allocator);

    private:
        using Kind = Bindings::Kind;
        using BindingView = Bindings::BindingView;

        //! What Execute() does with a binding.
        enum class Access
        {
            Call,
            Get,
            Set
        };

        //! The arguments of binding_call, binding_get or binding_set that Execute() reads; absent ones are nullptr.
        struct Request
        {
            Access m_access = Access::Call;
            const Json* m_args = nullptr;
            const Json* m_value = nullptr;
            const Json* m_target = nullptr;
            const Json* m_address = nullptr;
            AZStd::optional<size_t> m_overload;
        };

        //! Name, documentation, scope and whether the binding is callable, with the reason when it is not.
        static Json Metadata(const BindingView& binding, Allocator& allocator);

        //! Writes the binding's metadata and native signatures into data.
        void Describe(const BindingView& binding, Json& data, Allocator& allocator);

        //! Calls, reads or writes the binding and writes the result into data.
        BridgeResult<void> Execute(const BindingView& binding, const Request& request, Json& data, Allocator& allocator);

        //! True on the main thread; otherwise reports data.error.
        bool OnMainThread(ToolCall& call) const;

        //! Runs work with the Python lock held and azlmbr.object imported. A failure becomes data.error.
        void WithPython(ToolCall& call, const AZStd::function<BridgeResult<void>()>& work);

        //! Runs work on the binding with ID id, or reports that it no longer exists.
        void WithBinding(AZStd::string_view id, ToolCall& call, const AZStd::function<BridgeResult<void>(const BindingView&)>& work);

        //! Runs a binding_call, binding_get or binding_set request.
        void RunRequest(AZStd::string_view id, const Request& request, ToolCall& call);

        //! Runs a call of an open module's typed tool as the binding_call it stands for.
        ToolProvider::Result InvokeModuleTool(const ModuleTool& tool, const Json& args, ToolCall& call);

        // BindingToolsHandler overrides
        bool HandleBindingSearch(const BindingSearchArgs& args, ToolCall& call) override;
        bool HandleBindingBrowse(const BindingBrowseArgs& args, ToolCall& call) override;
        bool HandleBindingOpenModule(const BindingOpenModuleArgs& args, ToolCall& call) override;
        bool HandleBindingCloseModule(const BindingCloseModuleArgs& args, ToolCall& call) override;
        bool HandleBindingDescribe(const BindingDescribeArgs& args, ToolCall& call) override;
        bool HandleBindingCall(const BindingCallArgs& args, ToolCall& call) override;
        bool HandleBindingGet(const BindingGetArgs& args, ToolCall& call) override;
        bool HandleBindingSet(const BindingSetArgs& args, ToolCall& call) override;
        bool HandleBindingRelease(const BindingReleaseArgs& args, ToolCall& call) override;

        // EditorPythonBindingsNotificationBus overrides
        void OnPreFinalize() override;

        // EntityCompositionNotificationBus overrides
        void OnEntityCompositionChanging(const AzToolsFramework::EntityIdList& entities) override;

        // BehaviorContextBus overrides
        void OnRemoveClass(const char* className, AZ::BehaviorClass* behaviorClass) override;

        // EditorEntityContextNotificationBus overrides
        void OnPrepareForContextReset() override;
        void OnEditorEntityDeleted(const AZ::EntityId& entity) override;

        BridgeValues m_values;
        ModuleTools m_modules;
        AZStd::thread_id m_mainThread;
    };
} // namespace LocalMcp
