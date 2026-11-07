/* Copyright (c) Contributors to the Open 3D Engine Project.
 * SPDX-License-Identifier: Apache-2.0 OR MIT */
#pragma once
#include "LocalMcpServer.h"
#include "BehaviorBridge.h"
#include "EditorTools.h"
#include <AzCore/Component/Component.h>
#include <AzCore/Component/TickBus.h>
#include <AzCore/Component/ComponentApplicationBus.h>
#include <AzToolsFramework/API/ToolsApplicationAPI.h>

namespace LocalMcp
{
    inline constexpr const char* EditorSettingsRoot = "/O3DE/Gems/LocalMcp/Editor";

    class EditorSystemComponent final : public AZ::Component,
        private AZ::SystemTickBus::Handler, private AzToolsFramework::EditorEvents::Bus::Handler,
        private ToolProvider
    {
    public:
        AZ_COMPONENT(EditorSystemComponent, "{9BB863EE-F886-4EE6-A431-329B1D7A0463}");
        static void Reflect(AZ::ReflectContext* context);
        static void GetRequiredServices(AZ::ComponentDescriptor::DependencyArrayType& services);
        static bool SupportsApplication(const AZ::ApplicationTypeQuery& type);
        void Activate() override;
        void Deactivate() override;
    private:
        bool SkipEditorStartupUI() override { return true; }
        void NotifyEditorInitialized() override;
        void OnSystemTick() override;
        const char* GetServerName() const override { return "o3de-local-editor"; }
        void AddTools(rapidjson::Value& tools, rapidjson::Document::AllocatorType& allocator) const override;
        Result Invoke(AZStd::string_view name, const rapidjson::Value& args, rapidjson::Value& data,
            rapidjson::Document::AllocatorType& allocator, AZStd::string& detail) override;
        Result PollPending(rapidjson::Value& data, rapidjson::Document::AllocatorType& allocator, AZStd::string& detail) override
        { return m_tools.PollPending(data, allocator, detail); }
        void CancelPending() override { m_tools.CancelPending(); }
        BehaviorBridge m_bridge;
        EditorTools m_tools{m_bridge};
        Config m_config;
        Server m_server{this};
    };
}
