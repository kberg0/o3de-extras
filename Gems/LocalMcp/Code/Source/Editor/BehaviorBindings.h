/*
 * Copyright (c) Contributors to the Open 3D Engine Project.
 * For complete copyright and license terms please see the LICENSE at the root of this distribution.
 *
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 *
 */

#pragma once

#include <AzCore/Preprocessor/Enum.h>
#include <AzCore/RTTI/BehaviorContext.h>
#include <AzCore/std/functional.h>
#include <AzCore/std/string/string.h>
#include <AzCore/std/string/string_view.h>

//! Enumerates BehaviorContext entries under stable binding IDs such as "bus/TransformBus/event/GetWorldTM".
//! No copy of the reflection registry is kept; every call walks the live BehaviorContext.
namespace LocalMcp::Bindings
{
    //! What a binding refers to. ToString() gives the name that binding_search reports and filters by.
    AZ_ENUM_CLASS(Kind, Class, Method, Property, Bus, Event, BusProperty);

    //! One reflected entry, borrowed from the BehaviorContext for the duration of a Visit() callback only.
    struct BindingView
    {
        Kind m_kind;
        //! Canonical binding ID: owner and member names joined with '/', each escaped by Escape().
        AZStd::string m_id;
        AZStd::string_view m_name;
        //! Attributes of the entry itself, or nullptr when it has none.
        const AZ::AttributeArray* m_attributes = nullptr;
        //! Attributes of the class or bus that owns the entry, which decide its script scope.
        const AZ::AttributeArray* m_scope = nullptr;

        // The native definition; only the members that match m_kind are set.
        const AZ::BehaviorClass* m_class = nullptr;
        AZ::BehaviorMethod* m_method = nullptr;
        const AZ::BehaviorProperty* m_property = nullptr;
        const AZ::BehaviorEBus* m_bus = nullptr;
        const AZ::BehaviorEBusEventSender* m_sender = nullptr;
        const AZ::BehaviorEBus::VirtualProperty* m_busProperty = nullptr;
    };

    //! Called for each binding; returns false to stop the walk.
    using Visitor = AZStd::function<bool(const BindingView&)>;

    //! Visits global methods, global properties, classes with their members, and buses with their events and properties.
    void Visit(AZ::BehaviorContext& context, const Visitor& visitor);

    //! Escapes a name for use as one binding ID segment, as JSON Pointer does: '~' becomes "~0" and '/' becomes "~1".
    AZStd::string Escape(AZStd::string_view name);
} // namespace LocalMcp::Bindings
