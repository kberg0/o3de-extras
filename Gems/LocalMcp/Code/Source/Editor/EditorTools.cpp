/* Copyright (c) Contributors to the Open 3D Engine Project.
 * SPDX-License-Identifier: Apache-2.0 OR MIT */
#include "EditorTools.h"
#include "EditorToolSchemas.h"
#include <AzCore/JSON/schema.h>
#include <AzToolsFramework/API/ToolsApplicationAPI.h>
#include <AzToolsFramework/Component/EditorComponentAPIBus.h>
#include <AzCore/Math/Transform.h>

namespace LocalMcp
{
    EditorTools::EditorTools(BehaviorBridge& bridge) : m_bindings(bridge), m_scriptCanvas(bridge)
    {
        m_timer.setSingleShot(true);
        QObject::connect(&m_timer, &QTimer::timeout, [this] { RunPending(); });
    }

    void EditorTools::Activate(AZ::s64 connectionTimeoutMs)
    {
        m_connectionTimeoutMs = connectionTimeoutMs;
        AzToolsFramework::Prefab::PrefabPublicNotificationBus::Handler::BusConnect();
        m_import.Activate();
    }

    void EditorTools::Deactivate()
    {
        CancelPending();
        m_import.Deactivate();
        AzToolsFramework::Prefab::PrefabPublicNotificationBus::Handler::BusDisconnect();
    }

    void EditorTools::AddTools(Json& tools, Allocator& allocator)
    {
        rapidjson::Document schemas;
        schemas.Parse(EditorToolSchemas);
        for (const auto& schema : schemas.GetArray()) { tools.PushBack(Json(schema, allocator), allocator); }
    }

    bool EditorTools::Validate(AZStd::string_view name, const Json& args)
    {
        rapidjson::Document schemas;
        schemas.Parse(EditorToolSchemas);
        for (const auto& schema : schemas.GetArray())
        {
            if (name != schema["name"].GetString()) { continue; }
            rapidjson::SchemaDocument contract(schema["inputSchema"]);
            rapidjson::SchemaValidator validator(contract);
            if (!args.Accept(validator))
            {
                rapidjson::StringBuffer pointer;
                validator.GetInvalidDocumentPointer().StringifyUriFragment(pointer);
                throw BridgeError(AZStd::string::format("Invalid arguments at %s (%s)", pointer.GetString(),
                    validator.GetInvalidSchemaKeyword()).c_str());
            }
            // Reject embedded NULs before a native path/name accepts a C string.
            for (auto member = args.MemberBegin(); member != args.MemberEnd(); ++member)
            {
                if (member->value.IsString() && !IsString(args, member->name.GetString(), 4096))
                { throw BridgeError("String arguments cannot contain embedded NULs"); }
            }
            return true;
        }
        return false;
    }

    bool EditorTools::Playing(Allocator& allocator)
    {
        return m_bindings.Call("bus/PythonEditorBus/event/IsInGameMode", allocator).GetBool();
    }

    bool EditorTools::PropagationPending(Allocator& allocator)
    {
        return m_propagating || m_bindings.Call("bus/PrefabPublicRequestBus/event/IsPropagationPending", allocator).GetBool();
    }

    Json EditorTools::Focus(Allocator& allocator)
    {
        Json context = m_bindings.Call("bus/EditorEntityContextRequestBus/event/GetEditorEntityContextId", allocator);
        return m_bindings.Call("bus/PrefabFocusPublicRequestBus/event/GetFocusedPrefabContainerEntityId", allocator, {&context});
    }

    Json EditorTools::State(Allocator& allocator)
    {
        Json state(rapidjson::kObjectType);
        state.AddMember("level", m_bindings.Call("global/method/get_level_document_path", allocator), allocator);
        const bool ready = m_bindings.Call("global/method/is_level_document_ready", allocator).GetBool();
        state.AddMember("ready", ready, allocator);
        state.AddMember("loading", !ready && state["level"].GetStringLength() != 0, allocator);
        state.AddMember("dirty", m_bindings.Call("global/method/is_level_document_modified", allocator), allocator);
        state.AddMember("playing", Playing(allocator), allocator);
        state.AddMember("busy", m_active || m_running, allocator);
        state.AddMember("activeTool", String(m_tool, allocator), allocator);
        state.AddMember("prefabFocus", Focus(allocator), allocator);
        state.AddMember("dirtyPrefabPaths", m_bindings.Call("bus/PrefabPublicRequestBus/event/GetDirtyPrefabPaths", allocator), allocator);
        state.AddMember("propagationPending", PropagationPending(allocator), allocator);
        return state;
    }

    void EditorTools::RequireEditable(Allocator& allocator, bool requireLevel)
    {
        if (Playing(allocator)) { throw BridgeError("Stop play before editing the document"); }
        if (PropagationPending(allocator)) { throw BridgeError("Prefab propagation is still pending"); }
        const bool ready = m_bindings.Call("global/method/is_level_document_ready", allocator).GetBool();
        const auto path = m_bindings.Call("global/method/get_level_document_path", allocator);
        if (!ready && path.GetStringLength()) { throw BridgeError("The editor document is loading"); }
        if (requireLevel && !ready)
        { throw BridgeError("Load or create a ready level before this operation"); }
    }

    ToolProvider::Result EditorTools::Invoke(AZStd::string_view name, const Json& args, Json& data,
        Allocator& allocator, AZStd::string& detail)
    {
        try
        {
            if (!Validate(name, args)) { return ToolProvider::Result::Unknown; }
        }
        catch (const std::exception& error)
        {
            detail = error.what();
            return ToolProvider::Result::InvalidArguments;
        }
        try
        {
            if (name == "editor_state")
            {
                data = State(allocator);
                return ToolProvider::Result::Complete;
            }
            if (m_active || m_running) { throw BridgeError("An editor operation is active; wait for its response before editing or reading document values"); }
            const bool read = name == "transform_get" || name == "component_types" || name == "component_list" ||
                name == "component_properties" || name == "component_get";
            if (name != "editor_stop")
            {
                RequireEditable(allocator, name != "level_create" && name != "level_open" && name != "asset_import" &&
                    name != "component_types" && !name.starts_with("sc_"));
            }
            if (read)
            {
                Read(name, args, data, allocator);
                return ToolProvider::Result::Complete;
            }
            if (name == "asset_import")
            {
                const auto result = m_import.Begin(args, data, allocator, detail, m_connectionTimeoutMs);
                if (result != ToolProvider::Result::Pending) { return result; }
            }
            m_tool = name;
            m_active = true;
            m_finished = false;
            m_args.CopyFrom(args, m_args.GetAllocator());
            m_result.SetObject();
            m_until = AZStd::chrono::steady_clock::now() + AZStd::chrono::milliseconds(m_connectionTimeoutMs - 250);
            if (name != "asset_import") { m_timer.start(0); }
            return ToolProvider::Result::Pending;
        }
        catch (const std::exception& error)
        {
            data.AddMember("error", String(error.what(), allocator), allocator);
            return ToolProvider::Result::Complete;
        }
    }

    void EditorTools::CancelPending()
    {
        if (m_active && (m_running || m_finished || m_import.IsActive()))
        {
            AZ_Warning("LocalMcp", false, "Canceled active %s; native changes and copied source files are retained. Inspect editor_state and logs.", m_tool.c_str());
        }
        ResetPending();
    }

    void EditorTools::ResetPending()
    {
        m_timer.stop();
        m_import.Cancel();
        m_scriptCanvas.CancelSave();
        m_active = false;
        m_finished = false;
        m_tool.clear();
        m_args.SetObject();
        m_args.GetAllocator().Clear();
        m_result.SetObject();
        m_result.GetAllocator().Clear();
    }

    void EditorTools::RunPending()
    {
        if (!m_active || m_running) { return; }
        // Native level calls may pump events; cancellation cannot invalidate the arguments on this stack.
        const AZStd::string tool = m_tool;
        rapidjson::Document args;
        args.CopyFrom(m_args, args.GetAllocator());
        rapidjson::Document result(rapidjson::kObjectType);
        auto& allocator = result.GetAllocator();
        m_running = true;
        try
        {
            if (tool == "editor_stop")
            {
                m_bindings.Call("bus/PythonEditorBus/event/ExitGameMode", allocator);
            }
            else
            {
                RequireEditable(allocator, tool != "level_create" && tool != "level_open" && !tool.starts_with("sc_"));
                if (tool.starts_with("sc_"))
                { m_scriptCanvas.Invoke(tool, args, result, allocator); }
                else if (tool == "editor_undo" || tool == "editor_redo")
                {
                    // Undo/redo use the owning stack, never a new batch around the stack operation itself.
                    m_bindings.Call(tool == "editor_undo" ? "bus/PythonEditorBus/event/Undo" :
                        "bus/PythonEditorBus/event/Redo", allocator);
                }
                else if (tool == "level_create" || tool == "level_open" || tool == "level_save" || tool == "prefab_save" || tool == "editor_play")
                {
                    Document(tool, args, result, allocator);
                }
                else
                {
                    AzToolsFramework::ScopedUndoBatch undo(AZStd::string::format("LocalMcp %s", tool.c_str()).c_str());
                    Edit(tool, args, result, allocator);
                }
            }
        }
        catch (const std::exception& error)
        {
            result.AddMember("error", String(error.what(), allocator), allocator);
            result.AddMember("partialChangesPossible", true, allocator);
        }
        m_running = false;
        if (m_active)
        {
            m_result.CopyFrom(result, m_result.GetAllocator());
            m_finished = true;
        }
    }

    ToolProvider::Result EditorTools::PollPending(Json& data, Allocator& allocator, AZStd::string& detail)
    {
        if (!m_active)
        {
            detail = "No active editor operation";
            return ToolProvider::Result::InvalidArguments;
        }
        if (m_tool == "asset_import")
        {
            const auto result = m_import.Poll(data, allocator);
            if (result != ToolProvider::Result::Pending) { ResetPending(); }
            return result;
        }
        if (AZStd::chrono::steady_clock::now() >= m_until)
        {
            data.AddMember("error", "Editor operation timed out; inspect editor_state and logs before any further edit", allocator);
            data.AddMember("partialChangesPossible", m_running || m_finished, allocator);
            ResetPending();
            return ToolProvider::Result::Complete;
        }
        if (!m_finished || m_running) { return ToolProvider::Result::Pending; }
        try
        {
            if (!m_result.HasMember("error"))
            {
                if (m_tool == "sc_save")
                {
                    if (!m_import.IsActive())
                    {
                        if (!m_scriptCanvas.PollSave(m_result, m_result.GetAllocator()))
                        { return ToolProvider::Result::Pending; }
                        const auto remaining = AZStd::chrono::duration_cast<AZStd::chrono::milliseconds>(
                            m_until - AZStd::chrono::steady_clock::now()).count();
                        if (m_import.Observe(m_args["path"].GetString(), remaining, m_result, m_result.GetAllocator()) ==
                            ToolProvider::Result::Pending)
                        { return ToolProvider::Result::Pending; }
                    }
                    else
                    {
                        Json processing(rapidjson::kObjectType);
                        if (m_import.Poll(processing, m_result.GetAllocator()) == ToolProvider::Result::Pending)
                        { return ToolProvider::Result::Pending; }
                        if (processing.HasMember("error"))
                        { m_result.AddMember("error", Json(processing["error"], m_result.GetAllocator()), m_result.GetAllocator()); }
                        m_result.AddMember("assetProcessing", processing, m_result.GetAllocator());
                    }
                }
                // Native prefab propagation is paused during play.
                if (m_tool != "editor_play" && PropagationPending(allocator)) { return ToolProvider::Result::Pending; }
                if (m_tool == "editor_play" && !Playing(allocator)) { return ToolProvider::Result::Pending; }
                if (m_tool == "editor_stop" && Playing(allocator)) { return ToolProvider::Result::Pending; }
                if ((m_tool == "level_create" || m_tool == "level_open") &&
                    !m_bindings.Call("global/method/is_level_document_ready", allocator).GetBool())
                { return ToolProvider::Result::Pending; }
            }
            data.CopyFrom(m_result, allocator);
            ResetPending();
            data.AddMember("state", State(allocator), allocator);
        }
        catch (const std::exception& error)
        {
            data.AddMember(String(data.HasMember("error") ? "stateError" : "error", allocator), String(error.what(), allocator), allocator);
            if (!data.HasMember("partialChangesPossible")) { data.AddMember("partialChangesPossible", true, allocator); }
        }
        ResetPending();
        return ToolProvider::Result::Complete;
    }

    void EditorTools::Document(AZStd::string_view name, const Json& args, Json& data, Allocator& allocator)
    {
        if (name == "level_create" || name == "level_open")
        {
            const AZStd::string_view unsaved = args.HasMember("unsaved") ? args["unsaved"].GetString() : "reject";
            if (m_bindings.Call("global/method/is_level_document_modified", allocator).GetBool())
            {
                if (unsaved == "reject") { throw BridgeError("Current level is dirty; choose unsaved=save or discard explicitly"); }
                if (unsaved == "save" && !m_bindings.Call("global/method/save_level", allocator).GetBool())
                { throw BridgeError("Could not save the current level; replacement refused"); }
            }
            const Json dirty = m_bindings.Call("bus/PrefabPublicRequestBus/event/GetDirtyPrefabPaths", allocator);
            if (unsaved != "discard" && !dirty.Empty())
            {
                throw BridgeError("Dirty prefab templates remain; explicitly prefab_save each requested template or choose unsaved=discard");
            }
            if (name == "level_create")
            {
                Json empty = String("", allocator);
                Json discard(unsaved == "discard");
                const Json& source = args.HasMember("template") ? args["template"] : empty;
                data.AddMember("path", m_bindings.Call("global/method/create_level_at_path", allocator, {&args["path"], &source, &discard}), allocator);
            }
            else
            {
                // The dirty-document decision is complete above; always use the existing dialog-free owner path.
                const auto opened = m_bindings.Call("global/method/open_level_no_prompt", allocator, {&args["path"]});
                if (!opened.GetBool()) { throw BridgeError("Native level open failed; inspect the editor log"); }
            }
        }
        else if (name == "level_save")
        {
            if (!m_bindings.Call("global/method/save_level", allocator).GetBool())
            { throw BridgeError("Native level save failed; inspect the editor log"); }
        }
        else if (name == "prefab_save")
        { m_bindings.Call("bus/PrefabPublicRequestBus/event/SavePrefab", allocator, {&args["path"]}); }
        else if (name == "editor_play")
        { m_bindings.Call("bus/PythonEditorBus/event/EnterGameMode", allocator); }
    }

    void EditorTools::Read(AZStd::string_view name, const Json& args, Json& data, Allocator& allocator)
    {
        if (name == "transform_get")
        {
            const bool local = AZStd::string_view(args["space"].GetString()) == "local";
            data.AddMember("transform", m_bindings.Call(local ? "bus/TransformBus/event/GetLocalTM" :
                "bus/TransformBus/event/GetWorldTM", allocator, {}, &args["entity"]), allocator);
        }
        else if (name == "component_types")
        {
            const Json entityType(static_cast<int>(AzToolsFramework::EditorComponentAPIRequests::EntityType::Game));
            Json names = args.HasMember("names") ? Json(args["names"], allocator) :
                m_bindings.Call("bus/EditorComponentAPIBus/event/BuildComponentTypeNameListByEntityType", allocator, {&entityType});
            data.AddMember("types", m_bindings.Call("bus/EditorComponentAPIBus/event/FindComponentTypeIdsByEntityType",
                allocator, {&names, &entityType}), allocator);
            data.AddMember("names", names, allocator);
        }
        else if (name == "component_list")
        { data.AddMember("components", m_bindings.Call("bus/EditorComponentAPIBus/event/GetAllComponents", allocator, {&args["entity"]}), allocator); }
        else if (name == "component_get")
        { data.AddMember("value", m_bindings.Call("bus/EditorComponentAPIBus/event/GetComponentProperty", allocator,
            {&args["component"], &args["path"]}), allocator); }
        else if (name == "component_properties")
        {
            Json tree = m_bindings.Call("bus/EditorComponentAPIBus/event/BuildComponentPropertyTreeEditor", allocator, {&args["component"]});
            if (!tree.IsObject() || !IsString(tree, "$handle")) { throw BridgeError("Native property tree did not return an owning object handle"); }
            try
            {
                Json paths = m_bindings.Call("class/PropertyTreeEditor/method/BuildPathsList", allocator, {}, nullptr, &tree);
                Json properties(rapidjson::kArrayType);
                for (const auto& path : paths.GetArray())
                {
                    Json property(rapidjson::kObjectType);
                    property.AddMember("path", Json(path, allocator), allocator);
                    property.AddMember("type", m_bindings.Call("class/PropertyTreeEditor/method/GetPropertyType", allocator, {&path}, nullptr, &tree), allocator);
                    properties.PushBack(property, allocator);
                }
                data.AddMember("properties", properties, allocator);
            }
            catch (...)
            {
                m_bindings.Release(tree, allocator);
                throw;
            }
            m_bindings.Release(tree, allocator);
        }
    }

    void EditorTools::Edit(AZStd::string_view name, const Json& args, Json& data, Allocator& allocator)
    {
        if (name == "entity_create")
        {
            Json parent = args.HasMember("parent") ? Json(args["parent"], allocator) : Focus(allocator);
            rapidjson::Document origin;
            origin.Parse(R"({"$type":"Vector3","value":[0,0,0]})");
            const Json& position = args.HasMember("position") ? args["position"] : origin;
            Json entity = m_bindings.Call("bus/PrefabPublicRequestBus/event/CreateEntity", allocator, {&parent, &position});
            data.AddMember("entity", Json(entity, allocator), allocator);
            if (args.HasMember("name"))
            { m_bindings.Call("bus/EditorEntityAPIBus/event/SetName", allocator, {&args["name"]}, &entity); }
        }
        else if (name == "entity_delete")
        { m_bindings.Call("bus/PrefabPublicRequestBus/event/DeleteEntitiesAndAllDescendantsInInstance", allocator, {&args["entities"]}); }
        else if (name == "entity_set_parent")
        {
            Json entity = m_bindings.Normalize(args["entity"], azrtti_typeid<AZ::EntityId>(), allocator);
            Json parent = m_bindings.Normalize(args["parent"], azrtti_typeid<AZ::EntityId>(), allocator);
            Json descendants = m_bindings.Call("bus/TransformBus/event/GetAllDescendants", allocator, {}, &entity);
            if (entity["value"] == parent["value"]) { throw BridgeError("An entity cannot parent itself"); }
            for (const auto& child : descendants.GetArray())
            { if (child["value"] == parent["value"]) { throw BridgeError("Parenting would create a hierarchy cycle"); } }
            m_bindings.Call("bus/EditorEntityAPIBus/event/SetParent", allocator, {&parent}, &entity);
            Json actual = m_bindings.Call("bus/TransformBus/event/GetParentId", allocator, {}, &entity);
            if (actual["value"] != parent["value"]) { throw BridgeError("Native editor refused the requested parent"); }
            data.AddMember("parent", actual, allocator);
        }
        else if (name == "transform_set")
        {
            const bool local = AZStd::string_view(args["space"].GetString()) == "local";
            Json transform = m_bindings.Normalize(args["transform"], azrtti_typeid<AZ::Transform>(), allocator);
            // Read first to validate the editor entity/address before marking its native undo state dirty.
            m_bindings.Call(local ? "bus/TransformBus/event/GetLocalTM" : "bus/TransformBus/event/GetWorldTM", allocator, {}, &args["entity"]);
            m_bindings.Call(local ? "bus/TransformBus/event/SetLocalTM" : "bus/TransformBus/event/SetWorldTM", allocator,
                {&transform}, &args["entity"]);
            m_bindings.Call("bus/ToolsApplicationRequestBus/event/AddDirtyEntity", allocator, {&args["entity"]});
        }
        else if (name == "component_add")
        { data.AddMember("components", m_bindings.Call("bus/EditorComponentAPIBus/event/AddComponentsOfType", allocator,
            {&args["entity"], &args["types"]}), allocator); }
        else if (name == "component_remove")
        {
            if (!m_bindings.Call("bus/EditorComponentAPIBus/event/RemoveComponents", allocator, {&args["components"]}).GetBool())
            { throw BridgeError("Native composition rejected removal; inspect dependency/conflict diagnostics"); }
        }
        else if (name == "component_set")
        { data.AddMember("value", m_bindings.Call("bus/EditorComponentAPIBus/event/SetComponentProperty", allocator,
            {&args["component"], &args["path"], &args["value"]}), allocator); }
        else if (name == "prefab_create")
        { data.AddMember("container", m_bindings.Call("bus/PrefabPublicRequestBus/event/CreatePrefabInMemory", allocator,
            {&args["entities"], &args["path"]}), allocator); }
        else { throw BridgeError("Editor recipe is not implemented"); }
    }
}
