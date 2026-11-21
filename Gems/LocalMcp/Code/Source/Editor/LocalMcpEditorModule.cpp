/*
 * Copyright (c) Contributors to the Open 3D Engine Project.
 * For complete copyright and license terms please see the LICENSE at the root of this distribution.
 *
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 *
 */

#include "LocalMcpEditorSystemComponent.h"

#include <AzCore/Module/Module.h>

namespace LocalMcp
{
    //! Editor module of the LocalMcp gem; registers the editor endpoint's system component.
    class EditorModule final : public AZ::Module
    {
    public:
        AZ_RTTI(EditorModule, "{6C8F9CEF-62B6-4D9A-99A0-E6872852621F}", AZ::Module);

        EditorModule()
        {
            m_descriptors.push_back(EditorSystemComponent::CreateDescriptor());
        }

        AZ::ComponentTypeList GetRequiredSystemComponents() const override
        {
            return { azrtti_typeid<EditorSystemComponent>() };
        }
    };
} // namespace LocalMcp

AZ_DECLARE_MODULE_CLASS(Gem_LocalMcp_Editor, LocalMcp::EditorModule)
