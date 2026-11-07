/* Copyright (c) Contributors to the Open 3D Engine Project.
 * SPDX-License-Identifier: Apache-2.0 OR MIT */
#include "BehaviorBridge.h"
#include "LocalMcpSearch.h"
#include <AzCore/Component/ComponentApplicationBus.h>
#include <AzCore/Script/ScriptContextAttributes.h>
#include <AzCore/std/sort.h>
#include <AzCore/std/any.h>
#include <AzToolsFramework/API/EditorPythonConsoleBus.h>
#include <pybind11/embed.h>
#include <stdexcept>

namespace LocalMcp
{
    Json BehaviorBridge::Metadata(const BindingView& binding, Allocator& allocator)
    {
        using namespace EditorPythonBindings;
        Json result(rapidjson::kObjectType);
        result.AddMember("binding", String(binding.m_id, allocator), allocator);
        result.AddMember("kind", String(Bindings::KindName(binding.m_kind), allocator), allocator);
        result.AddMember("name", String(binding.m_name, allocator), allocator);
        AZStd::string alias(binding.m_name);
        AZ::Script::Attributes::ExcludeFlags exclude = static_cast<AZ::Script::Attributes::ExcludeFlags>(0);
        if (binding.m_attributes)
        {
            Scope::FetchScriptName(*binding.m_attributes, alias);
            result.AddMember("documentation", String(Text::ReadStringAttribute(*binding.m_attributes,
                AZ::Script::Attributes::ToolTip), allocator), allocator);
            AZ::AttributeReader(nullptr, AZ::FindAttribute(AZ::Script::Attributes::ExcludeFrom,
                *binding.m_attributes)).Read<AZ::Script::Attributes::ExcludeFlags>(exclude);
        }
        result.AddMember("alias", String(alias, allocator), allocator);
        result.AddMember("excludeFlags", static_cast<AZ::u64>(exclude), allocator);
        if (binding.m_attributes)
        {
            if (auto module = Module::GetName(*binding.m_attributes))
            {
                result.AddMember("declaredModule", String(*module, allocator), allocator);
            }
            AZ::Script::Attributes::ScopeFlags declaredScope;
            if (AZ::AttributeReader(nullptr, AZ::FindAttribute(AZ::Script::Attributes::Scope,
                *binding.m_attributes)).Read<AZ::Script::Attributes::ScopeFlags>(declaredScope))
            {
                result.AddMember("declaredScope", static_cast<AZ::u64>(declaredScope), allocator);
            }
        }
        AZ::Script::Attributes::ScopeFlags scope = AZ::Script::Attributes::ScopeFlags::Launcher;
        AZStd::string reason;
        if (binding.m_scope)
        {
            AZ::AttributeReader(nullptr, AZ::FindAttribute(AZ::Script::Attributes::Scope,
                *binding.m_scope)).Read<AZ::Script::Attributes::ScopeFlags>(scope);
            auto module = Module::GetName(*binding.m_scope);
            if (module) { result.AddMember("module", String(*module, allocator), allocator); }
            if (!Scope::IsBehaviorFlaggedForEditor(*binding.m_scope))
            {
                reason = "Binding scope excludes editor automation";
            }
        }
        else
        {
            reason = "Reflected entry is missing its native definition";
        }
        if (reason.empty() && binding.m_kind == Kind::Bus)
        {
            reason = "Bus container; select a reflected event";
        }
        if (reason.empty() && binding.m_kind == Kind::Event &&
            (!binding.m_sender || (!binding.m_sender->m_broadcast && !binding.m_sender->m_event)))
        {
            reason = "Event has no synchronous sender; queued and notification-only senders are unsupported";
        }
        if (reason.empty() && binding.m_kind == Kind::BusProperty)
        {
            reason = "Virtual bus property; invoke its described getter/setter event with an explicit address";
        }
        result.AddMember("scope", static_cast<AZ::u64>(scope), allocator);
        result.AddMember("callable", reason.empty(), allocator);
        result.AddMember("reason", String(reason, allocator), allocator);
        if (binding.m_class)
        {
            result.AddMember("ownerType", String(binding.m_class->m_typeId.ToString<AZStd::string>(), allocator), allocator);
        }
        return result;
    }
    void BehaviorBridge::Describe(const BindingView& binding, Json& data, Allocator& allocator)
    {
        using namespace EditorPythonBindings;
        data = Metadata(binding, allocator);
        Text::PythonBehaviorDescription text;
        auto parameterInfo = [&](const AZ::BehaviorParameter* parameter)
        {
            Json info(rapidjson::kObjectType);
            if (parameter)
            {
                info.AddMember("type", String(parameter->m_typeId.ToString<AZStd::string>(), allocator), allocator);
                info.AddMember("nativeTypeName", String(parameter->m_name ? parameter->m_name : "", allocator), allocator);
                info.AddMember("traits", parameter->m_traits, allocator);
                info.AddMember("pythonType", String(text.FetchPythonTypeName(*parameter), allocator), allocator);
                info.AddMember("nativeNullable", (parameter->m_traits & AZ::BehaviorParameter::TR_POINTER) != 0, allocator);
                info.AddMember("nullable", parameter->m_typeId == azrtti_typeid<void>() || parameter->m_typeId == azrtti_typeid<AZStd::any>(), allocator);
                info.AddMember("unsupportedReason", String(BridgeValues::Unsupported(*parameter), allocator), allocator);
            }
            return info;
        };
        auto methods = [&](AZ::BehaviorMethod* first)
        {
            Json overloads(rapidjson::kArrayType);
            size_t index = 0;
            for (auto* method = first; method; method = method->m_overload, ++index)
            {
                Json entry(rapidjson::kObjectType);
                entry.AddMember("overload", static_cast<uint64_t>(index), allocator);
                entry.AddMember("signature", String(text.MethodDefinition(binding.m_name, *method,
                    binding.m_class, true, true), allocator), allocator);
                entry.AddMember("minArguments", static_cast<uint64_t>(method->GetMinNumberOfArguments()), allocator);
                Json arguments(rapidjson::kArrayType);
                for (size_t argumentIndex = 0; argumentIndex < method->GetNumArguments(); ++argumentIndex)
                {
                    Json argument = parameterInfo(method->GetArgument(argumentIndex));
                    const auto* name = method->GetArgumentName(argumentIndex);
                    const auto* help = method->GetArgumentToolTip(argumentIndex);
                    argument.AddMember("name", String(name ? AZStd::string_view(*name) : AZStd::string_view{}, allocator), allocator);
                    argument.AddMember("documentation", String(help ? AZStd::string_view(*help) : AZStd::string_view{}, allocator), allocator);
                    const auto defaultValue = method->GetDefaultValue(argumentIndex);
                    argument.AddMember("hasDefault", !!defaultValue, allocator);
                    if (defaultValue)
                    {
                        Convert::StackVariableAllocator variables;
                        AZ::BehaviorArgument nativeDefault(defaultValue->GetValue());
                        try
                        {
                            AZStd::string error;
                            auto value = Convert::BehaviorValueParameterToPython(nativeDefault, variables, &error);
                            if (!error.empty()) { throw BridgeError(error.c_str()); }
                            argument.AddMember("default", m_values.Encode(value, method->GetArgument(argumentIndex), allocator), allocator);
                        }
                        catch (const std::exception& error)
                        {
                            argument.AddMember("defaultError", String(error.what(), allocator), allocator);
                        }
                    }
                    arguments.PushBack(argument, allocator);
                }
                entry.AddMember("arguments", arguments, allocator);
                entry.AddMember("result", parameterInfo(method->GetResult()), allocator);
                overloads.PushBack(entry, allocator);
            }
            return overloads;
        };
        if (binding.m_kind == Kind::Class && binding.m_class)
        {
            Json constructors(rapidjson::kArrayType);
            for (size_t index = 0; index < binding.m_class->m_constructors.size(); ++index)
            {
                Json item(rapidjson::kObjectType);
                item.AddMember("overload", static_cast<uint64_t>(index), allocator);
                item.AddMember("signatures", methods(binding.m_class->m_constructors[index]), allocator);
                constructors.PushBack(item, allocator);
            }
            data.AddMember("constructors", constructors, allocator);
            data.AddMember("defaultFactory", binding.m_class->m_defaultConstructor != nullptr, allocator);
            data.AddMember("copyable", binding.m_class->m_cloner != nullptr, allocator);
            Json members(rapidjson::kArrayType);
            for (const auto& [name, method] : binding.m_class->m_methods)
            {
                (void)method;
                members.PushBack(String(binding.m_id + "/method/" + Bindings::Escape(name), allocator), allocator);
            }
            for (const auto& [name, property] : binding.m_class->m_properties)
            {
                (void)property;
                members.PushBack(String(binding.m_id + "/property/" + Bindings::Escape(name), allocator), allocator);
            }
            data.AddMember("members", members, allocator);
        }
        if (binding.m_method)
        {
            data.AddMember("overloads", methods(binding.m_method), allocator);
        }
        if (binding.m_property)
        {
            data.AddMember("readable", binding.m_property->m_getter != nullptr, allocator);
            data.AddMember("writable", binding.m_property->m_setter != nullptr, allocator);
            data.AddMember("getter", methods(binding.m_property->m_getter), allocator);
            data.AddMember("setter", methods(binding.m_property->m_setter), allocator);
        }
        if (binding.m_bus)
        {
            data.AddMember("address", parameterInfo(&binding.m_bus->m_idParam), allocator);
            if (binding.m_kind == Kind::Bus)
            {
                Json members(rapidjson::kArrayType);
                for (const auto& [name, sender] : binding.m_bus->m_events)
                {
                    (void)sender;
                    members.PushBack(String(binding.m_id + "/event/" + Bindings::Escape(name), allocator), allocator);
                }
                data.AddMember("members", members, allocator);
            }
        }
        if (binding.m_sender)
        {
            data.AddMember("broadcast", methods(binding.m_sender->m_broadcast), allocator);
            data.AddMember("event", methods(binding.m_sender->m_event), allocator);
            data.AddMember("queuedBroadcastAvailable", binding.m_sender->m_queueBroadcast != nullptr, allocator);
            data.AddMember("queuedEventAvailable", binding.m_sender->m_queueEvent != nullptr, allocator);
        }
        if (binding.m_busProperty)
        {
            Json accessors(rapidjson::kArrayType);
            for (const auto& [name, sender] : binding.m_bus->m_events)
            {
                if (&sender == binding.m_busProperty->m_getter || &sender == binding.m_busProperty->m_setter)
                {
                    accessors.PushBack(String("bus/" + Bindings::Escape(binding.m_bus->m_name) + "/event/" + Bindings::Escape(name), allocator), allocator);
                }
            }
            data.AddMember("accessors", accessors, allocator);
        }
    }

    void BehaviorBridge::Activate()
    {
        m_mainThread = AZStd::this_thread::get_id();
        m_values.Activate();
        EditorPythonBindings::EditorPythonBindingsNotificationBus::Handler::BusConnect();
        AzToolsFramework::EditorEntityContextNotificationBus::Handler::BusConnect();
        AzToolsFramework::EntityCompositionNotificationBus::Handler::BusConnect();
        AZ::BehaviorContext* context = nullptr;
        AZ::ComponentApplicationBus::BroadcastResult(context, &AZ::ComponentApplicationRequests::GetBehaviorContext);
        if (context) { AZ::BehaviorContextBus::Handler::BusConnect(context); }
    }

    void BehaviorBridge::ClearHandles()
    {
        if (m_values.Empty()) { return; }
        auto* python = AZ::Interface<AzToolsFramework::EditorPythonEventsInterface>::Get();
        AZ_Assert(python && python->IsPythonActive(), "Owned Python objects must be released before interpreter shutdown");
        python->ExecuteWithLock([this]() { m_values.Clear(); });
    }

    void BehaviorBridge::OnPreFinalize()
    {
        ClearHandles();
    }

    void BehaviorBridge::Deactivate()
    {
        ClearHandles();
        AZ::BehaviorContextBus::Handler::BusDisconnect();
        AzToolsFramework::EntityCompositionNotificationBus::Handler::BusDisconnect();
        AzToolsFramework::EditorEntityContextNotificationBus::Handler::BusDisconnect();
        EditorPythonBindings::EditorPythonBindingsNotificationBus::Handler::BusDisconnect();
    }

    Json BehaviorBridge::NormalizeValue(const Json& value, const AZ::TypeId& type, Allocator& allocator)
    {
        if (AZStd::this_thread::get_id() != m_mainThread) { throw BridgeError("Binding conversion requires the editor main thread"); }
        auto* python = AZ::Interface<AzToolsFramework::EditorPythonEventsInterface>::Get();
        if (!python || !python->IsPythonActive()) { throw BridgeError("EditorPythonBindings interpreter is not active"); }
        Json result;
        AZStd::string failure;
        python->ExecuteWithLock([&]()
        {
            try
            {
                pybind11::module::import("azlmbr.object");
                AZ::BehaviorParameter expected{};
                expected.m_typeId = type;
                result = m_values.Encode(m_values.Decode(value, &expected), &expected, allocator);
            }
            catch (const std::exception& error) { failure = error.what(); }
        });
        if (!failure.empty()) { throw BridgeError(AZStd::move(failure)); }
        return result;
    }

    ToolProvider::Result BehaviorBridge::Invoke(AZStd::string_view tool, const Json& args, Json& data,
        Allocator& allocator, AZStd::string& detail)
    {
        using Result = ToolProvider::Result;
        if (tool != "binding_search" && tool != "binding_describe" && tool != "binding_call" &&
            tool != "binding_get" && tool != "binding_set" && tool != "binding_release")
        {
            return Result::Unknown;
        }
        if (!args.IsObject())
        {
            detail = "Expected object arguments";
            return Result::InvalidArguments;
        }
        if (AZStd::this_thread::get_id() != m_mainThread)
        {
            data.AddMember("error", "Binding tools require the editor main thread", allocator);
            return Result::Complete;
        }
        if (tool == "binding_search")
        {
            if (!IsString(args, "query", 512) || (args.HasMember("kind") && !IsString(args, "kind", 32)) ||
                (args.HasMember("limit") && (!args["limit"].IsUint() || !args["limit"].GetUint() || args["limit"].GetUint() > 100)) ||
                (args.HasMember("after") && !args["after"].IsUint()))
            {
                detail = "Expected query, optional kind, limit [1,100] and nonnegative after";
                return Result::InvalidArguments;
            }
            const AZStd::string_view kind = args.HasMember("kind") ? args["kind"].GetString() : "";
            if (!kind.empty() && kind != "class" && kind != "method" && kind != "property" && kind != "bus" && kind != "event" && kind != "bus_property")
            {
                detail = "Unknown binding kind";
                return Result::InvalidArguments;
            }
            AZ::BehaviorContext* context = nullptr;
            AZ::ComponentApplicationBus::BroadcastResult(context, &AZ::ComponentApplicationRequests::GetBehaviorContext);
            if (!context)
            {
                data.AddMember("error", "Application has no BehaviorContext", allocator);
                return Result::Complete;
            }
            struct Match { int m_score; AZStd::string m_id; };
            AZStd::vector<Match> matches;
            const SearchTerms terms(args["query"].GetString());
            Bindings::Visit(*context, [&](const BindingView& binding)
            {
                if (!kind.empty() && kind != Bindings::KindName(binding.m_kind)) { return true; }
                AZStd::string alias(binding.m_name);
                AZStd::string help;
                if (binding.m_attributes)
                {
                    EditorPythonBindings::Scope::FetchScriptName(*binding.m_attributes, alias);
                    help = EditorPythonBindings::Text::ReadStringAttribute(*binding.m_attributes, AZ::Script::Attributes::ToolTip);
                }
                const int score = AZStd::max(terms.Rank(binding.m_id, help),
                    AZStd::max(terms.Rank(binding.m_name, help), terms.Rank(alias, help)));
                if (score >= 0) { matches.push_back({score, binding.m_id}); }
                return true;
            });
            AZStd::sort(matches.begin(), matches.end(), [](const auto& left, const auto& right)
            {
                return left.m_score != right.m_score ? left.m_score > right.m_score : left.m_id < right.m_id;
            });
            size_t next = args.HasMember("after") ? args["after"].GetUint() : 0;
            next = AZStd::min(next, matches.size());
            const size_t limit = args.HasMember("limit") ? args["limit"].GetUint() : 20;
            Json list(rapidjson::kArrayType);
            AZStd::unordered_map<AZStd::string, rapidjson::SizeType> page;
            for (; next < matches.size() && list.Size() < limit; ++next)
            {
                page.emplace(matches[next].m_id, list.Size());
                list.PushBack(Json(), allocator);
            }
            Bindings::Visit(*context, [&](const BindingView& binding)
            {
                const auto found = page.find(binding.m_id);
                if (found != page.end()) { list[found->second] = Metadata(binding, allocator); }
                return true;
            });
            data.AddMember("matches", list, allocator);
            data.AddMember("next", static_cast<uint64_t>(next), allocator);
            data.AddMember("total", static_cast<uint64_t>(matches.size()), allocator);
            return Result::Complete;
        }
        if (tool != "binding_release" && !IsString(args, "binding", 2048))
        {
            detail = "Expected canonical binding ID";
            return Result::InvalidArguments;
        }
        if ((tool == "binding_call" && (!args.HasMember("args") || !args["args"].IsArray() || args["args"].Size() > 32)) ||
            (tool == "binding_set" && !args.HasMember("value")) ||
            (args.HasMember("overload") && !args["overload"].IsUint()) ||
            (tool == "binding_release" && (!args.HasMember("handles") || !args["handles"].IsArray() || args["handles"].Size() > 256)))
        {
            detail = "Expected bounded call args, setter value, nonnegative overload index or handle list";
            return Result::InvalidArguments;
        }
        auto* python = AZ::Interface<AzToolsFramework::EditorPythonEventsInterface>::Get();
        if (!python || !python->IsPythonActive())
        {
            data.AddMember("error", "EditorPythonBindings interpreter is not active", allocator);
            return Result::Complete;
        }
        python->ExecuteWithLock([&]()
        {
            try
            {
                pybind11::module::import("azlmbr.object");
                if (tool == "binding_release")
                {
                    data.AddMember("released", static_cast<uint64_t>(m_values.Release(args["handles"])), allocator);
                    return;
                }
                AZ::BehaviorContext* context = nullptr;
                AZ::ComponentApplicationBus::BroadcastResult(context, &AZ::ComponentApplicationRequests::GetBehaviorContext);
                bool found = false;
                if (context)
                {
                    Bindings::Visit(*context, [&](const BindingView& binding)
                    {
                        if (binding.m_id != args["binding"].GetString()) { return true; }
                        found = true;
                        if (tool == "binding_describe") { Describe(binding, data, allocator); }
                        else { Execute(binding, tool, args, data, allocator); }
                        return false;
                    });
                }
                if (!found) { data.AddMember("error", "Binding no longer exists in this editor context", allocator); }
            }
            catch (const std::exception& error)
            {
                if (!data.HasMember("nativeCallCompleted")) { data.SetObject(); }
                data.AddMember("error", String(error.what(), allocator), allocator);
            }
        });
        return Result::Complete;
    }

    void BehaviorBridge::Execute(const BindingView& binding, AZStd::string_view tool, const Json& args,
        Json& data, Allocator& allocator)
    {
        using namespace EditorPythonBindings;
        Json metadata = Metadata(binding, allocator);
        if (!metadata["callable"].GetBool())
        {
            data.AddMember("error", Json(metadata["reason"], allocator), allocator);
            return;
        }
        if (binding.m_kind == Kind::Class)
        {
            if (tool != "binding_call" || !binding.m_class)
            {
                data.AddMember("error", "Use binding_call to construct a reflected class", allocator);
                return;
            }
            if (!m_values.CanStore()) { throw BridgeError("Release object handles before constructing another object"); }
            data.AddMember("value", m_values.Store(m_values.Construct(*binding.m_class, args), allocator), allocator);
            return;
        }
        AZ::BehaviorMethod* first = nullptr;
        const bool setter = tool == "binding_set";
        const bool getter = tool == "binding_get";
        size_t count = 0;
        if (setter || getter)
        {
            if (!binding.m_property) { throw BridgeError("This binding is not a reflected property"); }
            first = setter ? binding.m_property->m_setter : binding.m_property->m_getter;
            if (!first) { throw BridgeError(setter ? "Property is read-only" : "Property is write-only"); }
            count = setter ? 1 : 0;
        }
        else
        {
            count = args["args"].Size();
            first = binding.m_method;
            if (binding.m_sender)
            {
                first = args.HasMember("address") ? binding.m_sender->m_event : binding.m_sender->m_broadcast;
            }
        }
        if (!first) { throw BridgeError("No synchronous reflected sender for this request"); }
        const bool addressed = binding.m_sender && args.HasMember("address");
        AZ::BehaviorMethod* selected = nullptr;
        size_t index = 0;
        for (auto* method = first; method; method = method->m_overload, ++index)
        {
            if (args.HasMember("overload") && index != args["overload"].GetUint()) { continue; }
            const bool member = binding.m_class && PythonProxyObjectManagement::IsMemberLike(*method, binding.m_class->m_typeId);
            const size_t supplied = count + (member ? 1 : 0) + (addressed ? 1 : 0);
            if (supplied >= method->GetMinNumberOfArguments() && supplied <= method->GetNumArguments())
            {
                if (selected) { throw BridgeError("Conversion may be ambiguous; select an overload from binding_describe"); }
                selected = method;
            }
        }
        if (!selected) { throw BridgeError("No selected overload accepts that argument count"); }
        const bool member = binding.m_class && PythonProxyObjectManagement::IsMemberLike(*selected, binding.m_class->m_typeId);
        for (size_t argumentIndex = 0; argumentIndex < selected->GetNumArguments(); ++argumentIndex)
        {
            const auto reason = BridgeValues::Unsupported(*selected->GetArgument(argumentIndex));
            if (!reason.empty()) { throw BridgeError(reason.c_str()); }
        }
        if (const auto* result = selected->GetResult())
        {
            const auto reason = BridgeValues::Unsupported(*result);
            if (!reason.empty()) { throw BridgeError(reason.c_str()); }
        }
        pybind11::object target = pybind11::none();
        if (member)
        {
            if (!args.HasMember("target") || !args["target"].IsObject() || !IsString(args["target"], "$handle", 128))
            {
                throw BridgeError("Instance members require a live object handle target");
            }
            target = m_values.Decode(args["target"], selected->GetArgument(0));
        }
        else if (args.HasMember("target")) { throw BridgeError("Static/global calls do not accept an instance target"); }
        pybind11::tuple inputs(count + (addressed ? 1 : 0));
        if (addressed) { inputs[0] = m_values.Decode(args["address"], selected->GetArgument(0)); }
        for (size_t argumentIndex = 0; argumentIndex < count; ++argumentIndex)
        {
            const size_t inputIndex = argumentIndex + (addressed ? 1 : 0);
            const Json& value = setter ? args["value"] : args["args"][static_cast<rapidjson::SizeType>(argumentIndex)];
            inputs[inputIndex] = m_values.Decode(value, selected->GetArgument(inputIndex + (member ? 1 : 0)));
        }
        bool didInvoke = false;
        auto result = Call::StructuredMethod(selected, target, pybind11::reinterpret_borrow<pybind11::args>(inputs), &didInvoke);
        if (didInvoke) { data.AddMember("nativeCallCompleted", true, allocator); }
        if (!result) { throw BridgeError(result.GetError().c_str()); }
        data.AddMember("value", m_values.Encode(result.TakeValue(), selected->GetResult(), allocator), allocator);
    }

    void BehaviorBridge::AddTools(Json& tools, Allocator& allocator)
    {
        rapidjson::Document schemas;
        schemas.Parse(R"MCP([{"name":"binding_search","description":"Ranked reflected binding discovery, including unavailable entries and their reasons.","inputSchema":{"type":"object","properties":{"query":{"type":"string","maxLength":512},"kind":{"type":"string","enum":["class","method","property","bus","event","bus_property"]},"limit":{"type":"integer","minimum":1,"maximum":100},"after":{"type":"integer","minimum":0}},"required":["query"]}},{"name":"binding_describe","description":"Describe native signatures, defaults, types, scope and synchronous senders.","inputSchema":{"type":"object","properties":{"binding":{"type":"string","minLength":1,"maxLength":2048}},"required":["binding"]}},{"name":"binding_call","description":"Invoke one reflected overload or construct a class. Native calls are not generally undoable; failures report whether a call completed.","inputSchema":{"type":"object","properties":{"binding":{"type":"string","minLength":1,"maxLength":2048},"overload":{"type":"integer","minimum":0},"target":{"type":"object","properties":{"$handle":{"type":"string","maxLength":128}},"required":["$handle"]},"address":{},"args":{"type":"array","maxItems":32}},"required":["binding","args"]}},{"name":"binding_get","description":"Read a reflected property.","inputSchema":{"type":"object","properties":{"binding":{"type":"string","minLength":1,"maxLength":2048},"target":{"type":"object","properties":{"$handle":{"type":"string","maxLength":128}},"required":["$handle"]}},"required":["binding"]}},{"name":"binding_set","description":"Set a reflected property. General binding writes do not supply editor undo semantics.","inputSchema":{"type":"object","properties":{"binding":{"type":"string","minLength":1,"maxLength":2048},"target":{"type":"object","properties":{"$handle":{"type":"string","maxLength":128}},"required":["$handle"]},"value":{}},"required":["binding","value"]}},{"name":"binding_release","description":"Release process-local object handles; context/owner changes also invalidate them.","inputSchema":{"type":"object","properties":{"handles":{"type":"array","maxItems":256,"items":{"type":"string","maxLength":128}}},"required":["handles"]}}])MCP");
        for (const auto& schema : schemas.GetArray())
        {
            tools.PushBack(Json(schema, allocator), allocator);
        }
    }

}
