/*
 * Copyright (c) Contributors to the Open 3D Engine Project.
 * For complete copyright and license terms please see the LICENSE at the root of this distribution.
 *
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 *
 */

#include "BehaviorBridge.h"

#include "LocalMcpSearch.h"

#include <AzCore/Component/ComponentApplicationBus.h>
#include <AzCore/Script/ScriptContextAttributes.h>
#include <AzCore/std/any.h>
#include <AzCore/std/containers/unordered_map.h>
#include <AzCore/std/sort.h>
#include <AzToolsFramework/API/EditorPythonConsoleBus.h>
#include <pybind11/embed.h>

namespace LocalMcp
{
    namespace
    {
        //! binding_search and binding_browse page size when the request gives no limit.
        constexpr size_t DefaultPageLimit = 20;

        //! Python module that registers the reflected object types the marshaller converts to.
        constexpr const char* ObjectModule = "azlmbr.object";

        AZ::BehaviorContext* GetBehaviorContext()
        {
            AZ::BehaviorContext* context = nullptr;
            AZ::ComponentApplicationBus::BroadcastResult(context, &AZ::ComponentApplicationRequests::GetBehaviorContext);
            return context;
        }

        //! The Python interpreter, or nullptr when it is not running.
        AzToolsFramework::EditorPythonEventsInterface* GetActivePython()
        {
            auto* python = AZ::Interface<AzToolsFramework::EditorPythonEventsInterface>::Get();
            return python && python->IsPythonActive() ? python : nullptr;
        }

        //! Imports ObjectModule; the caller holds the Python lock.
        BridgeResult<void> ImportObjectModule()
        {
            return TryPython(
                []()
                {
                    pybind11::module::import(ObjectModule);
                });
        }
    } // namespace

    void BehaviorBridge::Activate()
    {
        m_mainThread = AZStd::this_thread::get_id();
        m_values.Activate();

        EditorPythonBindings::EditorPythonBindingsNotificationBus::Handler::BusConnect();
        AzToolsFramework::EditorEntityContextNotificationBus::Handler::BusConnect();
        AzToolsFramework::EntityCompositionNotificationBus::Handler::BusConnect();

        if (AZ::BehaviorContext* context = GetBehaviorContext())
        {
            AZ::BehaviorContextBus::Handler::BusConnect(context);
        }
    }

    void BehaviorBridge::Deactivate()
    {
        ClearHandles();

        AZ::BehaviorContextBus::Handler::BusDisconnect();
        AzToolsFramework::EntityCompositionNotificationBus::Handler::BusDisconnect();
        AzToolsFramework::EditorEntityContextNotificationBus::Handler::BusDisconnect();
        EditorPythonBindings::EditorPythonBindingsNotificationBus::Handler::BusDisconnect();
    }

    void BehaviorBridge::ClearHandles()
    {
        if (m_values.Empty())
        {
            return;
        }

        auto* python = GetActivePython();
        AZ_Assert(python, "Owned Python objects must be released before interpreter shutdown");
        python->ExecuteWithLock(
            [this]()
            {
                m_values.Clear();
            });
    }

    void BehaviorBridge::AddTools(Json& tools, Allocator& allocator) const
    {
        rapidjson::Document schemas;
        schemas.Parse(BindingToolsSchemas);
        AZ_Assert(!schemas.HasParseError(), "Generated binding tool schemas are not valid JSON");

        for (const Json& schema : schemas["tools"].GetArray())
        {
            tools.PushBack(Json(schema, allocator), allocator);
        }
        m_modules.AddTools(tools, allocator);
    }

    AZ::u64 BehaviorBridge::GetToolsRevision() const
    {
        return m_modules.GetRevision();
    }

    ToolProvider::Result BehaviorBridge::Invoke(
        AZStd::string_view tool, const Json& args, Json& data, Allocator& allocator, AZStd::string& detail)
    {
        ToolCall call{ data, allocator };
        ToolProvider::Result result = Dispatch(tool, args, call);
        if (result == ToolProvider::Result::Unknown)
        {
            if (const ModuleTool* typed = m_modules.Find(tool))
            {
                result = InvokeModuleTool(*typed, args, call);
            }
        }
        detail = AZStd::move(call.m_error);
        return result;
    }

    ToolProvider::Result BehaviorBridge::InvokeModuleTool(const ModuleTool& tool, const Json& args, ToolCall& call)
    {
        if (!args.IsObject())
        {
            call.m_error = "arguments: must be an object";
            return ToolProvider::Result::InvalidArguments;
        }

        // The request outlives the dispatch, whose parsed arguments point into it.
        rapidjson::Document request;
        auto built = ModuleTools::BuildCall(tool, args, request, request.GetAllocator());
        if (!built)
        {
            call.m_error = built.TakeError();
            return ToolProvider::Result::InvalidArguments;
        }
        return Dispatch(BindingCallTool, request, call);
    }

    BridgeResult<Json> BehaviorBridge::NormalizeValue(const Json& value, const AZ::TypeId& type, Allocator& allocator)
    {
        if (AZStd::this_thread::get_id() != m_mainThread)
        {
            return AZ::Failure(AZStd::string("Binding conversion requires the editor main thread"));
        }

        auto* python = GetActivePython();
        if (!python)
        {
            return AZ::Failure(AZStd::string("EditorPythonBindings interpreter is not active"));
        }

        BridgeResult<Json> result = AZ::Success(Json());
        python->ExecuteWithLock(
            [&]()
            {
                auto imported = ImportObjectModule();
                if (!imported)
                {
                    result = AZ::Failure(imported.TakeError());
                    return;
                }

                AZ::BehaviorParameter expected{};
                expected.m_typeId = type;
                auto decoded = m_values.Decode(value, &expected);
                if (!decoded)
                {
                    result = AZ::Failure(decoded.TakeError());
                    return;
                }
                result = m_values.Encode(decoded.TakeValue(), &expected, allocator);
            });
        return result;
    }

    Json BehaviorBridge::Metadata(const BindingView& binding, Allocator& allocator)
    {
        using namespace EditorPythonBindings;
        using AZ::Script::Attributes::ExcludeFlags;
        using AZ::Script::Attributes::ScopeFlags;

        Json result(rapidjson::kObjectType);
        result.AddMember("binding", String(binding.m_id, allocator), allocator);
        result.AddMember("kind", String(Bindings::ToString(binding.m_kind), allocator), allocator);
        result.AddMember("name", String(binding.m_name, allocator), allocator);

        AZStd::string alias(binding.m_name);
        ExcludeFlags exclude = static_cast<ExcludeFlags>(0);
        if (binding.m_attributes)
        {
            Scope::FetchScriptName(*binding.m_attributes, alias);
            const auto documentation = Text::ReadStringAttribute(*binding.m_attributes, AZ::Script::Attributes::ToolTip);
            result.AddMember("documentation", String(documentation, allocator), allocator);
            AZ::AttributeReader(nullptr, AZ::FindAttribute(AZ::Script::Attributes::ExcludeFrom, *binding.m_attributes))
                .Read<ExcludeFlags>(exclude);
        }
        result.AddMember("alias", String(alias, allocator), allocator);
        result.AddMember("excludeFlags", static_cast<AZ::u64>(exclude), allocator);

        if (binding.m_attributes)
        {
            if (auto module = Module::GetName(*binding.m_attributes))
            {
                result.AddMember("declaredModule", String(*module, allocator), allocator);
            }

            ScopeFlags declaredScope;
            if (AZ::AttributeReader(nullptr, AZ::FindAttribute(AZ::Script::Attributes::Scope, *binding.m_attributes))
                    .Read<ScopeFlags>(declaredScope))
            {
                result.AddMember("declaredScope", static_cast<AZ::u64>(declaredScope), allocator);
            }
        }

        // The scope that decides whether editor automation may call the entry is the owner's.
        ScopeFlags scope = ScopeFlags::Launcher;
        AZStd::string reason;
        if (binding.m_scope)
        {
            AZ::AttributeReader(nullptr, AZ::FindAttribute(AZ::Script::Attributes::Scope, *binding.m_scope)).Read<ScopeFlags>(scope);
            if (auto module = Module::GetName(*binding.m_scope))
            {
                result.AddMember("module", String(*module, allocator), allocator);
            }
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

        const bool synchronous = binding.m_sender && (binding.m_sender->m_broadcast || binding.m_sender->m_event);
        if (reason.empty() && binding.m_kind == Kind::Event && !synchronous)
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
            if (!parameter)
            {
                return info;
            }

            const bool nullable = parameter->m_typeId == azrtti_typeid<void>() || parameter->m_typeId == azrtti_typeid<AZStd::any>();
            info.AddMember("type", String(parameter->m_typeId.ToString<AZStd::string>(), allocator), allocator);
            info.AddMember("nativeTypeName", String(parameter->m_name ? parameter->m_name : "", allocator), allocator);
            info.AddMember("traits", parameter->m_traits, allocator);
            info.AddMember("pythonType", String(text.FetchPythonTypeName(*parameter), allocator), allocator);
            info.AddMember("nativeNullable", (parameter->m_traits & AZ::BehaviorParameter::TR_POINTER) != 0, allocator);
            info.AddMember("nullable", nullable, allocator);
            info.AddMember("unsupportedReason", String(BridgeValues::Unsupported(*parameter), allocator), allocator);
            return info;
        };

        // One entry per overload of first, with each argument's name, documentation and default value.
        auto methods = [&](AZ::BehaviorMethod* first)
        {
            Json overloads(rapidjson::kArrayType);
            size_t index = 0;
            for (auto* method = first; method; method = method->m_overload, ++index)
            {
                Json entry(rapidjson::kObjectType);
                entry.AddMember("overload", static_cast<uint64_t>(index), allocator);
                const auto signature = text.MethodDefinition(binding.m_name, *method, binding.m_class, true, true);
                entry.AddMember("signature", String(signature, allocator), allocator);
                entry.AddMember("minArguments", static_cast<uint64_t>(method->GetMinNumberOfArguments()), allocator);

                Json arguments(rapidjson::kArrayType);
                for (size_t argumentIndex = 0; argumentIndex < method->GetNumArguments(); ++argumentIndex)
                {
                    Json argument = parameterInfo(method->GetArgument(argumentIndex));
                    const auto* name = method->GetArgumentName(argumentIndex);
                    const auto* help = method->GetArgumentToolTip(argumentIndex);
                    argument.AddMember("name", String(name ? AZStd::string_view(*name) : AZStd::string_view{}, allocator), allocator);
                    argument.AddMember(
                        "documentation", String(help ? AZStd::string_view(*help) : AZStd::string_view{}, allocator), allocator);

                    const auto defaultValue = method->GetDefaultValue(argumentIndex);
                    argument.AddMember("hasDefault", static_cast<bool>(defaultValue), allocator);
                    if (defaultValue)
                    {
                        Convert::StackVariableAllocator variables;
                        AZ::BehaviorArgument nativeDefault(defaultValue->GetValue());
                        AZStd::string error;
                        auto value = Convert::BehaviorValueParameterToPython(nativeDefault, variables, &error);
                        if (error.empty())
                        {
                            auto encoded = m_values.Encode(value, method->GetArgument(argumentIndex), allocator);
                            if (encoded)
                            {
                                argument.AddMember("default", encoded.GetValue(), allocator);
                            }
                            else
                            {
                                error = encoded.TakeError();
                            }
                        }
                        if (!error.empty())
                        {
                            argument.AddMember("defaultError", String(error, allocator), allocator);
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
            for (const auto& method : binding.m_class->m_methods)
            {
                members.PushBack(String(binding.m_id + "/method/" + Bindings::Escape(method.first), allocator), allocator);
            }
            for (const auto& property : binding.m_class->m_properties)
            {
                members.PushBack(String(binding.m_id + "/property/" + Bindings::Escape(property.first), allocator), allocator);
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
                for (const auto& event : binding.m_bus->m_events)
                {
                    members.PushBack(String(binding.m_id + "/event/" + Bindings::Escape(event.first), allocator), allocator);
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
            const AZStd::string busId = "bus/" + Bindings::Escape(binding.m_bus->m_name);
            Json accessors(rapidjson::kArrayType);
            for (const auto& [name, sender] : binding.m_bus->m_events)
            {
                if (&sender == binding.m_busProperty->m_getter || &sender == binding.m_busProperty->m_setter)
                {
                    accessors.PushBack(String(busId + "/event/" + Bindings::Escape(name), allocator), allocator);
                }
            }
            data.AddMember("accessors", accessors, allocator);
        }
    }

    BridgeResult<void> BehaviorBridge::Execute(const BindingView& binding, const Request& request, Json& data, Allocator& allocator)
    {
        using namespace EditorPythonBindings;

        Json metadata = Metadata(binding, allocator);
        if (!metadata["callable"].GetBool())
        {
            return AZ::Failure(AZStd::string(metadata["reason"].GetString(), metadata["reason"].GetStringLength()));
        }

        if (binding.m_kind == Kind::Class)
        {
            if (request.m_access != Access::Call || !binding.m_class)
            {
                return AZ::Failure(AZStd::string("Use binding_call to construct a reflected class"));
            }
            if (!m_values.CanStore())
            {
                return AZ::Failure(AZStd::string("Release object handles before constructing another object"));
            }

            auto constructed = m_values.Construct(*binding.m_class, *request.m_args, request.m_overload);
            if (!constructed)
            {
                return AZ::Failure(constructed.TakeError());
            }
            auto value = m_values.Store(constructed.TakeValue(), allocator);
            if (!value)
            {
                return AZ::Failure(value.TakeError());
            }
            data.AddMember("value", value.GetValue(), allocator);
            return AZ::Success();
        }

        // The first overload of the native method to call, and how many values the request supplies.
        const bool setter = request.m_access == Access::Set;
        AZ::BehaviorMethod* first = nullptr;
        size_t count = 0;
        if (request.m_access == Access::Call)
        {
            count = request.m_args->Size();
            first = binding.m_method;
            if (binding.m_sender)
            {
                first = request.m_address ? binding.m_sender->m_event : binding.m_sender->m_broadcast;
            }
        }
        else
        {
            if (!binding.m_property)
            {
                return AZ::Failure(AZStd::string("This binding is not a reflected property"));
            }
            first = setter ? binding.m_property->m_setter : binding.m_property->m_getter;
            if (!first)
            {
                return AZ::Failure(AZStd::string(setter ? "Property is read-only" : "Property is write-only"));
            }
            count = setter ? 1 : 0;
        }
        if (!first)
        {
            return AZ::Failure(AZStd::string("No synchronous reflected sender for this request"));
        }

        // An instance member takes the target first, and an addressed event takes the address before the arguments.
        const bool addressed = binding.m_sender && request.m_address;
        auto isMember = [&](const AZ::BehaviorMethod& method)
        {
            return binding.m_class && PythonProxyObjectManagement::IsMemberLike(method, binding.m_class->m_typeId);
        };

        AZ::BehaviorMethod* selected = nullptr;
        size_t index = 0;
        for (auto* method = first; method; method = method->m_overload, ++index)
        {
            if (request.m_overload && index != *request.m_overload)
            {
                continue;
            }

            const size_t supplied = count + (isMember(*method) ? 1 : 0) + (addressed ? 1 : 0);
            if (supplied < method->GetMinNumberOfArguments() || supplied > method->GetNumArguments())
            {
                continue;
            }
            if (selected)
            {
                return AZ::Failure(AZStd::string("Conversion may be ambiguous; select an overload from binding_describe"));
            }
            selected = method;
        }
        if (!selected)
        {
            return AZ::Failure(AZStd::string("No selected overload accepts that argument count"));
        }

        for (size_t argumentIndex = 0; argumentIndex < selected->GetNumArguments(); ++argumentIndex)
        {
            AZStd::string reason = BridgeValues::Unsupported(*selected->GetArgument(argumentIndex));
            if (!reason.empty())
            {
                return AZ::Failure(AZStd::move(reason));
            }
        }
        if (const AZ::BehaviorParameter* result = selected->GetResult())
        {
            AZStd::string reason = BridgeValues::Unsupported(*result);
            if (!reason.empty())
            {
                return AZ::Failure(AZStd::move(reason));
            }
        }

        const bool member = isMember(*selected);
        pybind11::object target = pybind11::none();
        if (member)
        {
            if (!request.m_target)
            {
                return AZ::Failure(AZStd::string("Instance members require a live object handle target"));
            }
            auto decoded = m_values.Decode(*request.m_target, selected->GetArgument(0));
            if (!decoded)
            {
                return AZ::Failure(decoded.TakeError());
            }
            target = decoded.TakeValue();
        }
        else if (request.m_target)
        {
            return AZ::Failure(AZStd::string("Static/global calls do not accept an instance target"));
        }

        const size_t firstArgument = addressed ? 1 : 0;
        pybind11::tuple inputs(count + firstArgument);
        if (addressed)
        {
            auto decoded = m_values.Decode(*request.m_address, selected->GetArgument(0));
            if (!decoded)
            {
                return AZ::Failure(decoded.TakeError());
            }
            inputs[0] = decoded.TakeValue();
        }
        for (size_t argumentIndex = 0; argumentIndex < count; ++argumentIndex)
        {
            const size_t inputIndex = argumentIndex + firstArgument;
            const Json& value = setter ? *request.m_value : (*request.m_args)[aznumeric_cast<rapidjson::SizeType>(argumentIndex)];
            auto decoded = m_values.Decode(value, selected->GetArgument(inputIndex + (member ? 1 : 0)));
            if (!decoded)
            {
                return AZ::Failure(decoded.TakeError());
            }
            inputs[inputIndex] = decoded.TakeValue();
        }

        bool didInvoke = false;
        auto result = Call::StructuredMethod(selected, target, pybind11::reinterpret_borrow<pybind11::args>(inputs), &didInvoke);
        if (didInvoke)
        {
            data.AddMember("nativeCallCompleted", true, allocator);
        }
        if (!result)
        {
            return AZ::Failure(result.TakeError());
        }

        auto value = m_values.Encode(result.TakeValue(), selected->GetResult(), allocator);
        if (!value)
        {
            return AZ::Failure(value.TakeError());
        }
        data.AddMember("value", value.GetValue(), allocator);
        return AZ::Success();
    }

    bool BehaviorBridge::OnMainThread(ToolCall& call) const
    {
        if (AZStd::this_thread::get_id() == m_mainThread)
        {
            return true;
        }

        call.m_data.AddMember("error", "Binding tools require the editor main thread", call.m_allocator);
        return false;
    }

    void BehaviorBridge::WithPython(ToolCall& call, const AZStd::function<BridgeResult<void>()>& work)
    {
        auto* python = GetActivePython();
        if (!python)
        {
            call.m_data.AddMember("error", "EditorPythonBindings interpreter is not active", call.m_allocator);
            return;
        }

        python->ExecuteWithLock(
            [&]()
            {
                BridgeResult<void> result = ImportObjectModule();
                if (result)
                {
                    result = work();
                }
                if (!result)
                {
                    // A native call that completed is reported along with the failure that followed it.
                    if (!call.m_data.HasMember("nativeCallCompleted"))
                    {
                        call.m_data.SetObject();
                    }
                    call.m_data.AddMember("error", String(result.GetError(), call.m_allocator), call.m_allocator);
                }
            });
    }

    void BehaviorBridge::WithBinding(
        AZStd::string_view id, ToolCall& call, const AZStd::function<BridgeResult<void>(const BindingView&)>& work)
    {
        WithPython(
            call,
            [&]() -> BridgeResult<void>
            {
                BridgeResult<void> result = AZ::Success();
                bool found = false;
                if (AZ::BehaviorContext* context = GetBehaviorContext())
                {
                    Bindings::Visit(
                        *context,
                        [&](const BindingView& binding)
                        {
                            if (AZStd::string_view(binding.m_id) != id)
                            {
                                return true;
                            }
                            found = true;
                            result = work(binding);
                            return false;
                        });
                }

                if (!found)
                {
                    call.m_data.AddMember("error", "Binding no longer exists in this editor context", call.m_allocator);
                }
                return result;
            });
    }

    void BehaviorBridge::RunRequest(AZStd::string_view id, const Request& request, ToolCall& call)
    {
        if (!OnMainThread(call))
        {
            return;
        }

        WithBinding(
            id,
            call,
            [&](const BindingView& binding)
            {
                return Execute(binding, request, call.m_data, call.m_allocator);
            });
    }

    bool BehaviorBridge::HandleBindingSearch(const BindingSearchArgs& args, ToolCall& call)
    {
        if (!OnMainThread(call))
        {
            return true;
        }

        // The schema enum lists every Kind name, so a kind that passed Parse() always converts.
        AZStd::optional<Kind> kind;
        if (args.m_kind)
        {
            kind = Bindings::FromStringToKind(*args.m_kind);
            if (!kind)
            {
                call.m_error = "kind: unknown binding kind";
                return false;
            }
        }

        AZ::BehaviorContext* context = GetBehaviorContext();
        if (!context)
        {
            call.m_data.AddMember("error", "Application has no BehaviorContext", call.m_allocator);
            return true;
        }

        // Rank every binding by its ID, name and script alias against the query.
        struct Match
        {
            int m_score;
            AZStd::string m_id;
        };
        AZStd::vector<Match> matches;
        const SearchTerms terms(args.m_query);
        Bindings::Visit(
            *context,
            [&](const BindingView& binding)
            {
                if (kind && *kind != binding.m_kind)
                {
                    return true;
                }
                if (args.m_module && !Bindings::InModule(Bindings::GetModule(binding), *args.m_module))
                {
                    return true;
                }

                AZStd::string alias(binding.m_name);
                AZStd::string help;
                if (binding.m_attributes)
                {
                    EditorPythonBindings::Scope::FetchScriptName(*binding.m_attributes, alias);
                    help = EditorPythonBindings::Text::ReadStringAttribute(*binding.m_attributes, AZ::Script::Attributes::ToolTip);
                }

                const int nameScore = AZStd::max(terms.Rank(binding.m_id, help), terms.Rank(binding.m_name, help));
                const int score = AZStd::max(nameScore, terms.Rank(alias, help));
                if (score > SearchTerms::NoMatch)
                {
                    matches.push_back({ score, binding.m_id });
                }
                return true;
            });

        AZStd::sort(
            matches.begin(),
            matches.end(),
            [](const Match& left, const Match& right)
            {
                if (left.m_score != right.m_score)
                {
                    return left.m_score > right.m_score;
                }
                return left.m_id < right.m_id;
            });

        // Reserve the page's slots, then fill them with metadata in a second walk.
        const size_t after = args.m_after ? aznumeric_cast<size_t>(*args.m_after) : 0;
        const size_t limit = args.m_limit ? aznumeric_cast<size_t>(*args.m_limit) : DefaultPageLimit;
        size_t next = AZStd::min(after, matches.size());
        Json list(rapidjson::kArrayType);
        AZStd::unordered_map<AZStd::string, rapidjson::SizeType> page;
        for (; next < matches.size() && list.Size() < limit; ++next)
        {
            page.emplace(matches[next].m_id, list.Size());
            list.PushBack(Json(), call.m_allocator);
        }

        Bindings::Visit(
            *context,
            [&](const BindingView& binding)
            {
                const auto found = page.find(binding.m_id);
                if (found != page.end())
                {
                    list[found->second] = Metadata(binding, call.m_allocator);
                }
                return true;
            });

        call.m_data.AddMember("matches", list, call.m_allocator);
        call.m_data.AddMember("next", static_cast<uint64_t>(next), call.m_allocator);
        call.m_data.AddMember("total", static_cast<uint64_t>(matches.size()), call.m_allocator);
        return true;
    }

    bool BehaviorBridge::HandleBindingBrowse(const BindingBrowseArgs& args, ToolCall& call)
    {
        if (!OnMainThread(call))
        {
            return true;
        }

        AZ::BehaviorContext* context = GetBehaviorContext();
        if (!context)
        {
            call.m_data.AddMember("error", "Application has no BehaviorContext", call.m_allocator);
            return true;
        }

        const AZStd::string_view path = args.m_path ? AZStd::string_view(*args.m_path) : AZStd::string_view();
        const size_t after = args.m_after ? aznumeric_cast<size_t>(*args.m_after) : 0;
        const size_t limit = args.m_limit ? aznumeric_cast<size_t>(*args.m_limit) : DefaultPageLimit;
        auto browsed = Bindings::Browse(*context, path, after, limit, call.m_data, call.m_allocator);
        if (!browsed)
        {
            call.m_data.SetObject();
            call.m_data.AddMember("error", String(browsed.GetError(), call.m_allocator), call.m_allocator);
        }
        return true;
    }

    bool BehaviorBridge::HandleBindingOpenModule(const BindingOpenModuleArgs& args, ToolCall& call)
    {
        if (!OnMainThread(call))
        {
            return true;
        }

        AZ::BehaviorContext* context = GetBehaviorContext();
        if (!context)
        {
            call.m_data.AddMember("error", "Application has no BehaviorContext", call.m_allocator);
            return true;
        }

        auto opened = m_modules.Open(*context, args.m_module, call.m_allocator);
        if (!opened)
        {
            call.m_data.AddMember("error", String(opened.GetError(), call.m_allocator), call.m_allocator);
            return true;
        }
        call.m_data = opened.TakeValue();
        return true;
    }

    bool BehaviorBridge::HandleBindingCloseModule(const BindingCloseModuleArgs& args, ToolCall& call)
    {
        if (!OnMainThread(call))
        {
            return true;
        }

        auto closed = m_modules.Close(args.m_module, call.m_allocator);
        if (!closed)
        {
            call.m_data.AddMember("error", String(closed.GetError(), call.m_allocator), call.m_allocator);
            return true;
        }
        call.m_data = closed.TakeValue();
        return true;
    }

    bool BehaviorBridge::HandleBindingDescribe(const BindingDescribeArgs& args, ToolCall& call)
    {
        if (!OnMainThread(call))
        {
            return true;
        }

        WithBinding(
            args.m_binding,
            call,
            [&](const BindingView& binding) -> BridgeResult<void>
            {
                Describe(binding, call.m_data, call.m_allocator);
                return AZ::Success();
            });
        return true;
    }

    bool BehaviorBridge::HandleBindingCall(const BindingCallArgs& args, ToolCall& call)
    {
        Request request;
        request.m_access = Access::Call;
        request.m_args = args.m_args;
        request.m_target = args.m_target;
        request.m_address = args.m_address;
        if (args.m_overload)
        {
            request.m_overload = aznumeric_cast<size_t>(*args.m_overload);
        }

        RunRequest(args.m_binding, request, call);
        return true;
    }

    bool BehaviorBridge::HandleBindingGet(const BindingGetArgs& args, ToolCall& call)
    {
        Request request;
        request.m_access = Access::Get;
        request.m_target = args.m_target;
        if (args.m_overload)
        {
            request.m_overload = aznumeric_cast<size_t>(*args.m_overload);
        }

        RunRequest(args.m_binding, request, call);
        return true;
    }

    bool BehaviorBridge::HandleBindingSet(const BindingSetArgs& args, ToolCall& call)
    {
        Request request;
        request.m_access = Access::Set;
        request.m_target = args.m_target;
        request.m_value = args.m_value;
        if (args.m_overload)
        {
            request.m_overload = aznumeric_cast<size_t>(*args.m_overload);
        }

        RunRequest(args.m_binding, request, call);
        return true;
    }

    bool BehaviorBridge::HandleBindingRelease(const BindingReleaseArgs& args, ToolCall& call)
    {
        if (!OnMainThread(call))
        {
            return true;
        }

        WithPython(
            call,
            [&]() -> BridgeResult<void>
            {
                auto released = m_values.Release(args.m_handles);
                if (!released)
                {
                    return AZ::Failure(released.TakeError());
                }
                call.m_data.AddMember("released", static_cast<uint64_t>(released.GetValue()), call.m_allocator);
                return AZ::Success();
            });
        return true;
    }

    void BehaviorBridge::OnPreFinalize()
    {
        ClearHandles();
    }

    void BehaviorBridge::OnEntityCompositionChanging([[maybe_unused]] const AzToolsFramework::EntityIdList& entities)
    {
        ClearHandles();
    }

    void BehaviorBridge::OnRemoveClass([[maybe_unused]] const char* className, [[maybe_unused]] AZ::BehaviorClass* behaviorClass)
    {
        ClearHandles();
    }

    void BehaviorBridge::OnPrepareForContextReset()
    {
        ClearHandles();
    }

    void BehaviorBridge::OnEditorEntityDeleted([[maybe_unused]] const AZ::EntityId& entity)
    {
        ClearHandles();
    }
} // namespace LocalMcp
