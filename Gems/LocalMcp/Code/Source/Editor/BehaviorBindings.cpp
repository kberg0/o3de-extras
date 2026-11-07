/* Copyright (c) Contributors to the Open 3D Engine Project.
 * SPDX-License-Identifier: Apache-2.0 OR MIT */
#include "BehaviorBindings.h"
namespace LocalMcp::Bindings
{
    AZStd::string Escape(AZStd::string_view name)
    {
        AZStd::string result;
        for (const char ch : name)
        {
            if (ch == '~') { result += "~0"; }
            else if (ch == '/') { result += "~1"; }
            else { result += ch; }
        }
        return result;
    }

    const char* KindName(Kind kind)
    {
        switch (kind)
        {
        case Kind::Class: return "class";
        case Kind::Method: return "method";
        case Kind::Property: return "property";
        case Kind::Bus: return "bus";
        case Kind::Event: return "event";
        case Kind::BusProperty: return "bus_property";
        }
        return "unknown";
    }

    void Visit(AZ::BehaviorContext& context, const Visitor& visitor)
    {
        for (const auto& [name, method] : context.m_methods)
        {
            BindingView view{Kind::Method, "global/method/" + Escape(name), name};
            view.m_method = method;
            view.m_scope = view.m_attributes = method ? &method->m_attributes : nullptr;
            if (!visitor(view)) { return; }
        }
        for (const auto& [name, property] : context.m_properties)
        {
            BindingView view{Kind::Property, "global/property/" + Escape(name), name};
            view.m_property = property;
            view.m_scope = view.m_attributes = property ? &property->m_attributes : nullptr;
            if (!visitor(view)) { return; }
        }
        for (const auto& [name, type] : context.m_classes)
        {
            const AZStd::string base = "class/" + Escape(name);
            BindingView view{Kind::Class, base, name};
            view.m_class = type;
            view.m_scope = view.m_attributes = type ? &type->m_attributes : nullptr;
            if (!visitor(view)) { return; }
            if (!type) { continue; }
            for (const auto& [member, method] : type->m_methods)
            {
                BindingView item{Kind::Method, base + "/method/" + Escape(member), member};
                item.m_class = type;
                item.m_scope = &type->m_attributes;
                item.m_attributes = method ? &method->m_attributes : nullptr;
                item.m_method = method;
                if (!visitor(item)) { return; }
            }
            for (const auto& [member, property] : type->m_properties)
            {
                BindingView item{Kind::Property, base + "/property/" + Escape(member), member};
                item.m_class = type;
                item.m_scope = &type->m_attributes;
                item.m_attributes = property ? &property->m_attributes : nullptr;
                item.m_property = property;
                if (!visitor(item)) { return; }
            }
        }
        for (const auto& [name, bus] : context.m_ebuses)
        {
            const AZStd::string base = "bus/" + Escape(name);
            BindingView view{Kind::Bus, base, name};
            view.m_bus = bus;
            view.m_scope = view.m_attributes = bus ? &bus->m_attributes : nullptr;
            if (!visitor(view)) { return; }
            if (!bus) { continue; }
            for (const auto& [event, sender] : bus->m_events)
            {
                BindingView item{Kind::Event, base + "/event/" + Escape(event), event};
                item.m_bus = bus;
                item.m_scope = &bus->m_attributes;
                item.m_attributes = &sender.m_attributes;
                item.m_sender = &sender;
                if (!visitor(item)) { return; }
            }
            for (const auto& [propertyName, property] : bus->m_virtualProperties)
            {
                BindingView item{Kind::BusProperty, base + "/property/" + Escape(propertyName), propertyName};
                item.m_bus = bus;
                item.m_scope = &bus->m_attributes;
                item.m_busProperty = &property;
                if (!visitor(item)) { return; }
            }
        }
    }

}
