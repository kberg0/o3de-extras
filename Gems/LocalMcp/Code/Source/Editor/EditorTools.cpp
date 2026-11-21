/*
 * Copyright (c) Contributors to the Open 3D Engine Project.
 * For complete copyright and license terms please see the LICENSE at the root of this distribution.
 *
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 *
 */

#include "EditorTools.h"

#include "PythonExecute.h"

#include <AzCore/Math/Transform.h>
#include <AzCore/std/algorithm.h>
#include <AzToolsFramework/API/ToolsApplicationAPI.h>

#include <QGuiApplication>

namespace LocalMcp
{
    namespace
    {
        //! Prefix of the undo batch name an edit tool records, followed by the tool name.
        constexpr AZStd::string_view UndoBatchPrefix = "LocalMcp ";

        //! The space value of transform_set that selects the local transform.
        constexpr AZStd::string_view LocalSpace = "local";

        // The unsaved values of level_create and level_open.
        constexpr AZStd::string_view UnsavedReject = "reject";
        constexpr AZStd::string_view UnsavedSave = "save";
        constexpr AZStd::string_view UnsavedDiscard = "discard";

        //! entity_create position when the request gives none.
        constexpr const char* OriginPosition = R"({"$type":"Vector3","value":[0,0,0]})";

        // Bindings that more than one tool calls.
        constexpr AZStd::string_view LevelPathBinding = "global/method/get_level_document_path";
        constexpr AZStd::string_view LevelReadyBinding = "global/method/is_level_document_ready";
        constexpr AZStd::string_view LevelModifiedBinding = "global/method/is_level_document_modified";
        constexpr AZStd::string_view SaveLevelBinding = "global/method/save_level";
        constexpr AZStd::string_view InGameModeBinding = "bus/PythonEditorBus/event/IsInGameMode";
        constexpr AZStd::string_view DirtyPrefabPathsBinding = "bus/PrefabPublicRequestBus/event/GetDirtyPrefabPaths";
        constexpr AZStd::string_view GetLocalTransformBinding = "bus/TransformBus/event/GetLocalTM";
        constexpr AZStd::string_view GetWorldTransformBinding = "bus/TransformBus/event/GetWorldTM";

        //! The editor's reflected list of view pane names, which screenshot checks a panel name against.
        constexpr AZStd::string_view PaneNamesBinding = "global/method/get_pane_class_names";

        //! The undo batch name of an edit tool.
        AZStd::string UndoName(AZStd::string_view tool)
        {
            return AZStd::string(UndoBatchPrefix) + AZStd::string(tool);
        }

        //! A JSON array of strings.
        Json StringArray(const AZStd::vector<AZStd::string>& values, Allocator& allocator)
        {
            Json result(rapidjson::kArrayType);
            for (const AZStd::string& value : values)
            {
                result.PushBack(String(value, allocator), allocator);
            }
            return result;
        }
    } // namespace

    EditorTools::EditorTools(BehaviorBridge& bridge)
        : m_bindings(bridge)
    {
        m_timer.setSingleShot(true);
        QObject::connect(
            &m_timer,
            &QTimer::timeout,
            [this]
            {
                RunPending();
            });
    }

    void EditorTools::Activate(AZ::s64 connectionTimeoutMs, bool pythonExecute)
    {
        m_connectionTimeoutMs = connectionTimeoutMs;
        m_pythonExecute = pythonExecute;
        AzToolsFramework::Prefab::PrefabPublicNotificationBus::Handler::BusConnect();
        m_import.Activate();
    }

    void EditorTools::Deactivate()
    {
        CancelPending();
        m_import.Deactivate();
        AzToolsFramework::Prefab::PrefabPublicNotificationBus::Handler::BusDisconnect();
    }

    void EditorTools::AddTools(Json& tools, Allocator& allocator) const
    {
        rapidjson::Document schemas;
        schemas.Parse(EditorToolsSchemas);
        AZ_Assert(!schemas.HasParseError(), "Generated editor tool schemas are not valid JSON");

        for (const Json& schema : schemas["tools"].GetArray())
        {
            if (m_pythonExecute || AZStd::string_view(schema["name"].GetString()) != PythonExecuteTool)
            {
                tools.PushBack(Json(schema, allocator), allocator);
            }
        }
    }

    ToolProvider::Result EditorTools::Invoke(
        AZStd::string_view name, const Json& args, Json& data, Allocator& allocator, AZStd::string& detail)
    {
        if (!m_pythonExecute && name == PythonExecuteTool)
        {
            return ToolProvider::Result::Unknown;
        }

        ToolCall call{ data, allocator };
        const ToolProvider::Result result = Dispatch(name, args, call);
        detail = AZStd::move(call.m_error);
        if (result == ToolProvider::Result::Pending)
        {
            BeginPending(name, args);
        }
        return result;
    }

    ToolProvider::Result EditorTools::PollPending(Json& data, Allocator& allocator, AZStd::string& detail)
    {
        if (!m_active)
        {
            detail = "No active editor operation";
            return ToolProvider::Result::InvalidArguments;
        }

        if (m_tool == AssetImportTool)
        {
            const ToolProvider::Result result = m_import.Poll(data, allocator);
            if (result != ToolProvider::Result::Pending)
            {
                ResetPending();
            }
            return result;
        }

        if (AZStd::chrono::steady_clock::now() >= m_until)
        {
            data.AddMember("error", "Editor operation timed out; inspect editor_state and logs before any further edit", allocator);
            data.AddMember("partialChangesPossible", m_running || m_finished, allocator);
            ResetPending();
            return ToolProvider::Result::Complete;
        }

        if (!m_finished || m_running)
        {
            return ToolProvider::Result::Pending;
        }

        // A failure after the tool ran is reported next to the tool's own error, if it has one.
        auto fail = [&](const AZStd::string& error)
        {
            Json name = String(data.HasMember("error") ? "stateError" : "error", allocator);
            Json message = String(error, allocator);
            data.AddMember(name, message, allocator);
            if (!data.HasMember("partialChangesPossible"))
            {
                data.AddMember("partialChangesPossible", true, allocator);
            }
            ResetPending();
            return ToolProvider::Result::Complete;
        };

        if (!m_result.HasMember("error"))
        {
            // screenshot waits for its capture; a failed capture is the tool's own error.
            if (m_tool == ScreenshotTool)
            {
                Allocator& resultAllocator = m_result.GetAllocator();
                auto captured = m_screenshot.Poll(m_result, resultAllocator);
                if (!captured)
                {
                    m_result.AddMember("error", String(captured.GetError(), resultAllocator), resultAllocator);
                }
                else if (!captured.GetValue())
                {
                    return ToolProvider::Result::Pending;
                }
            }

            // Native prefab propagation is paused during play.
            if (m_tool != EditorPlayTool)
            {
                auto propagating = PropagationPending(allocator);
                if (!propagating)
                {
                    return fail(propagating.GetError());
                }
                if (propagating.GetValue())
                {
                    return ToolProvider::Result::Pending;
                }
            }

            // editor_play waits until play starts, and editor_stop until it stops.
            if (m_tool == EditorPlayTool || m_tool == EditorStopTool)
            {
                auto playing = Playing(allocator);
                if (!playing)
                {
                    return fail(playing.GetError());
                }
                if (playing.GetValue() != (m_tool == EditorPlayTool))
                {
                    return ToolProvider::Result::Pending;
                }
            }

            const bool loadsLevel = m_tool == LevelCreateTool || m_tool == LevelOpenTool;
            if (loadsLevel)
            {
                auto ready = m_bindings.Call(LevelReadyBinding, allocator);
                if (!ready)
                {
                    return fail(ready.GetError());
                }
                if (!ready.GetValue().GetBool())
                {
                    return ToolProvider::Result::Pending;
                }
            }
        }

        data.CopyFrom(m_result, allocator);
        ResetPending();
        auto state = State(allocator);
        if (!state)
        {
            return fail(state.GetError());
        }
        data.AddMember("state", state.GetValue(), allocator);
        return ToolProvider::Result::Complete;
    }

    void EditorTools::CancelPending()
    {
        const bool started = m_running || m_finished || m_import.IsActive();
        if (m_active && started)
        {
            AZ_Warning(
                "LocalMcp",
                false,
                "Canceled active %s; native changes and copied source files are retained. Inspect editor_state and logs.",
                m_tool.c_str());
        }
        ResetPending();
    }

    BridgeResult<void> EditorTools::Require(Requirement requirement, Allocator& allocator)
    {
        if (requirement == Requirement::None)
        {
            return AZ::Success();
        }

        auto playing = Playing(allocator);
        if (!playing)
        {
            return AZ::Failure(playing.TakeError());
        }
        if (playing.GetValue())
        {
            return AZ::Failure(AZStd::string("Stop play before editing the document"));
        }

        auto propagating = PropagationPending(allocator);
        if (!propagating)
        {
            return AZ::Failure(propagating.TakeError());
        }
        if (propagating.GetValue())
        {
            return AZ::Failure(AZStd::string("Prefab propagation is still pending"));
        }

        auto levelReady = m_bindings.Call(LevelReadyBinding, allocator);
        if (!levelReady)
        {
            return AZ::Failure(levelReady.TakeError());
        }
        auto path = m_bindings.Call(LevelPathBinding, allocator);
        if (!path)
        {
            return AZ::Failure(path.TakeError());
        }

        const bool ready = levelReady.GetValue().GetBool();
        if (!ready && path.GetValue().GetStringLength())
        {
            return AZ::Failure(AZStd::string("The editor document is loading"));
        }
        if (requirement == Requirement::ReadyLevel && !ready)
        {
            return AZ::Failure(AZStd::string("Load or create a ready level before this operation"));
        }
        return AZ::Success();
    }

    BridgeResult<void> EditorTools::RequireIdle(Requirement requirement, Allocator& allocator)
    {
        if (m_active || m_running)
        {
            return AZ::Failure(
                AZStd::string("An editor operation is active; wait for its response before editing or reading document values"));
        }
        return Require(requirement, allocator);
    }

    bool EditorTools::Deferred(Requirement requirement, ToolCall& call)
    {
        if (&call != m_running)
        {
            auto idle = RequireIdle(requirement, call.m_allocator);
            if (!idle)
            {
                return Fail(call, idle.GetError());
            }
            call.m_pending = true;
            return true;
        }

        auto met = Require(requirement, call.m_allocator);
        if (!met)
        {
            return Fail(call, met.GetError());
        }
        return false;
    }

    bool EditorTools::Fail(ToolCall& call, AZStd::string_view error) const
    {
        call.m_data.AddMember("error", String(error, call.m_allocator), call.m_allocator);
        if (&call == m_running)
        {
            call.m_data.AddMember("partialChangesPossible", true, call.m_allocator);
        }
        return true;
    }

    bool EditorTools::Reply(BridgeResult<Json> result, ToolCall& call) const
    {
        if (!result)
        {
            return Fail(call, result.GetError());
        }
        call.m_data = result.GetValue();
        return true;
    }

    void EditorTools::BeginPending(AZStd::string_view name, const Json& args)
    {
        m_tool = name;
        m_active = true;
        m_finished = false;
        m_args.CopyFrom(args, m_args.GetAllocator());
        m_result.SetObject();
        m_until = AZStd::chrono::steady_clock::now() + AZStd::chrono::milliseconds(m_connectionTimeoutMs - ReplyMarginMs);

        // asset_import already started; PollPending() polls it.
        if (name != AssetImportTool)
        {
            m_timer.start(0);
        }
    }

    void EditorTools::RunPending()
    {
        if (!m_active || m_running)
        {
            return;
        }

        // Native level calls may pump events; a cancellation cannot invalidate the arguments on this stack.
        const AZStd::string tool = m_tool;
        rapidjson::Document args;
        args.CopyFrom(m_args, args.GetAllocator());
        rapidjson::Document result(rapidjson::kObjectType);
        Allocator& allocator = result.GetAllocator();

        ToolCall call{ result, allocator };
        m_running = &call;
        if (Dispatch(tool, args, call) != ToolProvider::Result::Complete)
        {
            Fail(call, call.m_error);
        }
        m_running = nullptr;

        if (m_active)
        {
            m_result.CopyFrom(result, m_result.GetAllocator());
            m_finished = true;
        }
    }

    void EditorTools::ResetPending()
    {
        m_timer.stop();
        m_import.Cancel();
        m_screenshot.Cancel();
        m_active = false;
        m_finished = false;
        m_tool.clear();
        m_args.SetObject();
        m_args.GetAllocator().Clear();
        m_result.SetObject();
        m_result.GetAllocator().Clear();
    }

    BridgeResult<Json> EditorTools::State(Allocator& allocator)
    {
        Json state(rapidjson::kObjectType);
        auto level = m_bindings.Call(LevelPathBinding, allocator);
        if (!level)
        {
            return level;
        }
        state.AddMember("level", level.GetValue(), allocator);

        auto levelReady = m_bindings.Call(LevelReadyBinding, allocator);
        if (!levelReady)
        {
            return levelReady;
        }
        const bool ready = levelReady.GetValue().GetBool();
        const bool loading = !ready && state["level"].GetStringLength() != 0;
        state.AddMember("ready", ready, allocator);
        state.AddMember("loading", loading, allocator);

        auto dirty = m_bindings.Call(LevelModifiedBinding, allocator);
        if (!dirty)
        {
            return dirty;
        }
        state.AddMember("dirty", dirty.GetValue(), allocator);

        auto playing = Playing(allocator);
        if (!playing)
        {
            return AZ::Failure(playing.TakeError());
        }
        state.AddMember("playing", playing.GetValue(), allocator);
        state.AddMember("busy", m_active || m_running, allocator);
        state.AddMember("activeTool", String(m_tool, allocator), allocator);

        auto focus = Focus(allocator);
        if (!focus)
        {
            return focus;
        }
        state.AddMember("prefabFocus", focus.GetValue(), allocator);

        auto dirtyPrefabPaths = m_bindings.Call(DirtyPrefabPathsBinding, allocator);
        if (!dirtyPrefabPaths)
        {
            return dirtyPrefabPaths;
        }
        state.AddMember("dirtyPrefabPaths", dirtyPrefabPaths.GetValue(), allocator);

        auto propagating = PropagationPending(allocator);
        if (!propagating)
        {
            return AZ::Failure(propagating.TakeError());
        }
        state.AddMember("propagationPending", propagating.GetValue(), allocator);
        return AZ::Success(AZStd::move(state));
    }

    BridgeResult<Json> EditorTools::Focus(Allocator& allocator)
    {
        auto context = m_bindings.Call("bus/EditorEntityContextRequestBus/event/GetEditorEntityContextId", allocator);
        if (!context)
        {
            return context;
        }
        return m_bindings.Call(
            "bus/PrefabFocusPublicRequestBus/event/GetFocusedPrefabContainerEntityId", allocator, { &context.GetValue() });
    }

    BridgeResult<bool> EditorTools::Playing(Allocator& allocator)
    {
        auto playing = m_bindings.Call(InGameModeBinding, allocator);
        if (!playing)
        {
            return AZ::Failure(playing.TakeError());
        }
        return AZ::Success(playing.GetValue().GetBool());
    }

    BridgeResult<bool> EditorTools::PropagationPending(Allocator& allocator)
    {
        if (m_propagating)
        {
            return AZ::Success(true);
        }

        auto pending = m_bindings.Call("bus/PrefabPublicRequestBus/event/IsPropagationPending", allocator);
        if (!pending)
        {
            return AZ::Failure(pending.TakeError());
        }
        return AZ::Success(pending.GetValue().GetBool());
    }

    BridgeResult<AZStd::string_view> EditorTools::ReleaseCurrentLevel(const AZStd::optional<AZStd::string>& unsaved, Allocator& allocator)
    {
        const AZStd::string_view choice = unsaved ? AZStd::string_view(*unsaved) : UnsavedReject;
        auto modified = m_bindings.Call(LevelModifiedBinding, allocator);
        if (!modified)
        {
            return AZ::Failure(modified.TakeError());
        }
        if (modified.GetValue().GetBool())
        {
            if (choice == UnsavedReject)
            {
                return AZ::Failure(AZStd::string("Current level is dirty; choose unsaved=save or discard explicitly"));
            }
            if (choice == UnsavedSave)
            {
                auto saved = m_bindings.Call(SaveLevelBinding, allocator);
                if (!saved)
                {
                    return AZ::Failure(saved.TakeError());
                }
                if (!saved.GetValue().GetBool())
                {
                    return AZ::Failure(AZStd::string("Could not save the current level; replacement refused"));
                }
            }
        }

        auto dirty = m_bindings.Call(DirtyPrefabPathsBinding, allocator);
        if (!dirty)
        {
            return AZ::Failure(dirty.TakeError());
        }
        if (choice != UnsavedDiscard && !dirty.GetValue().Empty())
        {
            return AZ::Failure(
                AZStd::string("Dirty prefab templates remain; explicitly prefab_save each requested template or choose unsaved=discard"));
        }
        return AZ::Success(choice);
    }

    bool EditorTools::HandleEditorInfo([[maybe_unused]] const EditorInfoArgs& args, ToolCall& call)
    {
        const QByteArray platform = QGuiApplication::platformName().toUtf8();
        call.m_data.AddMember("kind", "editor", call.m_allocator);
        call.m_data.AddMember("ready", true, call.m_allocator);
        call.m_data.AddMember(
            "qtPlatform", String({ platform.constData(), aznumeric_cast<size_t>(platform.size()) }, call.m_allocator), call.m_allocator);
        return true;
    }

    bool EditorTools::HandleEntityCreate(const EntityCreateArgs& args, ToolCall& call)
    {
        if (Deferred(Requirement::ReadyLevel, call))
        {
            return true;
        }

        AzToolsFramework::ScopedUndoBatch undo(UndoName(EntityCreateTool).c_str());
        Allocator& allocator = call.m_allocator;
        auto parent = args.m_parent ? BridgeResult<Json>(Json(*args.m_parent, allocator)) : Focus(allocator);
        if (!parent)
        {
            return Fail(call, parent.GetError());
        }
        rapidjson::Document origin;
        origin.Parse(OriginPosition);
        const Json& position = args.m_position ? *args.m_position : origin;

        auto entity = m_bindings.Call("bus/PrefabPublicRequestBus/event/CreateEntity", allocator, { &parent.GetValue(), &position });
        if (!entity)
        {
            return Fail(call, entity.GetError());
        }
        call.m_data.AddMember("entity", Json(entity.GetValue(), allocator), allocator);
        if (args.m_name)
        {
            const Json name = String(*args.m_name, allocator);
            auto named = m_bindings.Call("bus/EditorEntityAPIBus/event/SetName", allocator, { &name }, &entity.GetValue());
            if (!named)
            {
                return Fail(call, named.GetError());
            }
        }
        return true;
    }

    bool EditorTools::HandleEntityDelete(const EntityDeleteArgs& args, ToolCall& call)
    {
        if (Deferred(Requirement::ReadyLevel, call))
        {
            return true;
        }

        AzToolsFramework::ScopedUndoBatch undo(UndoName(EntityDeleteTool).c_str());
        auto deleted = m_bindings.Call(
            "bus/PrefabPublicRequestBus/event/DeleteEntitiesAndAllDescendantsInInstance", call.m_allocator, { args.m_entities });
        if (!deleted)
        {
            return Fail(call, deleted.GetError());
        }
        return true;
    }

    bool EditorTools::HandleEntitySetParent(const EntitySetParentArgs& args, ToolCall& call)
    {
        if (Deferred(Requirement::ReadyLevel, call))
        {
            return true;
        }

        AzToolsFramework::ScopedUndoBatch undo(UndoName(EntitySetParentTool).c_str());
        Allocator& allocator = call.m_allocator;
        auto entity = m_bindings.Normalize(*args.m_entity, azrtti_typeid<AZ::EntityId>(), allocator);
        if (!entity)
        {
            return Fail(call, entity.GetError());
        }
        auto parent = m_bindings.Normalize(*args.m_parent, azrtti_typeid<AZ::EntityId>(), allocator);
        if (!parent)
        {
            return Fail(call, parent.GetError());
        }
        auto descendants = m_bindings.Call("bus/TransformBus/event/GetAllDescendants", allocator, {}, &entity.GetValue());
        if (!descendants)
        {
            return Fail(call, descendants.GetError());
        }

        const Json& parentId = parent.GetValue()["value"];
        if (entity.GetValue()["value"] == parentId)
        {
            return Fail(call, "An entity cannot parent itself");
        }
        for (const Json& child : descendants.GetValue().GetArray())
        {
            if (child["value"] == parentId)
            {
                return Fail(call, "Parenting would create a hierarchy cycle");
            }
        }

        auto set = m_bindings.Call("bus/EditorEntityAPIBus/event/SetParent", allocator, { &parent.GetValue() }, &entity.GetValue());
        if (!set)
        {
            return Fail(call, set.GetError());
        }
        auto actual = m_bindings.Call("bus/TransformBus/event/GetParentId", allocator, {}, &entity.GetValue());
        if (!actual)
        {
            return Fail(call, actual.GetError());
        }
        if (actual.GetValue()["value"] != parentId)
        {
            return Fail(call, "Native editor refused the requested parent");
        }
        call.m_data.AddMember("parent", actual.GetValue(), allocator);
        return true;
    }

    bool EditorTools::HandleTransformSet(const TransformSetArgs& args, ToolCall& call)
    {
        if (Deferred(Requirement::ReadyLevel, call))
        {
            return true;
        }

        AzToolsFramework::ScopedUndoBatch undo(UndoName(TransformSetTool).c_str());
        Allocator& allocator = call.m_allocator;
        const bool local = AZStd::string_view(args.m_space) == LocalSpace;
        auto transform = m_bindings.Normalize(*args.m_transform, azrtti_typeid<AZ::Transform>(), allocator);
        if (!transform)
        {
            return Fail(call, transform.GetError());
        }

        // Read first, to validate the editor entity before its native undo state is marked dirty.
        auto current = m_bindings.Call(local ? GetLocalTransformBinding : GetWorldTransformBinding, allocator, {}, args.m_entity);
        if (!current)
        {
            return Fail(call, current.GetError());
        }
        auto set = m_bindings.Call(
            local ? "bus/TransformBus/event/SetLocalTM" : "bus/TransformBus/event/SetWorldTM",
            allocator,
            { &transform.GetValue() },
            args.m_entity);
        if (!set)
        {
            return Fail(call, set.GetError());
        }
        auto dirty = m_bindings.Call("bus/ToolsApplicationRequestBus/event/AddDirtyEntity", allocator, { args.m_entity });
        if (!dirty)
        {
            return Fail(call, dirty.GetError());
        }
        return true;
    }

    bool EditorTools::HandleComponentAdd(const ComponentAddArgs& args, ToolCall& call)
    {
        if (Deferred(Requirement::ReadyLevel, call))
        {
            return true;
        }

        AzToolsFramework::ScopedUndoBatch undo(UndoName(ComponentAddTool).c_str());
        Allocator& allocator = call.m_allocator;
        const Json types = StringArray(args.m_types, allocator);
        auto components = m_bindings.Call("bus/EditorComponentAPIBus/event/AddComponentsOfType", allocator, { args.m_entity, &types });
        if (!components)
        {
            return Fail(call, components.GetError());
        }
        call.m_data.AddMember("components", components.GetValue(), allocator);
        return true;
    }

    bool EditorTools::HandleComponentRemove(const ComponentRemoveArgs& args, ToolCall& call)
    {
        if (Deferred(Requirement::ReadyLevel, call))
        {
            return true;
        }

        AzToolsFramework::ScopedUndoBatch undo(UndoName(ComponentRemoveTool).c_str());
        auto removed = m_bindings.Call("bus/EditorComponentAPIBus/event/RemoveComponents", call.m_allocator, { args.m_components });
        if (!removed)
        {
            return Fail(call, removed.GetError());
        }
        if (!removed.GetValue().GetBool())
        {
            return Fail(call, "Native composition rejected removal; inspect dependency/conflict diagnostics");
        }
        return true;
    }

    bool EditorTools::HandleComponentSet(const ComponentSetArgs& args, ToolCall& call)
    {
        if (Deferred(Requirement::ReadyLevel, call))
        {
            return true;
        }

        AzToolsFramework::ScopedUndoBatch undo(UndoName(ComponentSetTool).c_str());
        Allocator& allocator = call.m_allocator;
        const Json path = String(args.m_path, allocator);
        auto value =
            m_bindings.Call("bus/EditorComponentAPIBus/event/SetComponentProperty", allocator, { args.m_component, &path, args.m_value });
        if (!value)
        {
            return Fail(call, value.GetError());
        }
        call.m_data.AddMember("value", value.GetValue(), allocator);
        return true;
    }

    bool EditorTools::HandleLevelCreate(const LevelCreateArgs& args, ToolCall& call)
    {
        if (Deferred(Requirement::EditableDocument, call))
        {
            return true;
        }

        Allocator& allocator = call.m_allocator;
        auto released = ReleaseCurrentLevel(args.m_unsaved, allocator);
        if (!released)
        {
            return Fail(call, released.GetError());
        }
        const Json path = String(args.m_path, allocator);
        const Json source = String(args.m_template ? AZStd::string_view(*args.m_template) : AZStd::string_view{}, allocator);
        const Json discard(released.GetValue() == UnsavedDiscard);
        auto created = m_bindings.Call("global/method/create_level_at_path", allocator, { &path, &source, &discard });
        if (!created)
        {
            return Fail(call, created.GetError());
        }
        call.m_data.AddMember("path", created.GetValue(), allocator);
        return true;
    }

    bool EditorTools::HandleLevelOpen(const LevelOpenArgs& args, ToolCall& call)
    {
        if (Deferred(Requirement::EditableDocument, call))
        {
            return true;
        }

        // The unsaved choice is applied first, so the dialog-free open never prompts.
        Allocator& allocator = call.m_allocator;
        auto released = ReleaseCurrentLevel(args.m_unsaved, allocator);
        if (!released)
        {
            return Fail(call, released.GetError());
        }
        const Json path = String(args.m_path, allocator);
        auto opened = m_bindings.Call("global/method/open_level_no_prompt", allocator, { &path });
        if (!opened)
        {
            return Fail(call, opened.GetError());
        }
        if (!opened.GetValue().GetBool())
        {
            return Fail(call, "Native level open failed; inspect the editor log");
        }
        return true;
    }

    bool EditorTools::HandleLevelSave([[maybe_unused]] const LevelSaveArgs& args, ToolCall& call)
    {
        if (Deferred(Requirement::ReadyLevel, call))
        {
            return true;
        }

        auto saved = m_bindings.Call(SaveLevelBinding, call.m_allocator);
        if (!saved)
        {
            return Fail(call, saved.GetError());
        }
        if (!saved.GetValue().GetBool())
        {
            return Fail(call, "Native level save failed; inspect the editor log");
        }
        return true;
    }

    bool EditorTools::HandlePrefabCreate(const PrefabCreateArgs& args, ToolCall& call)
    {
        if (Deferred(Requirement::ReadyLevel, call))
        {
            return true;
        }

        AzToolsFramework::ScopedUndoBatch undo(UndoName(PrefabCreateTool).c_str());
        Allocator& allocator = call.m_allocator;
        const Json path = String(args.m_path, allocator);
        auto container = m_bindings.Call("bus/PrefabPublicRequestBus/event/CreatePrefabInMemory", allocator, { args.m_entities, &path });
        if (!container)
        {
            return Fail(call, container.GetError());
        }
        call.m_data.AddMember("container", container.GetValue(), allocator);
        return true;
    }

    bool EditorTools::HandlePrefabSave(const PrefabSaveArgs& args, ToolCall& call)
    {
        if (Deferred(Requirement::ReadyLevel, call))
        {
            return true;
        }

        const Json path = String(args.m_path, call.m_allocator);
        auto saved = m_bindings.Call("bus/PrefabPublicRequestBus/event/SavePrefab", call.m_allocator, { &path });
        if (!saved)
        {
            return Fail(call, saved.GetError());
        }
        return true;
    }

    bool EditorTools::HandleAssetImport(const AssetImportArgs& args, ToolCall& call)
    {
        auto idle = RequireIdle(Requirement::EditableDocument, call.m_allocator);
        if (!idle)
        {
            return Fail(call, idle.GetError());
        }

        const ToolProvider::Result result = m_import.Begin(args, call.m_data, call.m_allocator, call.m_error, m_connectionTimeoutMs);
        call.m_pending = result == ToolProvider::Result::Pending;
        return result != ToolProvider::Result::InvalidArguments;
    }

    bool EditorTools::HandleEditorPlay([[maybe_unused]] const EditorPlayArgs& args, ToolCall& call)
    {
        if (Deferred(Requirement::ReadyLevel, call))
        {
            return true;
        }

        auto entered = m_bindings.Call("bus/PythonEditorBus/event/EnterGameMode", call.m_allocator);
        if (!entered)
        {
            return Fail(call, entered.GetError());
        }
        return true;
    }

    bool EditorTools::HandleEditorStop([[maybe_unused]] const EditorStopArgs& args, ToolCall& call)
    {
        if (Deferred(Requirement::None, call))
        {
            return true;
        }

        auto exited = m_bindings.Call("bus/PythonEditorBus/event/ExitGameMode", call.m_allocator);
        if (!exited)
        {
            return Fail(call, exited.GetError());
        }
        return true;
    }

    bool EditorTools::HandleEditorState([[maybe_unused]] const EditorStateArgs& args, ToolCall& call)
    {
        return Reply(State(call.m_allocator), call);
    }

    bool EditorTools::HandleEditorUndo([[maybe_unused]] const EditorUndoArgs& args, ToolCall& call)
    {
        if (Deferred(Requirement::ReadyLevel, call))
        {
            return true;
        }

        // Undo and redo use the owning stack, never a new batch around the stack operation itself.
        auto undone = m_bindings.Call("bus/PythonEditorBus/event/Undo", call.m_allocator);
        if (!undone)
        {
            return Fail(call, undone.GetError());
        }
        return true;
    }

    bool EditorTools::HandleEditorRedo([[maybe_unused]] const EditorRedoArgs& args, ToolCall& call)
    {
        if (Deferred(Requirement::ReadyLevel, call))
        {
            return true;
        }

        auto redone = m_bindings.Call("bus/PythonEditorBus/event/Redo", call.m_allocator);
        if (!redone)
        {
            return Fail(call, redone.GetError());
        }
        return true;
    }

    bool EditorTools::HandlePythonExecute(const PythonExecuteArgs& args, ToolCall& call)
    {
        // The code must stop before the server answers the request on its own.
        const AZ::s64 maximumTimeoutMs = m_connectionTimeoutMs - ReplyMarginMs;
        const AZ::s64 timeoutMs = args.m_timeoutMs ? aznumeric_cast<AZ::s64>(*args.m_timeoutMs) : maximumTimeoutMs;
        if (timeoutMs > maximumTimeoutMs)
        {
            call.m_error = AZStd::string::format("timeout_ms: at most %lld, the connection deadline", maximumTimeoutMs);
            return false;
        }

        if (Deferred(Requirement::None, call))
        {
            return true;
        }

        auto ran = PythonExecute::Run(args.m_code, timeoutMs, call.m_data, call.m_allocator);
        if (!ran)
        {
            return Fail(call, ran.GetError());
        }
        return true;
    }

    bool EditorTools::HandleScreenshot(const ScreenshotArgs& args, ToolCall& call)
    {
        auto valid = Screenshot::Validate(args);
        if (!valid)
        {
            call.m_error = valid.TakeError();
            return false;
        }

        // An unknown panel is refused before scheduling, with the names an agent can use.
        if (args.m_panel && &call != m_running)
        {
            auto idle = RequireIdle(Requirement::None, call.m_allocator);
            if (!idle)
            {
                return Fail(call, idle.GetError());
            }
            auto panes = m_bindings.Call(PaneNamesBinding, call.m_allocator);
            if (!panes)
            {
                return Fail(call, panes.GetError());
            }
            const Json name = String(*args.m_panel, call.m_allocator);
            const Json& names = panes.GetValue();
            if (!names.IsArray() || AZStd::find(names.Begin(), names.End(), name) == names.End())
            {
                call.m_data.AddMember("panels", Json(names, call.m_allocator), call.m_allocator);
                return Fail(call, "Unknown panel; data.panels lists the view pane names");
            }
        }

        if (Deferred(Requirement::None, call))
        {
            return true;
        }

        auto begun = m_screenshot.Begin(args);
        if (!begun)
        {
            return Fail(call, begun.GetError());
        }
        return true;
    }

    void EditorTools::OnPrefabInstancePropagationBegin()
    {
        m_propagating = true;
    }

    void EditorTools::OnPrefabInstancePropagationEnd()
    {
        m_propagating = false;
    }
} // namespace LocalMcp
