/*
 * Copyright (c) Contributors to the Open 3D Engine Project.
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 */
#include "LocalMcpServer.h"
#include <AzCore/Component/Component.h>
#include <AzCore/Component/TickBus.h>
#include <AzCore/Module/Module.h>
#include <AzCore/Serialization/SerializeContext.h>

namespace LocalMcp
{
    class SystemComponent final : public AZ::Component, private AZ::SystemTickBus::Handler
    {
    public:
        AZ_COMPONENT(SystemComponent, "{C28A4CF5-C36F-4142-9574-A369A509DE16}");
        static void Reflect(AZ::ReflectContext* context)
        {
            if (auto* serializeContext = azrtti_cast<AZ::SerializeContext*>(context))
            {
                serializeContext->Class<SystemComponent, AZ::Component>()->Version(1);
            }
        }
        void Activate() override
        {
            auto* registry = AZ::SettingsRegistry::Get();
            Config config;
            if (registry && ReadConfig(*registry, config) && config.m_enabled)
            {
                if (m_server.Start(config))
                {
                    AZ::SystemTickBus::Handler::BusConnect();
                    AZ_TracePrintf("LocalMcp", "Listening on port %u (local peers only)\n", m_server.GetPort());
                }
                else
                {
                    AZ_Warning("LocalMcp", false, "Startup failed; check port, authentication and certificate configuration.");
                }
            }
        }
        void Deactivate() override
        {
            AZ::SystemTickBus::Handler::BusDisconnect();
            m_server.Stop();
        }

    private:
        void OnSystemTick() override
        {
            m_server.Poll();
        }
        Server m_server;
    };

    class Module final : public AZ::Module
    {
    public:
        AZ_RTTI(Module, "{6F6B4FD4-A17F-4CA6-8212-2C93FEBD4173}", AZ::Module);
        Module()
        {
            m_descriptors.push_back(SystemComponent::CreateDescriptor());
        }
        AZ::ComponentTypeList GetRequiredSystemComponents() const override
        {
            return { azrtti_typeid<SystemComponent>() };
        }
    };
}

AZ_DECLARE_MODULE_CLASS(Gem_LocalMcp, LocalMcp::Module)
