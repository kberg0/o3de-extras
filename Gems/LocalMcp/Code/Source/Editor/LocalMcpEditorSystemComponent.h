/*
 * Copyright (c) Contributors to the Open 3D Engine Project.
 * For complete copyright and license terms please see the LICENSE at the root of this distribution.
 *
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 *
 */

#pragma once

#include "BehaviorBridge.h"
#include "EditorTools.h"
#include "LocalMcpServer.h"

#include <AzCore/Component/Component.h>
#include <AzCore/Component/ComponentApplicationBus.h>
#include <AzCore/Component/TickBus.h>
#include <AzToolsFramework/API/ToolsApplicationAPI.h>

namespace LocalMcp
{
    //! Settings root of the editor endpoint, separate from the runtime endpoint's.
    inline constexpr const char* EditorSettingsRoot = "/O3DE/Gems/LocalMcp/Editor";

    //! Port of the editor endpoint when the settings give none.
    inline constexpr AZ::s64 EditorDefaultPort = 4601;

    //! Runs the editor endpoint: a loopback-only LocalMcp server inside the Editor that also serves the editor and binding
    //! tools. It starts once the editor has initialized, and only when enabled under EditorSettingsRoot.
    class EditorSystemComponent final
        : public AZ::Component
        , private AZ::SystemTickBus::Handler
        , private AzToolsFramework::EditorEvents::Bus::Handler
        , private ToolProvider
    {
    public:
        AZ_COMPONENT(EditorSystemComponent, "{9BB863EE-F886-4EE6-A431-329B1D7A0463}");

        static void Reflect(AZ::ReflectContext* context);
        static void GetRequiredServices(AZ::ComponentDescriptor::DependencyArrayType& services);

        //! True for the Editor; false for the Asset Processor, builders, tools and launchers.
        static bool SupportsApplication(const AZ::ApplicationTypeQuery& type);

        // AZ::Component overrides
        void Activate() override;
        void Deactivate() override;

    private:
        // EditorEvents overrides
        bool SkipEditorStartupUI() override;
        void NotifyEditorInitialized() override;

        // SystemTickBus overrides
        void OnSystemTick() override;

        // ToolProvider overrides
        const char* GetServerName() const override;
        void AddTools(Json& tools, Allocator& allocator) const override;
        AZ::u64 GetToolsRevision() const override;
        Result Invoke(AZStd::string_view name, const Json& args, Json& data, Allocator& allocator, AZStd::string& detail) override;
        Result PollPending(Json& data, Allocator& allocator, AZStd::string& detail) override;
        void CancelPending() override;

        BehaviorBridge m_bridge;
        EditorTools m_tools{ m_bridge };
        Config m_config;
        Server m_server{ this };
    };
} // namespace LocalMcp
