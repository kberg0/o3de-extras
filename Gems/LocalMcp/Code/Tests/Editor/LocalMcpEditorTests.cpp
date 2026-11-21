/*
 * Copyright (c) Contributors to the Open 3D Engine Project.
 * For complete copyright and license terms please see the LICENSE at the root of this distribution.
 *
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 *
 */

#include "Editor/LocalMcpEditorSystemComponent.h"

#include <AzCore/Settings/SettingsRegistryImpl.h>
#include <AzCore/UnitTest/TestTypes.h>
#include <AzTest/AzTest.h>

AZ_UNIT_TEST_HOOK(DEFAULT_UNIT_TEST_ENV);

namespace LocalMcp
{
    class LocalMcpEditorTests : public UnitTest::LeakDetectionFixture
    {
    };

    TEST_F(LocalMcpEditorTests, EditorOnlyActivationExcludesBuildersAndLaunchers)
    {
        using Mask = AZ::ApplicationTypeQuery::Masks;
        AZ::ApplicationTypeQuery type;
        for (const auto mask : { Mask::Invalid, Mask::Game, Mask::Tool, Mask::AssetProcessor, Mask::Tool | Mask::Headless })
        {
            type.m_maskValue = mask;
            EXPECT_FALSE(EditorSystemComponent::SupportsApplication(type));
        }

        type.m_maskValue = Mask::Editor | Mask::Headless;
        EXPECT_TRUE(EditorSystemComponent::SupportsApplication(type));
    }

    TEST_F(LocalMcpEditorTests, IndependentDisabledConfigurationAndLoopbackDefaults)
    {
        constexpr AZ::s64 RuntimePort = 4600;
        constexpr AZ::s64 EditorPort = 4611;
        const AZStd::string runtimeRoot(SettingsRoot);
        const AZStd::string editorRoot(EditorSettingsRoot);

        // The runtime endpoint's settings do not enable or move the editor endpoint.
        AZ::SettingsRegistryImpl registry;
        registry.Set(runtimeRoot + "/Enabled", true);
        registry.Set(runtimeRoot + "/Port", RuntimePort);
        Config defaults;
        defaults.m_port = EditorDefaultPort;
        defaults.m_loopbackOnly = true;
        Config config;
        ASSERT_TRUE(ReadConfig(registry, config, EditorSettingsRoot, defaults));
        EXPECT_FALSE(config.m_enabled);
        EXPECT_EQ(config.m_port, EditorDefaultPort);
        EXPECT_TRUE(config.m_loopbackOnly);

        registry.Set(editorRoot + "/Enabled", true);
        registry.Set(editorRoot + "/Port", EditorPort);
        ASSERT_TRUE(ReadConfig(registry, config, EditorSettingsRoot, defaults));
        EXPECT_TRUE(config.m_enabled);
        EXPECT_EQ(config.m_port, EditorPort);
    }
} // namespace LocalMcp
