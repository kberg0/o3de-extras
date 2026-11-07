/* Copyright (c) Contributors to the Open 3D Engine Project.
 * SPDX-License-Identifier: Apache-2.0 OR MIT */
#include "ScriptCanvasTools.h"

namespace LocalMcp
{
    void ScriptCanvasTools::Invoke(AZStd::string_view name, const Json& args, Json& data, Allocator& allocator)
    {
        constexpr AZStd::string_view bus = "bus/GraphAutomationRequestBus/event/";
        auto call = [&](AZStd::string_view method, AZStd::initializer_list<const Json*> values)
        {
            data = m_bindings.Call(AZStd::string(bus) + AZStd::string(method), allocator, values);
            if (!data.IsObject()) { throw BridgeError("ScriptCanvas owner returned a non-object result"); }
        };
        if (name == "sc_create" || name == "sc_open")
        { call(name == "sc_create" ? "Create" : "Open", {&args["path"]}); }
        else if (name == "sc_describe" || name == "sc_undo" || name == "sc_redo")
        { call(name == "sc_describe" ? "Describe" : name == "sc_undo" ? "Undo" : "Redo", {&args["graph"]}); }
        else if (name == "sc_reload")
        {
            const Json discard(false);
            call("Reload", {&args["graph"], args.HasMember("discard") ? &args["discard"] : &discard});
        }
        else if (name == "sc_palette")
        {
            const Json query = String("", allocator), after(0), limit(64);
            call("Palette", {args.HasMember("query") ? &args["query"] : &query,
                args.HasMember("after") ? &args["after"] : &after, args.HasMember("limit") ? &args["limit"] : &limit});
        }
        else if (name == "sc_add_node")
        { call("AddNode", {&args["graph"], &args["palette"], &args["position"]}); }
        else if (name == "sc_connect")
        { call("Connect", {&args["graph"], &args["source_node"], &args["source_slot"], &args["target_node"], &args["target_slot"]}); }
        else if (name == "sc_set_value")
        { call("SetValue", {&args["graph"], &args["node"], &args["slot"], &args["value"]}); }
        else if (name == "sc_set_variable")
        { call("SetVariable", {&args["graph"], &args["variable"], &args["value"]}); }
        else if (name == "sc_remove_nodes")
        { call("RemoveNodes", {&args["graph"], &args["nodes"]}); }
        else if (name == "sc_remove_connections")
        { call("RemoveConnections", {&args["graph"], &args["connections"]}); }
        else if (name == "sc_attach")
        { call("Attach", {&args["component"], &args["path"]}); }
        else if (name == "sc_attachment")
        { call("Attachment", {&args["component"]}); }
        else if (name == "sc_save")
        {
            call("Save", {&args["graph"], &args["path"]});
            m_saving = true;
        }
        else { throw BridgeError("Unknown ScriptCanvas recipe"); }
    }

    bool ScriptCanvasTools::PollSave(Json& data, Allocator& allocator)
    {
        if (!m_saving) { throw BridgeError("No active ScriptCanvas save"); }
        data = m_bindings.Call("bus/GraphAutomationRequestBus/event/PollSave", allocator);
        if (!data.IsObject() || !data.HasMember("pending") || !data["pending"].IsBool())
        { throw BridgeError("ScriptCanvas save returned an invalid completion"); }
        if (data["pending"].GetBool()) { return false; }
        m_saving = false;
        return true;
    }

    void ScriptCanvasTools::CancelSave()
    {
        if (!m_saving) { return; }
        m_saving = false;
        try
        {
            rapidjson::Document temporary;
            m_bindings.Call("bus/GraphAutomationRequestBus/event/CancelSave", temporary.GetAllocator());
        }
        catch (const std::exception& error)
        { AZ_Warning("LocalMcp", false, "Could not cancel the graph save observer: %s", error.what()); }
    }
}
