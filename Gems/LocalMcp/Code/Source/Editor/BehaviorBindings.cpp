/*
 * Copyright (c) Contributors to the Open 3D Engine Project.
 * For complete copyright and license terms please see the LICENSE at the root of this distribution.
 *
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 *
 */

#include "BehaviorBindings.h"

#include <AzCore/StringFunc/StringFunc.h>

namespace LocalMcp::Bindings
{
    AZStd::string Escape(AZStd::string_view name)
    {
        // Escape '~' first, so the '~' that escapes a '/' is not escaped again.
        constexpr bool caseSensitive = true;
        AZStd::string result(name);
        AZ::StringFunc::Replace(result, "~", "~0", caseSensitive);
        AZ::StringFunc::Replace(result, "/", "~1", caseSensitive);
        return result;
    }

    void Visit(AZ::BehaviorContext& context, const Visitor& visitor)
    {
        for (const auto& [name, method] : context.m_methods)
        {
            BindingView view{ Kind::Method, "global/method/" + Escape(name), name };
            view.m_method = method;
            view.m_attributes = method ? &method->m_attributes : nullptr;
            view.m_scope = view.m_attributes;
            if (!visitor(view))
            {
                return;
            }
        }

        for (const auto& [name, property] : context.m_properties)
        {
            BindingView view{ Kind::Property, "global/property/" + Escape(name), name };
            view.m_property = property;
            view.m_attributes = property ? &property->m_attributes : nullptr;
            view.m_scope = view.m_attributes;
            if (!visitor(view))
            {
                return;
            }
        }

        for (const auto& [name, type] : context.m_classes)
        {
            const AZStd::string classId = "class/" + Escape(name);
            BindingView view{ Kind::Class, classId, name };
            view.m_class = type;
            view.m_attributes = type ? &type->m_attributes : nullptr;
            view.m_scope = view.m_attributes;
            if (!visitor(view))
            {
                return;
            }
            if (!type)
            {
                continue;
            }

            for (const auto& [memberName, method] : type->m_methods)
            {
                BindingView member{ Kind::Method, classId + "/method/" + Escape(memberName), memberName };
                member.m_class = type;
                member.m_scope = &type->m_attributes;
                member.m_attributes = method ? &method->m_attributes : nullptr;
                member.m_method = method;
                if (!visitor(member))
                {
                    return;
                }
            }

            for (const auto& [memberName, property] : type->m_properties)
            {
                BindingView member{ Kind::Property, classId + "/property/" + Escape(memberName), memberName };
                member.m_class = type;
                member.m_scope = &type->m_attributes;
                member.m_attributes = property ? &property->m_attributes : nullptr;
                member.m_property = property;
                if (!visitor(member))
                {
                    return;
                }
            }
        }

        for (const auto& [name, bus] : context.m_ebuses)
        {
            const AZStd::string busId = "bus/" + Escape(name);
            BindingView view{ Kind::Bus, busId, name };
            view.m_bus = bus;
            view.m_attributes = bus ? &bus->m_attributes : nullptr;
            view.m_scope = view.m_attributes;
            if (!visitor(view))
            {
                return;
            }
            if (!bus)
            {
                continue;
            }

            for (const auto& [eventName, sender] : bus->m_events)
            {
                BindingView member{ Kind::Event, busId + "/event/" + Escape(eventName), eventName };
                member.m_bus = bus;
                member.m_scope = &bus->m_attributes;
                member.m_attributes = &sender.m_attributes;
                member.m_sender = &sender;
                if (!visitor(member))
                {
                    return;
                }
            }

            for (const auto& [propertyName, property] : bus->m_virtualProperties)
            {
                BindingView member{ Kind::BusProperty, busId + "/property/" + Escape(propertyName), propertyName };
                member.m_bus = bus;
                member.m_scope = &bus->m_attributes;
                member.m_busProperty = &property;
                if (!visitor(member))
                {
                    return;
                }
            }
        }
    }
} // namespace LocalMcp::Bindings
