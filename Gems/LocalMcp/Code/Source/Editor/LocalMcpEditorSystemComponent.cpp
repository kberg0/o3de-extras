/*
 * Copyright (c) Contributors to the Open 3D Engine Project.
 * For complete copyright and license terms please see the LICENSE at the root of this distribution.
 *
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 *
 */

#include "LocalMcpEditorSystemComponent.h"

#include "PythonExecute.h"

#include <AzCore/Serialization/SerializeContext.h>
#include <EditorPythonBindings/EditorPythonBindingsSymbols.h>

namespace LocalMcp
{
    namespace
    {
        //! serverInfo.name of the editor endpoint.
        constexpr const char* EditorServerName = "o3de-local-editor";
    } // namespace

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
        defaults.m_port = EditorDefaultPort;
        defaults.m_loopbackOnly = true;
        auto* registry = AZ::SettingsRegistry::Get();
        const bool enabled = registry && ReadConfig(*registry, m_config, EditorSettingsRoot, defaults) && m_config.m_enabled;
        if (!enabled)
        {
            return;
        }

        // Read once here, so a setting changed later, for example over the endpoint's console tools, has no effect.
        bool pythonExecute = false;
        registry->Get(pythonExecute, PythonExecute::EnabledSetting);

        m_bridge.Activate();
        m_tools.Activate(m_config.m_idleTimeoutMs, pythonExecute);

        // The editor's existing startup hook suppresses both welcome dialogs and last-level autoload.
        AzToolsFramework::EditorEvents::Bus::Handler::BusConnect();
    }

    void EditorSystemComponent::Deactivate()
    {
        AZ::SystemTickBus::Handler::BusDisconnect();
        AzToolsFramework::EditorEvents::Bus::Handler::BusDisconnect();
        m_server.Stop();
        m_tools.Deactivate();
        m_bridge.Deactivate();
    }

    bool EditorSystemComponent::SkipEditorStartupUI()
    {
        return true;
    }

    void EditorSystemComponent::NotifyEditorInitialized()
    {
        if (!m_server.Start(m_config))
        {
            AZ_Warning("LocalMcp", false, "Editor endpoint startup failed on port %lld.", m_config.m_port);
            return;
        }

        AZ::SystemTickBus::Handler::BusConnect();
        AZ_TracePrintf("LocalMcp", "Editor listening on loopback port %u\n", m_server.GetPort());
    }

    void EditorSystemComponent::OnSystemTick()
    {
        m_server.Poll();
    }

    const char* EditorSystemComponent::GetServerName() const
    {
        return EditorServerName;
    }

    void EditorSystemComponent::AddTools(Json& tools, Allocator& allocator) const
    {
        m_tools.AddTools(tools, allocator);
        m_bridge.AddTools(tools, allocator);
    }

    AZ::u64 EditorSystemComponent::GetToolsRevision() const
    {
        return m_bridge.GetToolsRevision();
    }

    ToolProvider::Result EditorSystemComponent::Invoke(
        AZStd::string_view name, const Json& args, Json& data, Allocator& allocator, AZStd::string& detail)
    {
        const Result result = m_tools.Invoke(name, args, data, allocator, detail);
        if (result != Result::Unknown)
        {
            return result;
        }
        return m_bridge.Invoke(name, args, data, allocator, detail);
    }

    ToolProvider::Result EditorSystemComponent::PollPending(Json& data, Allocator& allocator, AZStd::string& detail)
    {
        return m_tools.PollPending(data, allocator, detail);
    }

    void EditorSystemComponent::CancelPending()
    {
        m_tools.CancelPending();
    }
} // namespace LocalMcp
