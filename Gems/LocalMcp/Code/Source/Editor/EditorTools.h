/*
 * Copyright (c) Contributors to the Open 3D Engine Project.
 * For complete copyright and license terms please see the LICENSE at the root of this distribution.
 *
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 *
 */

#pragma once

#include "AssetImport.h"
#include "BindingRecipe.h"
#include "Screenshot.h"

#include <AzToolsFramework/Prefab/PrefabPublicNotificationBus.h>
#include <Source/Editor/AutoGen/LocalMcpEditorTools.McpTools.h>

#include <QTimer>

namespace LocalMcp
{
    //! Serves the editor tools: entities, components, levels, prefabs, asset import, play mode, undo, Python and screenshots.
    //! editor_info and editor_state answer at once. Every other tool is queued: Invoke() checks it and returns Pending, a
    //! zero-length timer runs it outside the request, and PollPending() answers once its native effects have settled.
    //! At most one tool is queued or running; every call happens on the editor main thread.
    class EditorTools final
        : private EditorToolsHandler
        , private AzToolsFramework::Prefab::PrefabPublicNotificationBus::Handler
    {
    public:
        explicit EditorTools(BehaviorBridge& bridge);

        //! connectionTimeoutMs is the server's idle timeout, which bounds how long a pending tool may run.
        //! pythonExecute lists and serves python_execute; it is read from the settings once, so MCP clients cannot change it.
        void Activate(AZ::s64 connectionTimeoutMs, bool pythonExecute);

        void Deactivate();

        //! Appends the editor tool schemas to the tools/list array; python_execute only when it is enabled.
        void AddTools(Json& tools, Allocator& allocator) const;

        //! Runs a read, or queues any other tool and returns Pending. Returns Unknown when name is not an editor tool.
        ToolProvider::Result Invoke(AZStd::string_view name, const Json& args, Json& data, Allocator& allocator, AZStd::string& detail);

        //! Returns Pending until the queued tool and its native effects have finished, then its reply.
        ToolProvider::Result PollPending(Json& data, Allocator& allocator, AZStd::string& detail);

        //! Drops the queued tool. Native changes that already happened are kept.
        void CancelPending();

    private:
        //! What a tool needs from the editor before it runs.
        enum class Requirement
        {
            //! Nothing: editor_stop, python_execute and screenshot.
            None,
            //! Not playing, no prefab propagation pending, and no level loading.
            EditableDocument,
            //! EditableDocument with a ready level.
            ReadyLevel
        };

        //! Default connection timeout until Activate() sets it.
        static constexpr AZ::s64 DefaultConnectionTimeoutMs = 10000;

        //! Fails unless the editor meets requirement.
        BridgeResult<void> Require(Requirement requirement, Allocator& allocator);

        //! Fails while a tool is queued or running, or unless the editor meets requirement.
        BridgeResult<void> RequireIdle(Requirement requirement, Allocator& allocator);

        //! On the first call for a tool, checks RequireIdle(), queues it by setting call.m_pending and returns true.
        //! When RunPending() runs the queued call, checks the requirement again and returns false, so the handler runs.
        //! A requirement that is not met is reported with Fail(), and true is returned.
        bool Deferred(Requirement requirement, ToolCall& call);

        //! Adds error to the reply as data.error and returns true, so the handler completes with that error.
        //! The queued call that RunPending() runs also gets data.partialChangesPossible.
        bool Fail(ToolCall& call, AZStd::string_view error) const;

        //! Makes result's value the whole reply, or reports its error with Fail(). Returns true.
        bool Reply(BridgeResult<Json> result, ToolCall& call) const;

        //! Records the queued tool and its arguments, and starts the timer that runs it.
        void BeginPending(AZStd::string_view name, const Json& args);

        //! Runs the queued tool by dispatching it again, and stores its reply for PollPending().
        void RunPending();

        //! Forgets the queued tool and stops any import or capture it started.
        void ResetPending();

        //! The editor_state reply.
        BridgeResult<Json> State(Allocator& allocator);

        //! The container entity of the focused prefab.
        BridgeResult<Json> Focus(Allocator& allocator);

        BridgeResult<bool> Playing(Allocator& allocator);

        BridgeResult<bool> PropagationPending(Allocator& allocator);

        //! Applies the unsaved choice of level_create or level_open to the current level and dirty prefabs, and returns it.
        BridgeResult<AZStd::string_view> ReleaseCurrentLevel(const AZStd::optional<AZStd::string>& unsaved, Allocator& allocator);

        // EditorToolsHandler overrides
        bool HandleEditorInfo(const EditorInfoArgs& args, ToolCall& call) override;
        bool HandleEntityCreate(const EntityCreateArgs& args, ToolCall& call) override;
        bool HandleEntityDelete(const EntityDeleteArgs& args, ToolCall& call) override;
        bool HandleEntitySetParent(const EntitySetParentArgs& args, ToolCall& call) override;
        bool HandleTransformSet(const TransformSetArgs& args, ToolCall& call) override;
        bool HandleComponentAdd(const ComponentAddArgs& args, ToolCall& call) override;
        bool HandleComponentRemove(const ComponentRemoveArgs& args, ToolCall& call) override;
        bool HandleComponentSet(const ComponentSetArgs& args, ToolCall& call) override;
        bool HandleLevelCreate(const LevelCreateArgs& args, ToolCall& call) override;
        bool HandleLevelOpen(const LevelOpenArgs& args, ToolCall& call) override;
        bool HandleLevelSave(const LevelSaveArgs& args, ToolCall& call) override;
        bool HandlePrefabCreate(const PrefabCreateArgs& args, ToolCall& call) override;
        bool HandlePrefabSave(const PrefabSaveArgs& args, ToolCall& call) override;
        bool HandleAssetImport(const AssetImportArgs& args, ToolCall& call) override;
        bool HandleEditorPlay(const EditorPlayArgs& args, ToolCall& call) override;
        bool HandleEditorStop(const EditorStopArgs& args, ToolCall& call) override;
        bool HandleEditorState(const EditorStateArgs& args, ToolCall& call) override;
        bool HandleEditorUndo(const EditorUndoArgs& args, ToolCall& call) override;
        bool HandleEditorRedo(const EditorRedoArgs& args, ToolCall& call) override;
        bool HandlePythonExecute(const PythonExecuteArgs& args, ToolCall& call) override;
        bool HandleScreenshot(const ScreenshotArgs& args, ToolCall& call) override;

        // PrefabPublicNotificationBus overrides
        void OnPrefabInstancePropagationBegin() override;
        void OnPrefabInstancePropagationEnd() override;

        BindingRecipe m_bindings;
        AssetImport m_import;
        Screenshot m_screenshot;
        QTimer m_timer;
        AZ::s64 m_connectionTimeoutMs = DefaultConnectionTimeoutMs;
        bool m_pythonExecute = false;

        //! A tool is queued; it may be running or finished.
        bool m_active = false;
        //! The queued call while RunPending() runs it, otherwise nullptr.
        const ToolCall* m_running = nullptr;
        //! The queued tool finished running and m_result holds its reply.
        bool m_finished = false;
        bool m_propagating = false;
        AZStd::string m_tool;
        rapidjson::Document m_args;
        rapidjson::Document m_result;
        AZStd::chrono::steady_clock::time_point m_until;
    };
} // namespace LocalMcp
