/* Copyright (c) Contributors to the Open 3D Engine Project.
 * SPDX-License-Identifier: Apache-2.0 OR MIT */
#pragma once
#include "AssetImport.h"
#include "BindingRecipe.h"
#include "ScriptCanvasTools.h"
#include <AzToolsFramework/Prefab/PrefabPublicNotificationBus.h>
#include <QTimer>

namespace LocalMcp
{
    class EditorTools final : private AzToolsFramework::Prefab::PrefabPublicNotificationBus::Handler
    {
    public:
        explicit EditorTools(BehaviorBridge& bridge);
        void Activate(AZ::s64 connectionTimeoutMs);
        void Deactivate();
        static void AddTools(Json& tools, Allocator& allocator);
        ToolProvider::Result Invoke(AZStd::string_view name, const Json& args, Json& data,
            Allocator& allocator, AZStd::string& detail);
        ToolProvider::Result PollPending(Json& data, Allocator& allocator, AZStd::string& detail);
        void CancelPending();
    private:
        void RunPending();
        void ResetPending();
        Json State(Allocator& allocator);
        Json Focus(Allocator& allocator);
        bool Playing(Allocator& allocator);
        bool PropagationPending(Allocator& allocator);
        void RequireEditable(Allocator& allocator, bool requireLevel);
        void Read(AZStd::string_view name, const Json& args, Json& data, Allocator& allocator);
        void Edit(AZStd::string_view name, const Json& args, Json& data, Allocator& allocator);
        void Document(AZStd::string_view name, const Json& args, Json& data, Allocator& allocator);
        static bool Validate(AZStd::string_view name, const Json& args);
        void OnPrefabInstancePropagationBegin() override { m_propagating = true; }
        void OnPrefabInstancePropagationEnd() override { m_propagating = false; }
        BindingRecipe m_bindings;
        ScriptCanvasTools m_scriptCanvas;
        AssetImport m_import;
        QTimer m_timer;
        AZ::s64 m_connectionTimeoutMs = 10000;
        bool m_active = false;
        bool m_running = false;
        bool m_finished = false;
        bool m_propagating = false;
        AZStd::string m_tool;
        rapidjson::Document m_args;
        rapidjson::Document m_result;
        AZStd::chrono::steady_clock::time_point m_until;
    };
}
