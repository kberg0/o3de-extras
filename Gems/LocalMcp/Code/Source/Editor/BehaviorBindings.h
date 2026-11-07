/* Copyright (c) Contributors to the Open 3D Engine Project.
 * SPDX-License-Identifier: Apache-2.0 OR MIT */
#pragma once
#include <AzCore/RTTI/BehaviorContext.h>
#include <AzCore/std/functional.h>
namespace LocalMcp::Bindings
{
    enum class Kind { Class, Method, Property, Bus, Event, BusProperty };
    // A view is borrowed only for the Visit callback. No duplicate reflection registry is retained.
    struct BindingView
    {
        Kind m_kind;
        AZStd::string m_id;
        AZStd::string_view m_name;
        const AZ::AttributeArray* m_attributes = nullptr;
        const AZ::AttributeArray* m_scope = nullptr;
        const AZ::BehaviorClass* m_class = nullptr;
        AZ::BehaviorMethod* m_method = nullptr;
        const AZ::BehaviorProperty* m_property = nullptr;
        const AZ::BehaviorEBus* m_bus = nullptr;
        const AZ::BehaviorEBusEventSender* m_sender = nullptr;
        const AZ::BehaviorEBus::VirtualProperty* m_busProperty = nullptr;
    };
    using Visitor = AZStd::function<bool(const BindingView&)>;
    void Visit(AZ::BehaviorContext& context, const Visitor& visitor);
    const char* KindName(Kind kind);
    AZStd::string Escape(AZStd::string_view name);
}
