/* Copyright (c) Contributors to the Open 3D Engine Project.
 * SPDX-License-Identifier: Apache-2.0 OR MIT */
#include "Editor/LocalMcpEditorSystemComponent.h"
#include <AzCore/Settings/SettingsRegistryImpl.h>
#include <AzCore/UnitTest/TestTypes.h>
#include <AzTest/AzTest.h>
AZ_UNIT_TEST_HOOK(DEFAULT_UNIT_TEST_ENV);
namespace LocalMcp
{
    class LocalMcpEditorTests : public UnitTest::LeakDetectionFixture {};
    TEST_F(LocalMcpEditorTests, EditorOnlyActivationExcludesBuildersAndLaunchers)
    {
        AZ::ApplicationTypeQuery type;
        using Mask = AZ::ApplicationTypeQuery::Masks;
        for (auto mask : {Mask::Invalid, Mask::Game, Mask::Tool, Mask::AssetProcessor, Mask::Tool | Mask::Headless})
        {
            type.m_maskValue = mask;
            EXPECT_FALSE(EditorSystemComponent::SupportsApplication(type));
        }
        type.m_maskValue = Mask::Editor | Mask::Headless;
        EXPECT_TRUE(EditorSystemComponent::SupportsApplication(type));
    }
    TEST_F(LocalMcpEditorTests, IndependentDisabledConfigurationAndLoopbackDefaults)
    {
        AZ::SettingsRegistryImpl registry;
        registry.Set("/O3DE/Gems/LocalMcp/Enabled", true);
        registry.Set("/O3DE/Gems/LocalMcp/Port", AZ::s64(4600));
        Config defaults;
        defaults.m_port = 4601;
        defaults.m_loopbackOnly = true;
        Config config;
        ASSERT_TRUE(ReadConfig(registry, config, EditorSettingsRoot, defaults));
        EXPECT_FALSE(config.m_enabled);
        EXPECT_EQ(config.m_port, 4601);
        EXPECT_TRUE(config.m_loopbackOnly);
        registry.Set("/O3DE/Gems/LocalMcp/Editor/Enabled", true);
        registry.Set("/O3DE/Gems/LocalMcp/Editor/Port", AZ::s64(4611));
        ASSERT_TRUE(ReadConfig(registry, config, EditorSettingsRoot, defaults));
        EXPECT_TRUE(config.m_enabled);
        EXPECT_EQ(config.m_port, 4611);
    }
}
