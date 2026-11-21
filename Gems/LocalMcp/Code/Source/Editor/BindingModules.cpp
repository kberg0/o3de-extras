/*
 * Copyright (c) Contributors to the Open 3D Engine Project.
 * For complete copyright and license terms please see the LICENSE at the root of this distribution.
 *
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 *
 */

#include "BindingModules.h"

#include <AzCore/Math/Crc.h>
#include <AzCore/Script/ScriptContextAttributes.h>
#include <AzCore/StringFunc/StringFunc.h>
#include <AzCore/std/algorithm.h>
#include <AzCore/std/containers/map.h>
#include <AzCore/std/containers/unordered_set.h>
#include <AzCore/std/optional.h>
#include <AzCore/std/sort.h>

namespace LocalMcp::Bindings
{
    namespace
    {
        // Modules that azlmbr gives bindings whose owner has no Module attribute.
        constexpr const char* ClassFallbackModule = "default";
        constexpr const char* BusFallbackModule = "bus";
        constexpr const char* GlobalFallbackModule = "globals";

        //! Separates the parts of a module path and the levels of a browse path.
        constexpr char PathSeparator = '.';

        //! Kind reported for a module level of a browse path.
        constexpr AZStd::string_view ModuleKind = "Module";

        //! One level of a binding's browse path.
        struct Segment
        {
            AZStd::string_view m_name;
            AZStd::string_view m_kind;
        };

        //! The browse path of a binding: the parts of module, then the owning class or bus, then the binding itself.
        //! The segments point into module and into the BehaviorContext.
        AZStd::vector<Segment> PathSegments(const BindingView& binding, const AZStd::string& module)
        {
            AZStd::vector<Segment> segments;
            AZ::StringFunc::TokenizeVisitor(
                module,
                [&segments](AZStd::string_view part)
                {
                    segments.push_back({ part, ModuleKind });
                },
                PathSeparator);

            const bool classMember = binding.m_class && binding.m_kind != Kind::Class;
            const bool busMember = binding.m_bus && binding.m_kind != Kind::Bus;
            if (classMember)
            {
                segments.push_back({ binding.m_class->m_name, ToString(Kind::Class) });
            }
            else if (busMember)
            {
                segments.push_back({ binding.m_bus->m_name, ToString(Kind::Bus) });
            }
            segments.push_back({ binding.m_name, ToString(binding.m_kind) });
            return segments;
        }
    } // namespace

    AZStd::string GetModule(const BindingView& binding)
    {
        if (binding.m_scope)
        {
            if (auto module = EditorPythonBindings::Module::GetName(*binding.m_scope))
            {
                return AZStd::string(*module);
            }
        }

        if (binding.m_bus)
        {
            return BusFallbackModule;
        }
        return binding.m_class ? ClassFallbackModule : GlobalFallbackModule;
    }

    bool IsAutomation(const BindingView& binding)
    {
        return binding.m_scope && EditorPythonBindings::Scope::IsBehaviorFlaggedForEditor(*binding.m_scope);
    }

    bool InModule(AZStd::string_view module, AZStd::string_view filter)
    {
        if (!module.starts_with(filter))
        {
            return false;
        }
        return module.size() == filter.size() || module[filter.size()] == PathSeparator;
    }

    BridgeResult<void> Browse(
        AZ::BehaviorContext& context, AZStd::string_view path, size_t after, size_t limit, Json& data, Allocator& allocator)
    {
        //! A child of path, merged over every binding at or below it.
        struct Child
        {
            AZStd::string_view m_kind;
            AZStd::string m_binding;
            AZStd::string m_tooltip;
            AZStd::unordered_set<AZStd::string> m_children;
        };

        AZStd::map<AZStd::string, Child> children;
        bool found = path.empty();
        Visit(
            context,
            [&](const BindingView& binding)
            {
                if (!IsAutomation(binding))
                {
                    return true;
                }

                const AZStd::string module = GetModule(binding);
                const AZStd::vector<Segment> segments = PathSegments(binding, module);

                // Count the leading segments that spell path. Names may hold dots, so whole joined names are compared.
                size_t depth = 0;
                if (!path.empty())
                {
                    AZStd::string joined;
                    while (depth < segments.size() && joined.size() < path.size())
                    {
                        if (depth > 0)
                        {
                            joined += PathSeparator;
                        }
                        joined += segments[depth].m_name;
                        ++depth;
                    }
                    if (joined != path)
                    {
                        return true;
                    }
                }

                found = true;
                if (depth == segments.size())
                {
                    // path names this binding, which has no children of its own.
                    return true;
                }

                Child& child = children[AZStd::string(segments[depth].m_name)];
                const bool isThisBinding = depth + 1 == segments.size();
                if (isThisBinding)
                {
                    child.m_kind = segments[depth].m_kind;
                    child.m_binding = binding.m_id;
                    if (binding.m_attributes)
                    {
                        child.m_tooltip =
                            EditorPythonBindings::Text::ReadStringAttribute(*binding.m_attributes, AZ::Script::Attributes::ToolTip);
                    }
                }
                else
                {
                    if (child.m_kind.empty())
                    {
                        child.m_kind = segments[depth].m_kind;
                    }
                    child.m_children.emplace(segments[depth + 1].m_name);
                }
                return true;
            });

        if (!found)
        {
            return AZ::Failure(AZStd::string("No editor automation binding at this path; browse its parent for valid names"));
        }

        const size_t first = AZStd::min(after, children.size());
        Json list(rapidjson::kArrayType);
        size_t index = 0;
        for (const auto& [name, child] : children)
        {
            if (index++ < first)
            {
                continue;
            }
            if (list.Size() >= limit)
            {
                break;
            }

            AZStd::string childPath(path);
            if (!childPath.empty())
            {
                childPath += PathSeparator;
            }
            childPath += name;

            Json entry(rapidjson::kObjectType);
            entry.AddMember("name", String(name, allocator), allocator);
            entry.AddMember("path", String(childPath, allocator), allocator);
            entry.AddMember("kind", String(child.m_kind, allocator), allocator);
            entry.AddMember("childCount", static_cast<uint64_t>(child.m_children.size()), allocator);
            if (!child.m_tooltip.empty())
            {
                entry.AddMember("tooltip", String(child.m_tooltip, allocator), allocator);
            }
            if (!child.m_binding.empty())
            {
                entry.AddMember("binding", String(child.m_binding, allocator), allocator);
            }
            list.PushBack(entry, allocator);
        }

        // AddMember() moves list, so its size is read first.
        const uint64_t next = first + list.Size();
        data.AddMember("path", String(path, allocator), allocator);
        data.AddMember("children", list, allocator);
        data.AddMember("next", next, allocator);
        data.AddMember("total", static_cast<uint64_t>(children.size()), allocator);
        return AZ::Success();
    }
} // namespace LocalMcp::Bindings

namespace LocalMcp
{
    namespace
    {
        using Bindings::BindingView;
        using Bindings::Kind;

        //! Longest generated tool description; longer tooltips are cut.
        constexpr size_t MaxToolDescriptionLength = 1024;

        //! Longest native argument name used as a schema property; longer or unusual names become argN.
        constexpr size_t MaxArgumentNameLength = 64;

        //! Prefix of the property name of an argument whose native name cannot be used.
        constexpr const char* ArgumentFallbackPrefix = "arg";

        // Properties of a generated tool that are not native arguments.
        constexpr const char* TargetProperty = "target";
        constexpr const char* AddressProperty = "address";

        //! Length of the end of a hashed tool name: '-' and eight hexadecimal digits of the binding ID's CRC32.
        constexpr size_t NameHashLength = 9;

        //! True for the characters MCP allows in a tool name.
        bool IsToolNameCharacter(char character)
        {
            const bool letter = (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z');
            const bool digit = character >= '0' && character <= '9';
            return letter || digit || character == '_' || character == '.' || character == '-';
        }

        //! True when name can be used as a schema property for an argument: letters, digits and '_' only.
        bool IsArgumentName(AZStd::string_view name)
        {
            if (name.empty() || name.size() > MaxArgumentNameLength || name == TargetProperty || name == AddressProperty)
            {
                return false;
            }
            for (const char character : name)
            {
                if (!IsToolNameCharacter(character) || character == '.' || character == '-')
                {
                    return false;
                }
            }
            return true;
        }

        //! The JSON schema of one native parameter, with help as its description.
        Json ParameterSchema(const AZ::BehaviorParameter& parameter, AZStd::string_view help, Allocator& allocator)
        {
            const AZ::TypeId& type = parameter.m_typeId;
            const bool pointer = (parameter.m_traits & AZ::BehaviorParameter::TR_POINTER) != 0;
            const bool isInteger = type == azrtti_typeid<AZ::s8>() || type == azrtti_typeid<AZ::u8>() || type == azrtti_typeid<AZ::s16>() ||
                type == azrtti_typeid<AZ::u16>() || type == azrtti_typeid<AZ::s32>() || type == azrtti_typeid<AZ::u32>();
            const bool isWideInteger = type == azrtti_typeid<AZ::s64>() || type == azrtti_typeid<AZ::u64>() ||
                type == azrtti_typeid<long>() || type == azrtti_typeid<unsigned long>();
            const bool isNumber = type == azrtti_typeid<float>() || type == azrtti_typeid<double>();
            const bool isString = type == azrtti_typeid<AZStd::string>() || type == azrtti_typeid<AZStd::string_view>() ||
                (type == azrtti_typeid<char>() && pointer);

            Json schema(rapidjson::kObjectType);
            AZStd::string description(help);
            auto describe = [&description](AZStd::string_view note)
            {
                if (!description.empty())
                {
                    description += ' ';
                }
                description += note;
            };

            if (type == azrtti_typeid<bool>())
            {
                schema.AddMember("type", "boolean", allocator);
            }
            else if (isInteger)
            {
                schema.AddMember("type", "integer", allocator);
            }
            else if (isWideInteger)
            {
                describe(R"(64-bit integer; beyond 2^53 pass {"$type":"int64" or "uint64","value":"<decimal>"}.)");
            }
            else if (isNumber)
            {
                schema.AddMember("type", "number", allocator);
            }
            else if (isString)
            {
                schema.AddMember("type", "string", allocator);
            }
            else
            {
                EditorPythonBindings::Text::PythonBehaviorDescription text;
                describe(text.FetchPythonTypeName(parameter) + " as a tagged value; binding_describe shows its form.");
            }

            if (!description.empty())
            {
                schema.AddMember("description", String(description, allocator), allocator);
            }
            return schema;
        }

        //! Describes the typed tool of a callable binding, without its name.
        //! Returns nothing for a binding that has no tool: a class, property, bus or event without a synchronous sender.
        //! Also returns nothing, with skipReason set, when a parameter cannot be converted.
        AZStd::optional<ModuleTool> DescribeTool(const BindingView& binding, AZStd::string& skipReason)
        {
            // The native method whose arguments the tool takes, and how many leading ones it passes separately.
            ModuleTool tool;
            AZ::BehaviorMethod* method = nullptr;
            size_t firstArgument = 0;
            if (binding.m_kind == Kind::Method && binding.m_method)
            {
                method = binding.m_method;
                const bool member = binding.m_class &&
                    EditorPythonBindings::PythonProxyObjectManagement::IsMemberLike(*method, binding.m_class->m_typeId);
                if (member)
                {
                    tool.m_takesTarget = true;
                    firstArgument = 1;
                }
            }
            else if (binding.m_kind == Kind::Event && binding.m_sender)
            {
                // Arguments come from the broadcast sender, which has no address argument, when there is one.
                const AZ::BehaviorEBusEventSender& sender = *binding.m_sender;
                tool.m_takesAddress = sender.m_event != nullptr;
                tool.m_requiresAddress = sender.m_broadcast == nullptr;
                method = sender.m_broadcast ? sender.m_broadcast : sender.m_event;
                firstArgument = sender.m_broadcast ? 0 : 1;
            }
            if (!method)
            {
                return AZStd::nullopt;
            }

            for (size_t index = 0; index < method->GetNumArguments(); ++index)
            {
                skipReason = BridgeValues::Unsupported(*method->GetArgument(index));
                if (!skipReason.empty())
                {
                    return AZStd::nullopt;
                }
            }
            if (const AZ::BehaviorParameter* result = method->GetResult())
            {
                skipReason = BridgeValues::Unsupported(*result);
                if (!skipReason.empty())
                {
                    return AZStd::nullopt;
                }
            }

            rapidjson::Document schema(rapidjson::kObjectType);
            Allocator& allocator = schema.GetAllocator();
            Json properties(rapidjson::kObjectType);
            Json required(rapidjson::kArrayType);
            const size_t minimum = method->GetMinNumberOfArguments();
            tool.m_requiredArguments = minimum > firstArgument ? minimum - firstArgument : 0;

            for (size_t index = firstArgument; index < method->GetNumArguments(); ++index)
            {
                const AZStd::string* nativeName = method->GetArgumentName(index);
                AZStd::string name = nativeName ? *nativeName : AZStd::string();
                const bool taken = AZStd::find(tool.m_arguments.begin(), tool.m_arguments.end(), name) != tool.m_arguments.end();
                if (!IsArgumentName(name) || taken)
                {
                    name = AZStd::string::format("%s%zu", ArgumentFallbackPrefix, index - firstArgument);
                }

                const AZStd::string* help = method->GetArgumentToolTip(index);
                const Json nameValue = String(name, allocator);
                properties.AddMember(
                    Json(nameValue, allocator),
                    ParameterSchema(*method->GetArgument(index), help ? AZStd::string_view(*help) : AZStd::string_view(), allocator),
                    allocator);
                if (tool.m_arguments.size() < tool.m_requiredArguments)
                {
                    required.PushBack(Json(nameValue, allocator), allocator);
                }
                tool.m_arguments.push_back(AZStd::move(name));
            }

            if (tool.m_takesTarget)
            {
                Json handle(rapidjson::kObjectType);
                handle.AddMember("type", "string", allocator);
                Json handleProperties(rapidjson::kObjectType);
                handleProperties.AddMember("$handle", handle, allocator);
                Json handleRequired(rapidjson::kArrayType);
                handleRequired.PushBack("$handle", allocator);

                Json target(rapidjson::kObjectType);
                target.AddMember("type", "object", allocator);
                target.AddMember("description", "Object handle returned by an earlier call.", allocator);
                target.AddMember("properties", handleProperties, allocator);
                target.AddMember("required", handleRequired, allocator);
                properties.AddMember(rapidjson::StringRef(TargetProperty), target, allocator);
                required.PushBack(rapidjson::StringRef(TargetProperty), allocator);
            }

            if (tool.m_takesAddress)
            {
                properties.AddMember(
                    rapidjson::StringRef(AddressProperty), ParameterSchema(binding.m_bus->m_idParam, "Bus address.", allocator), allocator);
                if (tool.m_requiresAddress)
                {
                    required.PushBack(rapidjson::StringRef(AddressProperty), allocator);
                }
            }

            schema.AddMember("type", "object", allocator);
            schema.AddMember("properties", properties, allocator);
            if (!required.Empty())
            {
                schema.AddMember("required", required, allocator);
            }
            schema.AddMember("additionalProperties", false, allocator);

            AZStd::string description;
            if (binding.m_attributes)
            {
                description = EditorPythonBindings::Text::ReadStringAttribute(*binding.m_attributes, AZ::Script::Attributes::ToolTip);
            }
            if (!description.empty())
            {
                description += ' ';
            }
            description += "Calls binding " + binding.m_id + " as binding_call does.";
            if (description.size() > MaxToolDescriptionLength)
            {
                description.resize(MaxToolDescriptionLength);
            }

            tool.m_binding = binding.m_id;
            tool.m_description = AZStd::move(description);
            tool.m_inputSchema = Serialize(schema);
            tool.m_overloaded = method->m_overload != nullptr;
            return tool;
        }

        //! The dotted module path of a binding that has a tool: module, owner class or bus, then the binding's name.
        AZStd::string ToolPath(const BindingView& binding, const AZStd::string& module)
        {
            AZStd::string path = module;
            if (binding.m_class)
            {
                path += '.';
                path += binding.m_class->m_name;
            }
            else if (binding.m_bus)
            {
                path += '.';
                path += binding.m_bus->m_name;
            }
            path += '.';
            path += binding.m_name;
            return path;
        }
    } // namespace

    BridgeResult<Json> ModuleTools::Open(AZ::BehaviorContext& context, AZStd::string_view module, Allocator& allocator)
    {
        for (const OpenModule& open : m_modules)
        {
            if (open.m_name == module)
            {
                return AZ::Failure(AZStd::string::format("Module %.*s is already open", AZ_STRING_ARG(module)));
            }
            if (Bindings::InModule(open.m_name, module) || Bindings::InModule(module, open.m_name))
            {
                return AZ::Failure(AZStd::string::format(
                    "Module %.*s overlaps the open module %s; close that module first", AZ_STRING_ARG(module), open.m_name.c_str()));
            }
        }

        // Collect the module's tools, ordered by path so that names stay the same from one opening to the next.
        AZStd::vector<AZStd::pair<AZStd::string, ModuleTool>> candidates;
        Json skipped(rapidjson::kArrayType);
        Bindings::Visit(
            context,
            [&](const BindingView& binding)
            {
                if (!Bindings::IsAutomation(binding))
                {
                    return true;
                }
                const AZStd::string bindingModule = Bindings::GetModule(binding);
                if (!Bindings::InModule(bindingModule, module))
                {
                    return true;
                }

                AZStd::string skipReason;
                AZStd::optional<ModuleTool> tool = DescribeTool(binding, skipReason);
                if (tool)
                {
                    candidates.emplace_back(ToolPath(binding, bindingModule), AZStd::move(*tool));
                }
                else if (!skipReason.empty())
                {
                    Json entry(rapidjson::kObjectType);
                    entry.AddMember("binding", String(binding.m_id, allocator), allocator);
                    entry.AddMember("reason", String(skipReason, allocator), allocator);
                    skipped.PushBack(entry, allocator);
                }
                return true;
            });
        AZStd::sort(
            candidates.begin(),
            candidates.end(),
            [](const auto& left, const auto& right)
            {
                return left.first < right.first;
            });

        // Name the tools. A name that another open tool already has falls back to its hashed form.
        AZStd::unordered_set<AZStd::string> used;
        for (const OpenModule& open : m_modules)
        {
            for (const ModuleTool& tool : open.m_tools)
            {
                used.insert(tool.m_name);
            }
        }

        OpenModule opened;
        opened.m_name = AZStd::string(module);
        for (auto& [path, tool] : candidates)
        {
            AZStd::string name = MakeToolName(path, tool.m_binding);
            if (used.contains(name))
            {
                name = HashedToolName(path, tool.m_binding);
            }
            if (used.contains(name))
            {
                Json entry(rapidjson::kObjectType);
                entry.AddMember("binding", String(tool.m_binding, allocator), allocator);
                entry.AddMember("reason", "Its tool name clashes with another open tool; use binding_call", allocator);
                skipped.PushBack(entry, allocator);
                continue;
            }

            used.insert(name);
            tool.m_name = AZStd::move(name);
            opened.m_tools.push_back(AZStd::move(tool));
        }

        if (opened.m_tools.empty())
        {
            return AZ::Failure(AZStd::string::format(
                "Module %.*s has no callable editor automation bindings; binding_browse lists the modules", AZ_STRING_ARG(module)));
        }

        const size_t openTools = CountOpenTools();
        if (openTools + opened.m_tools.size() > MaxOpenTools)
        {
            return AZ::Failure(AZStd::string::format(
                "Opening %.*s would add %zu tools, but at most %zu generated tools may be open and %zu already are; close a module "
                "or open one of its sub-modules",
                AZ_STRING_ARG(module),
                opened.m_tools.size(),
                MaxOpenTools,
                openTools));
        }

        Json names(rapidjson::kArrayType);
        for (const ModuleTool& tool : opened.m_tools)
        {
            names.PushBack(String(tool.m_name, allocator), allocator);
        }

        m_modules.push_back(AZStd::move(opened));
        ++m_revision;

        Json result(rapidjson::kObjectType);
        result.AddMember("module", String(module, allocator), allocator);
        result.AddMember("tools", names, allocator);
        result.AddMember("skipped", skipped, allocator);
        result.AddMember("openTools", static_cast<uint64_t>(CountOpenTools()), allocator);
        return AZ::Success(AZStd::move(result));
    }

    BridgeResult<Json> ModuleTools::Close(AZStd::string_view module, Allocator& allocator)
    {
        const auto open = AZStd::find_if(
            m_modules.begin(),
            m_modules.end(),
            [module](const OpenModule& candidate)
            {
                return candidate.m_name == module;
            });
        if (open == m_modules.end())
        {
            return AZ::Failure(AZStd::string::format("Module %.*s is not open", AZ_STRING_ARG(module)));
        }

        const size_t closed = open->m_tools.size();
        m_modules.erase(open);
        ++m_revision;

        Json result(rapidjson::kObjectType);
        result.AddMember("module", String(module, allocator), allocator);
        result.AddMember("closedTools", static_cast<uint64_t>(closed), allocator);
        result.AddMember("openTools", static_cast<uint64_t>(CountOpenTools()), allocator);
        return AZ::Success(AZStd::move(result));
    }

    void ModuleTools::AddTools(Json& tools, Allocator& allocator) const
    {
        for (const OpenModule& open : m_modules)
        {
            for (const ModuleTool& tool : open.m_tools)
            {
                rapidjson::Document schema;
                schema.Parse(tool.m_inputSchema.c_str(), tool.m_inputSchema.size());
                AZ_Assert(!schema.HasParseError(), "Generated tool schema is not valid JSON");

                Json entry(rapidjson::kObjectType);
                entry.AddMember("name", String(tool.m_name, allocator), allocator);
                entry.AddMember("description", String(tool.m_description, allocator), allocator);
                entry.AddMember("inputSchema", Json(schema, allocator), allocator);
                tools.PushBack(entry, allocator);
            }
        }
    }

    const ModuleTool* ModuleTools::Find(AZStd::string_view name) const
    {
        for (const OpenModule& open : m_modules)
        {
            for (const ModuleTool& tool : open.m_tools)
            {
                if (tool.m_name == name)
                {
                    return &tool;
                }
            }
        }
        return nullptr;
    }

    AZ::u64 ModuleTools::GetRevision() const
    {
        return m_revision;
    }

    BridgeResult<void> ModuleTools::BuildCall(const ModuleTool& tool, const Json& args, Json& request, Allocator& allocator)
    {
        for (auto member = args.MemberBegin(); member != args.MemberEnd(); ++member)
        {
            const AZStd::string_view name(member->name.GetString(), member->name.GetStringLength());
            const bool known = AZStd::find(tool.m_arguments.begin(), tool.m_arguments.end(), name) != tool.m_arguments.end() ||
                (tool.m_takesTarget && name == TargetProperty) || (tool.m_takesAddress && name == AddressProperty);
            if (!known)
            {
                return AZ::Failure(AZStd::string(name) + ": unexpected argument");
            }
        }

        // Native arguments are positional, so only trailing arguments with defaults may be left out.
        Json values(rapidjson::kArrayType);
        bool omitted = false;
        for (size_t index = 0; index < tool.m_arguments.size(); ++index)
        {
            const AZStd::string& name = tool.m_arguments[index];
            const bool present = args.HasMember(name.c_str());
            if (!present && index < tool.m_requiredArguments)
            {
                return AZ::Failure(name + ": required");
            }
            if (present && omitted)
            {
                return AZ::Failure(name + ": an earlier optional argument was left out; give it as well");
            }
            if (!present)
            {
                omitted = true;
                continue;
            }
            values.PushBack(Json(args[name.c_str()], allocator), allocator);
        }

        if (tool.m_takesTarget && !args.HasMember(TargetProperty))
        {
            return AZ::Failure(AZStd::string(TargetProperty) + ": required");
        }
        if (tool.m_requiresAddress && !args.HasMember(AddressProperty))
        {
            return AZ::Failure(AZStd::string(AddressProperty) + ": required");
        }

        request.SetObject();
        request.AddMember("binding", String(tool.m_binding, allocator), allocator);
        request.AddMember("args", values, allocator);
        if (tool.m_takesTarget)
        {
            request.AddMember(rapidjson::StringRef(TargetProperty), Json(args[TargetProperty], allocator), allocator);
        }
        if (tool.m_takesAddress && args.HasMember(AddressProperty))
        {
            request.AddMember(rapidjson::StringRef(AddressProperty), Json(args[AddressProperty], allocator), allocator);
        }
        if (tool.m_overloaded)
        {
            request.AddMember("overload", 0, allocator);
        }
        return AZ::Success();
    }

    AZStd::string ModuleTools::MakeToolName(AZStd::string_view path, AZStd::string_view bindingId)
    {
        const bool valid = path.size() <= MaxToolNameLength && AZStd::all_of(path.begin(), path.end(), IsToolNameCharacter);
        if (!valid)
        {
            return HashedToolName(path, bindingId);
        }
        return AZStd::string(path);
    }

    AZStd::string ModuleTools::HashedToolName(AZStd::string_view path, AZStd::string_view bindingId)
    {
        AZStd::string name(path.substr(0, MaxToolNameLength - NameHashLength));
        for (char& character : name)
        {
            if (!IsToolNameCharacter(character))
            {
                character = '_';
            }
        }
        name += AZStd::string::format("-%08x", static_cast<AZ::u32>(AZ::Crc32(bindingId)));
        return name;
    }

    size_t ModuleTools::CountOpenTools() const
    {
        size_t count = 0;
        for (const OpenModule& open : m_modules)
        {
            count += open.m_tools.size();
        }
        return count;
    }
} // namespace LocalMcp
