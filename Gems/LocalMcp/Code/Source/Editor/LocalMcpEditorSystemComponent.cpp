/* Copyright (c) Contributors to the Open 3D Engine Project.
 * SPDX-License-Identifier: Apache-2.0 OR MIT */
#include "LocalMcpEditorSystemComponent.h"
#include "LocalMcpJson.h"
#include <AzCore/Serialization/SerializeContext.h>
#include <QGuiApplication>
#include <EditorPythonBindings/EditorPythonBindingsSymbols.h>

namespace LocalMcp
{
    void EditorSystemComponent::Reflect(AZ::ReflectContext* context)
    {
        if (auto* serialize = azrtti_cast<AZ::SerializeContext*>(context))
        {
            serialize->Class<EditorSystemComponent, AZ::Component>()->Version(1);
        }
    }

    void EditorSystemComponent::GetRequiredServices(AZ::ComponentDescriptor::DependencyArrayType& services)
    {
        services.push_back(EditorPythonBindings::PythonEmbeddedService);
        services.push_back(EditorPythonBindings::PythonMarshalingService);
        services.push_back(EditorPythonBindings::PythonReflectionService);
    }

    bool EditorSystemComponent::SupportsApplication(const AZ::ApplicationTypeQuery& type)
    {
        return type.IsEditor() && !type.IsAssetProcessor();
    }

    void EditorSystemComponent::Activate()
    {
        AZ::ApplicationTypeQuery type;
        AZ::ComponentApplicationBus::Broadcast(&AZ::ComponentApplicationRequests::QueryApplicationType, type);
        if (!SupportsApplication(type))
        {
            return;
        }
        Config defaults;
        defaults.m_port = 4601;
        defaults.m_loopbackOnly = true;
        auto* registry = AZ::SettingsRegistry::Get();
        if (registry && ReadConfig(*registry, m_config, EditorSettingsRoot, defaults) && m_config.m_enabled)
        {
            m_bridge.Activate();
            m_tools.Activate(m_config.m_idleTimeoutMs);
            // The editor's existing startup hook suppresses both welcome dialogs and last-level autoload.
            AzToolsFramework::EditorEvents::Bus::Handler::BusConnect();
        }
    }

    void EditorSystemComponent::NotifyEditorInitialized()
    {
        if (m_server.Start(m_config))
        {
            AZ::SystemTickBus::Handler::BusConnect();
            AZ_TracePrintf("LocalMcp", "Editor listening on loopback port %u\n", m_server.GetPort());
        }
        else
        {
            AZ_Warning("LocalMcp", false, "Editor endpoint startup failed on port %lld.", m_config.m_port);
        }
    }

    void EditorSystemComponent::Deactivate()
    {
        AZ::SystemTickBus::Handler::BusDisconnect();
        AzToolsFramework::EditorEvents::Bus::Handler::BusDisconnect();
        m_server.Stop();
        m_tools.Deactivate();
        m_bridge.Deactivate();
    }

    void EditorSystemComponent::OnSystemTick()
    {
        m_server.Poll();
    }

    void EditorSystemComponent::AddTools(Json& tools, Allocator& allocator) const
    {
        rapidjson::Document schema;
        schema.Parse(R"({"name":"editor_info","description":"Editor endpoint readiness and process kind.",
            "inputSchema":{"type":"object","properties":{},"additionalProperties":false}})");
        Json entry;
        entry.CopyFrom(schema, allocator);
        tools.PushBack(entry, allocator);
        BehaviorBridge::AddTools(tools, allocator);
        EditorTools::AddTools(tools, allocator);
    }

    ToolProvider::Result EditorSystemComponent::Invoke(AZStd::string_view name, const Json& args, Json& data,
        Allocator& allocator, AZStd::string& detail)
    {
        if (name != "editor_info")
        {
            const auto result = m_tools.Invoke(name, args, data, allocator, detail);
            return result == Result::Unknown ? m_bridge.Invoke(name, args, data, allocator, detail) : result;
        }
        data.AddMember("kind", "editor", allocator);
        data.AddMember("ready", true, allocator);
        const auto platform = QGuiApplication::platformName().toUtf8();
        data.AddMember("qtPlatform", String({platform.constData(), static_cast<size_t>(platform.size())}, allocator), allocator);
        return Result::Complete;
    }
}
