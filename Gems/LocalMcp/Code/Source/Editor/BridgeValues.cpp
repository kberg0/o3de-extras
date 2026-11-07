/* Copyright (c) Contributors to the Open 3D Engine Project.
 * SPDX-License-Identifier: Apache-2.0 OR MIT */
#include "BridgeValues.h"
#include "BehaviorBindings.h"
#include <AzCore/Component/ComponentApplicationBus.h>
#include <AzCore/Component/Entity.h>
#include <AzCore/Component/ComponentBus.h>
#include <AzCore/std/limits.h>
#include <AzCore/std/typetraits/is_signed.h>
#include <AzCore/Math/Quaternion.h>
#include <AzCore/Math/Transform.h>
#include <AzCore/Math/Vector2.h>
#include <AzCore/Math/Vector3.h>
#include <AzCore/Math/Vector4.h>
#include <AzCore/Asset/AssetManagerBus.h>
#include <AzCore/Serialization/Utils.h>
#include <AzCore/Outcome/Outcome.h>
#include <AzCore/std/any.h>
#include <AzCore/std/containers/array.h>
#include <AzCore/std/math.h>
#include <AzCore/std/utility/charconv.h>
#include <AzToolsFramework/Component/EditorComponentAPIBus.h>
#include <AzToolsFramework/Entity/EditorEntityContextBus.h>
#include <stdexcept>
#include <cmath>
#include <string>
namespace LocalMcp
{
    namespace
    {
        template<class T>
        T NativeValue(pybind11::object value)
        {
            EditorPythonBindings::Convert::StackVariableAllocator variables;
            AZ::BehaviorParameter expected{};
            expected.m_typeId = azrtti_typeid<T>();
            AZ::BehaviorArgument argument;
            if (!EditorPythonBindings::Convert::PythonToBehaviorValueParameter(expected, value, argument, variables) || !argument.ConvertTo<T>())
            {
                throw BridgeError("Native Python marshaller refused the tagged value");
            }
            return *argument.GetAsUnsafe<T>();
        }

        template<class T>
        pybind11::object PythonValue(const T& value)
        {
            AZ::BehaviorArgument argument(&value);
            EditorPythonBindings::Convert::StackVariableAllocator variables;
            AZStd::string error;
            auto result = EditorPythonBindings::Convert::BehaviorValueParameterToPython(argument, variables, &error);
            if (!error.empty()) { throw BridgeError(error.c_str()); }
            return result;
        }

        template<class T>
        T Decimal(const Json& value)
        {
            if (!value.IsString() || value.GetStringLength() > 32) { throw BridgeError("Expected typed decimal string"); }
            T result{};
            const char* end = value.GetString() + value.GetStringLength();
            const auto parsed = AZStd::from_chars(value.GetString(), end, result);
            if (parsed.ec != AZStd::errc{} || parsed.ptr != end) { throw BridgeError("Invalid decimal integer or overflow"); }
            return result;
        }

        template<class T>
        void IntegerRange(pybind11::object value)
        {
            if (!PyLong_Check(value.ptr()) || PyBool_Check(value.ptr())) { throw BridgeError("Integer parameter requires an integer"); }
            if constexpr (AZStd::is_signed_v<T>)
            {
                const auto number = value.cast<AZ::s64>();
                if (number < AZStd::numeric_limits<T>::min() || number > AZStd::numeric_limits<T>::max())
                { throw BridgeError("Integer parameter overflow"); }
            }
            else
            {
                const auto number = value.cast<AZ::u64>();
                if (number > AZStd::numeric_limits<T>::max()) { throw BridgeError("Unsigned integer parameter overflow"); }
            }
        }

        void ScalarContract(pybind11::object value, const AZ::BehaviorParameter* expected)
        {
            if (!expected) { return; }
            auto type = expected->m_typeId;
            if (expected->m_azRtti)
            {
                type = AZ::Internal::GetUnderlyingTypeId(*expected->m_azRtti);
            }
            if ((type == azrtti_typeid<AZStd::string>() || type == azrtti_typeid<AZStd::string_view>() ||
                (type == azrtti_typeid<char>() && (expected->m_traits & AZ::BehaviorParameter::TR_POINTER))) && !PyUnicode_Check(value.ptr()))
            { throw BridgeError("String parameter requires a string"); }
            if (type == azrtti_typeid<bool>() && !PyBool_Check(value.ptr())) { throw BridgeError("Boolean parameter requires true or false"); }
            if (type == azrtti_typeid<char>() && !(expected->m_traits & AZ::BehaviorParameter::TR_POINTER)) { IntegerRange<char>(value); }
            if (type == azrtti_typeid<AZ::s8>()) { IntegerRange<AZ::s8>(value); }
            if (type == azrtti_typeid<AZ::u8>()) { IntegerRange<AZ::u8>(value); }
            if (type == azrtti_typeid<AZ::s16>()) { IntegerRange<AZ::s16>(value); }
            if (type == azrtti_typeid<AZ::u16>()) { IntegerRange<AZ::u16>(value); }
            if (type == azrtti_typeid<AZ::s32>()) { IntegerRange<AZ::s32>(value); }
            if (type == azrtti_typeid<AZ::u32>()) { IntegerRange<AZ::u32>(value); }
            if (type == azrtti_typeid<long>()) { IntegerRange<long>(value); }
            if (type == azrtti_typeid<unsigned long>()) { IntegerRange<unsigned long>(value); }
            if (type == azrtti_typeid<AZ::s64>()) { IntegerRange<AZ::s64>(value); }
            if (type == azrtti_typeid<AZ::u64>()) { IntegerRange<AZ::u64>(value); }
            if (type == azrtti_typeid<float>() || type == azrtti_typeid<double>())
            {
                if ((!PyLong_Check(value.ptr()) && !PyFloat_Check(value.ptr())) || PyBool_Check(value.ptr()))
                { throw BridgeError("Floating parameter requires a number"); }
                const double number = value.cast<double>();
                const double represented = type == azrtti_typeid<float>() ? static_cast<float>(number) : number;
                if (!std::isfinite(represented)) { throw BridgeError("Nonfinite float or floating parameter overflow"); }
                if (PyLong_Check(value.ptr()))
                {
                    auto exact = pybind11::reinterpret_steal<pybind11::object>(PyLong_FromDouble(represented));
                    if (!exact.equal(value)) { throw BridgeError("Integer loses precision in floating parameter"); }
                }
            }
        }

        float Coordinate(const Json& value)
        {
            if (!value.IsNumber() || !std::isfinite(value.GetDouble()) ||
                !std::isfinite(static_cast<float>(value.GetDouble())))
            { throw BridgeError("Expected finite float coordinate"); }
            if (value.IsInt64() || value.IsUint64())
            {
                auto integer = value.IsInt64() ? pybind11::int_(value.GetInt64()) : pybind11::int_(value.GetUint64());
                AZ::BehaviorParameter coordinate{};
                coordinate.m_typeId = azrtti_typeid<float>();
                ScalarContract(integer, &coordinate);
            }
            return static_cast<float>(value.GetDouble());
        }

        AZStd::array<float, 4> Coordinates(const Json& value, size_t count)
        {
            if (!value.IsArray() || value.Size() != count) { throw BridgeError("Tagged coordinate count does not match type"); }
            AZStd::array<float, 4> result{};
            for (size_t index = 0; index < count; ++index) { result[index] = Coordinate(value[static_cast<rapidjson::SizeType>(index)]); }
            return result;
        }

        AZ::Uuid UuidValue(const Json& value)
        {
            if (!value.IsString() || value.GetStringLength() > 64 || strlen(value.GetString()) != value.GetStringLength())
            { throw BridgeError("Expected UUID string"); }
            const AZStd::string_view text(value.GetString(), value.GetStringLength());
            const auto uuid = AZ::Uuid::CreateStringPermissive(text, true);
            if (uuid.IsNull() && text != "{00000000-0000-0000-0000-000000000000}" && text != "00000000-0000-0000-0000-000000000000")
            { throw BridgeError("Invalid UUID string"); }
            return uuid;
        }

        void EditorReference(AZ::EntityId entity)
        {
            bool exists = false;
            AzToolsFramework::EditorEntityContextRequestBus::BroadcastResult(exists,
                &AzToolsFramework::EditorEntityContextRequests::IsEditorEntity, entity);
            if (!exists) { throw BridgeError("Entity does not exist in the current editor context"); }
        }

        void ComponentReference(const AZ::EntityComponentIdPair& component)
        {
            EditorReference(component.GetEntityId());
            bool exists = false;
            AzToolsFramework::EditorComponentAPIBus::BroadcastResult(exists,
                &AzToolsFramework::EditorComponentAPIRequests::IsValid, component);
            if (!exists) { throw BridgeError("Component no longer exists in the editor context"); }
        }

        void AssetReference(const AZ::Data::AssetId& asset)
        {
            if (asset == AZ::Data::AssetId{}) { return; }
            AZ::Data::AssetInfo info;
            AZ::Data::AssetCatalogRequestBus::BroadcastResult(info, &AZ::Data::AssetCatalogRequests::GetAssetInfoById, asset);
            if (!asset.IsValid() || info.m_assetId != asset) { throw BridgeError("Asset ID is not present in the Asset Catalog"); }
        }
    }

    pybind11::object BridgeValues::Construct(const AZ::BehaviorClass& type, const Json& arguments, unsigned depth)
    {
        const auto& args = arguments["args"];
        const size_t count = args.Size();
        AZStd::optional<size_t> selected;
        if (arguments.HasMember("overload"))
        {
            selected = arguments["overload"].GetUint();
            if (*selected >= type.m_constructors.size()) { throw BridgeError("Constructor index is out of range"); }
        }
        else if (count || !type.m_defaultConstructor)
        {
            for (size_t index = 0; index < type.m_constructors.size(); ++index)
            {
                const auto* constructor = type.m_constructors[index];
                if (constructor && count + 1 >= constructor->GetMinNumberOfArguments() && count + 1 <= constructor->GetNumArguments())
                {
                    if (selected) { throw BridgeError("Constructor conversion may be ambiguous; select an overload"); }
                    selected = index;
                }
            }
            if (!selected) { throw BridgeError("No reflected constructor accepts that argument count"); }
        }
        if (selected)
        {
            for (size_t index = 1; index < type.m_constructors[*selected]->GetNumArguments(); ++index)
            {
                const auto reason = Unsupported(*type.m_constructors[*selected]->GetArgument(index));
                if (!reason.empty()) { throw BridgeError(reason.c_str()); }
            }
        }
        pybind11::tuple inputs(count);
        for (size_t index = 0; index < count; ++index)
        {
            inputs[index] = Decode(args[static_cast<rapidjson::SizeType>(index)], type.m_constructors[*selected]->GetArgument(index + 1), depth + 1);
        }
        auto result = EditorPythonBindings::Call::StructuredConstruct(type, selected, pybind11::reinterpret_borrow<pybind11::args>(inputs));
        if (!result) { throw BridgeError(result.GetError().c_str()); }
        return result.TakeValue();
    }

    AZStd::string BridgeValues::Unsupported(const AZ::BehaviorParameter& parameter)
    {
        using Parameter = AZ::BehaviorParameter;
        if (parameter.m_traits & Parameter::TR_THIS_PTR) { return {}; }
        if (parameter.m_traits & (Parameter::TR_ARRAY_BEGIN | Parameter::TR_ARRAY_END | Parameter::TR_ARRAY_SIZE))
        {
            return "Native array pointer ranges require a dedicated reflected value API";
        }
        if (parameter.m_traits & Parameter::TR_POINTER)
        {
            if (parameter.m_typeId == azrtti_typeid<char>() && (parameter.m_traits & Parameter::TR_CONST)) { return {}; }
            const auto* type = AZ::BehaviorContextHelper::GetClass(parameter.m_typeId);
            if (!type || !type->m_cloner || !EditorPythonBindings::Scope::IsBehaviorFlaggedForEditor(type->m_attributes))
            {
                return "Native pointer has no owning Automation/Common Python value conversion";
            }
        }
        return {};
    }

    bool BridgeValues::IsOutcome(const AZ::TypeId& type)
    {
        const auto* owner = AZ::BehaviorContextHelper::GetClass(type);
        // Behavior-only automation types need not be serialized. Native RTTI retains the template identity.
        return owner && owner->m_azRtti && owner->m_azRtti->GetGenericTypeId() == AZ::AzTypeInfo<AZ::Outcome<void, void>>::GetTemplateId();
    }

    void BridgeValues::Activate()
    {
        m_scope = AZ::Uuid::CreateRandom().ToString<AZStd::string>();
        m_nextHandle = 0;
    }

    Json BridgeValues::Store(pybind11::object value, Allocator& allocator)
    {
        if (!CanStore())
        {
            throw BridgeError("Active object handle limit reached; release objects before continuing");
        }
        const auto id = m_scope + AZStd::string::format(":%llu", ++m_nextHandle);
        Json result(rapidjson::kObjectType);
        result.AddMember("$handle", String(id, allocator), allocator);
        result.AddMember("typename", String(EditorPythonBindings::Convert::GetPythonTypeName(value), allocator), allocator);
        const auto* type = AZ::BehaviorContextHelper::GetClass(EditorPythonBindings::Convert::GetPythonTypeName(value));
        if (!type) { throw BridgeError("Object handle requires a registered owning type"); }
        result.AddMember("type", String(type->m_typeId.ToString<AZStd::string>(), allocator), allocator);
        m_handles.emplace(id, AZStd::move(value));
        return result;
    }

    size_t BridgeValues::Release(const Json& handles)
    {
        if (!handles.IsArray() || handles.Size() > MaxHandles)
        {
            throw BridgeError("Expected a bounded object handle list");
        }
        for (const auto& handle : handles.GetArray())
        {
            if (!handle.IsString() || handle.GetStringLength() > 128 ||
                strlen(handle.GetString()) != handle.GetStringLength() || !m_handles.contains(handle.GetString()))
            {
                throw BridgeError("Unknown or expired object handle");
            }
        }
        size_t released = 0;
        for (const auto& handle : handles.GetArray()) { released += m_handles.erase(handle.GetString()); }
        return released;
    }

    pybind11::object BridgeValues::Decode(const Json& value, const AZ::BehaviorParameter* expected, unsigned depth)
    {
        if (depth > 16) { throw BridgeError("JSON value nesting exceeds 16"); }
        if (expected)
        {
            const auto reason = Unsupported(*expected);
            if (!reason.empty()) { throw BridgeError(reason.c_str()); }
        }
        const AZ::TypeId type = expected ? expected->m_typeId : AZ::TypeId{};
        auto finish = [&](pybind11::object result)
        {
            ScalarContract(result, expected);
            return result;
        };
        if (value.IsNull())
        {
            if (expected && type != azrtti_typeid<AZStd::any>() && type != azrtti_typeid<void>())
            { throw BridgeError("Null requires a nullable value; native pointer arguments are not supported by this marshaller"); }
            return pybind11::none();
        }
        if (value.IsBool()) { return finish(pybind11::bool_(value.GetBool())); }
        if (value.IsInt64() || value.IsUint64())
        {
            constexpr AZ::u64 SafeInteger = 9007199254740991ULL;
            if ((value.IsInt64() && (value.GetInt64() < -static_cast<AZ::s64>(SafeInteger) || value.GetInt64() > static_cast<AZ::s64>(SafeInteger))) ||
                (!value.IsInt64() && value.GetUint64() > SafeInteger))
            { throw BridgeError("64-bit values outside interoperable JSON range require a typed decimal string"); }
            return finish(value.IsInt64() ? pybind11::int_(value.GetInt64()) : pybind11::int_(value.GetUint64()));
        }
        if (value.IsNumber())
        {
            if (!std::isfinite(value.GetDouble())) { throw BridgeError("Nonfinite JSON number"); }
            return finish(pybind11::float_(value.GetDouble()));
        }
        if (value.IsString())
        {
            if (value.GetStringLength() > 65536) { throw BridgeError("String value exceeds conversion budget"); }
            if (expected && type == azrtti_typeid<char>() &&
                (expected->m_traits & AZ::BehaviorParameter::TR_POINTER) &&
                strlen(value.GetString()) != value.GetStringLength())
            { throw BridgeError("Native C string cannot preserve embedded NUL characters"); }
            if (type == azrtti_typeid<AZ::Uuid>()) { return PythonValue(UuidValue(value)); }
            return finish(pybind11::str(value.GetString(), value.GetStringLength()));
        }
        if (value.IsObject() && value.HasMember("$handle"))
        {
            if (!IsString(value, "$handle", 128)) { throw BridgeError("Malformed object handle"); }
            const auto found = m_handles.find(value["$handle"].GetString());
            if (found == m_handles.end()) { throw BridgeError("Unknown or expired object handle"); }
            auto result = found->second;
            const auto* actual = AZ::BehaviorContextHelper::GetClass(EditorPythonBindings::Convert::GetPythonTypeName(result));
            if (!actual) { throw BridgeError("Object handle type is no longer registered"); }
            if (actual->m_typeId == azrtti_typeid<AZ::EntityId>()) { EditorReference(NativeValue<AZ::EntityId>(result)); }
            if (actual->m_typeId == azrtti_typeid<AZ::EntityComponentIdPair>()) { ComponentReference(NativeValue<AZ::EntityComponentIdPair>(result)); }
            if (actual->m_typeId == azrtti_typeid<AZ::Data::AssetId>()) { AssetReference(NativeValue<AZ::Data::AssetId>(result)); }
            return finish(result);
        }
        if (value.IsObject() && value.HasMember("$type"))
        {
            if (!IsString(value, "$type", 128)) { throw BridgeError("Malformed value type tag"); }
            const AZStd::string_view tag(value["$type"].GetString());
            if (tag == "int64" || tag == "uint64")
            {
                if (!value.HasMember("value")) { throw BridgeError("Typed integer requires value"); }
                if (tag == "uint64" && type == azrtti_typeid<AZStd::any>())
                { throw BridgeError("Native Python any conversion cannot preserve an unsigned integer type; use a concrete uint64 signature"); }
                return finish(tag == "int64" ? pybind11::int_(Decimal<AZ::s64>(value["value"])) : pybind11::int_(Decimal<AZ::u64>(value["value"])));
            }
            if (tag == "EntityId")
            {
                if (!value.HasMember("value")) { throw BridgeError("EntityId requires decimal value"); }
                const AZ::EntityId entity(Decimal<AZ::u64>(value["value"]));
                EditorReference(entity);
                return finish(PythonValue(entity));
            }
            if (tag == "EntityComponentIdPair")
            {
                if (!value.HasMember("entity") || !value.HasMember("component")) { throw BridgeError("Component pair requires entity/component IDs"); }
                const AZ::EntityComponentIdPair component(AZ::EntityId(Decimal<AZ::u64>(value["entity"])), Decimal<AZ::u64>(value["component"]));
                ComponentReference(component);
                return finish(PythonValue(component));
            }
            if (tag == "Uuid")
            {
                if (!value.HasMember("value")) { throw BridgeError("Uuid requires value"); }
                return finish(PythonValue(UuidValue(value["value"])));
            }
            if (tag == "AssetId")
            {
                if (!value.HasMember("guid") || !value.HasMember("subId") || !value["subId"].IsUint())
                { throw BridgeError("AssetId requires guid and uint32 subId"); }
                const AZ::Data::AssetId asset(UuidValue(value["guid"]), value["subId"].GetUint());
                AssetReference(asset);
                return finish(PythonValue(asset));
            }
            if (tag == "Vector2" || tag == "Vector3" || tag == "Vector4" || tag == "Quaternion")
            {
                if (!value.HasMember("value")) { throw BridgeError("Tagged math value requires numeric array"); }
                const auto numbers = Coordinates(value["value"], tag == "Vector2" ? 2 : tag == "Vector3" ? 3 : 4);
                if (tag == "Vector2") { return finish(PythonValue(AZ::Vector2(numbers[0], numbers[1]))); }
                if (tag == "Vector3") { return finish(PythonValue(AZ::Vector3(numbers[0], numbers[1], numbers[2]))); }
                if (tag == "Vector4") { return finish(PythonValue(AZ::Vector4(numbers[0], numbers[1], numbers[2], numbers[3]))); }
                return finish(PythonValue(AZ::Quaternion(numbers[0], numbers[1], numbers[2], numbers[3])));
            }
            if (tag == "Transform")
            {
                if (!value.HasMember("translation") || !value.HasMember("rotation") || !value.HasMember("scale"))
                { throw BridgeError("Transform requires translation, rotation and uniform scale"); }
                const auto translation = Coordinates(value["translation"], 3);
                const auto rotation = Coordinates(value["rotation"], 4);
                const AZ::Quaternion quaternion(rotation[0], rotation[1], rotation[2], rotation[3]);
                if (!AZ::IsClose(quaternion.GetLengthSq(), 1.0f)) { throw BridgeError("Transform rotation must be a normalized quaternion"); }
                auto transform = AZ::Transform::CreateFromQuaternionAndTranslation(quaternion,
                    AZ::Vector3(translation[0], translation[1], translation[2]));
                transform.SetUniformScale(Coordinate(value["scale"]));
                return finish(PythonValue(transform));
            }
            if (tag == "Outcome")
            {
                const auto outcomeType = value.HasMember("type") ? UuidValue(value["type"]) : type;
                if (!IsOutcome(outcomeType) || (type != outcomeType && type != azrtti_typeid<AZStd::any>() && !type.IsNull()))
                { throw BridgeError("Outcome tag does not match its native signature"); }
                if (!value.HasMember("success") || !value["success"].IsBool()) { throw BridgeError("Outcome requires success flag"); }
                const auto* owner = AZ::BehaviorContextHelper::GetClass(outcomeType);
                if (!owner) { throw BridgeError("Outcome type has no reflected factory"); }
                const auto method = owner->m_methods.find(value["success"].GetBool() ? "Success" : "Failure");
                if (method == owner->m_methods.end()) { throw BridgeError("Outcome factory is not reflected"); }
                const size_t count = method->second->GetNumArguments();
                pybind11::tuple inputs(count);
                if (count)
                {
                    const char* key = value["success"].GetBool() ? "value" : "error";
                    if (count != 1 || !value.HasMember(key)) { throw BridgeError("Outcome payload does not match factory"); }
                    inputs[0] = Decode(value[key], method->second->GetArgument(0), depth + 1);
                }
                auto result = EditorPythonBindings::Call::StructuredMethod(method->second, pybind11::none(), pybind11::reinterpret_borrow<pybind11::args>(inputs));
                if (!result) { throw BridgeError(result.GetError().c_str()); }
                return finish(result.TakeValue());
            }
            const auto taggedType = UuidValue(value["$type"]);
            const auto* owner = AZ::BehaviorContextHelper::GetClass(taggedType);
            if (!owner || !EditorPythonBindings::Scope::IsBehaviorFlaggedForEditor(owner->m_attributes))
            { throw BridgeError("Tagged type is not a reflected Automation/Common value class"); }
            if (value.HasMember("args"))
            {
                if (!value["args"].IsArray() || value["args"].Size() > 32 || (value.HasMember("overload") && !value["overload"].IsUint()))
                { throw BridgeError("Tagged constructor requires bounded args and optional overload index"); }
                return finish(Construct(*owner, value, depth + 1));
            }
            if (value.HasMember("value") && AZ::Utils::IsContainerType(taggedType))
            {
                AZ::BehaviorParameter element{};
                element.m_typeId = taggedType;
                auto decoded = Decode(value["value"], &element, depth + 1);
                EditorPythonBindings::Convert::StackVariableAllocator variables;
                AZ::BehaviorArgument native;
                if (!EditorPythonBindings::Convert::PythonToBehaviorValueParameter(element, decoded, native, variables))
                { throw BridgeError("Native marshaller refused typed container"); }
                if (!owner->m_cloner) { throw BridgeError("Typed container has no owning native clone"); }
                auto proxy = EditorPythonBindings::PythonProxyObjectManagement::CreatePythonProxyObject(taggedType, native.GetValueAddress());
                if (PyErr_Occurred()) { throw pybind11::error_already_set(); }
                if (proxy.is_none()) { throw BridgeError("Native typed container clone failed"); }
                return finish(proxy);
            }
            throw BridgeError("Tagged class requires constructor args or a supported container value; use object handles for member edits");
        }
        const auto contained = type.IsNull() ? AZStd::vector<AZ::Uuid>{} : AZ::Utils::GetContainedTypes(type);
        if (value.IsArray())
        {
            if (value.Size() > 4096) { throw BridgeError("Container exceeds conversion budget"); }
            if (AZ::Utils::IsMapContainerType(type))
            {
                if (contained.size() != 2) { throw BridgeError("Map key/value type metadata is unavailable"); }
                pybind11::dict result;
                AZ::BehaviorParameter keyType{}; keyType.m_typeId = contained[0];
                AZ::BehaviorParameter valueType{}; valueType.m_typeId = contained[1];
                for (const auto& pair : value.GetArray())
                {
                    if (!pair.IsArray() || pair.Size() != 2) { throw BridgeError("Non-string map requires key/value pair arrays"); }
                    result[Decode(pair[0], &keyType, depth + 1)] = Decode(pair[1], &valueType, depth + 1);
                }
                return result;
            }
            const bool tuple = AZ::Utils::IsPairContainerType(type) || AZ::Utils::IsTupleContainerType(type);
            if (tuple && value.Size() != contained.size()) { throw BridgeError("Tuple length does not match its native signature"); }
            pybind11::list result;
            for (rapidjson::SizeType index = 0; index < value.Size(); ++index)
            {
                AZ::BehaviorParameter itemType{};
                const bool typed = !contained.empty();
                if (typed) { itemType.m_typeId = contained[tuple ? index : 0]; }
                result.append(Decode(value[index], typed ? &itemType : nullptr, depth + 1));
            }
            if (AZ::Utils::IsSetContainerType(type)) { return pybind11::set(result); }
            return tuple ? pybind11::object(pybind11::tuple(result)) : pybind11::object(result);
        }
        if (value.IsObject() && AZ::Utils::IsMapContainerType(type))
        {
            if (value.MemberCount() > 4096 || contained.size() != 2) { throw BridgeError("Map budget or metadata is invalid"); }
            pybind11::dict result;
            AZ::BehaviorParameter keyType{}; keyType.m_typeId = contained[0];
            AZ::BehaviorParameter valueType{}; valueType.m_typeId = contained[1];
            for (auto iterator = value.MemberBegin(); iterator != value.MemberEnd(); ++iterator)
            {
                result[Decode(iterator->name, &keyType, depth + 1)] = Decode(iterator->value, &valueType, depth + 1);
            }
            return result;
        }
        throw BridgeError("JSON value is not supported by the described native signature");
    }

    Json BridgeValues::Encode(pybind11::object value, const AZ::BehaviorParameter* expected, Allocator& allocator, unsigned depth)
    {
        if (depth > 16) { throw BridgeError("Result nesting exceeds conversion budget"); }
        const AZ::TypeId type = expected ? expected->m_typeId : AZ::TypeId{};
        if (value.is_none()) { return Json(); }
        if (PyBool_Check(value.ptr())) { return Json(value.cast<bool>()); }
        if (PyLong_Check(value.ptr()))
        {
            constexpr AZ::s64 SafeInteger = 9007199254740991LL;
            int overflow = 0;
            const auto number = PyLong_AsLongLongAndOverflow(value.ptr(), &overflow);
            if (PyErr_Occurred()) { throw pybind11::error_already_set(); }
            if (overflow < 0) { throw BridgeError("Integer result is outside int64 range"); }
            if (overflow > 0)
            {
                const auto unsignedValue = PyLong_AsUnsignedLongLong(value.ptr());
                if (PyErr_Occurred()) { throw pybind11::error_already_set(); }
                Json result(rapidjson::kObjectType);
                result.AddMember("$type", "uint64", allocator);
                result.AddMember("value", String(AZStd::string::format("%llu", unsignedValue), allocator), allocator);
                return result;
            }
            if (number > SafeInteger || number < -SafeInteger)
            {
                const bool unsignedType = type == azrtti_typeid<AZ::u64>();
                Json result(rapidjson::kObjectType);
                result.AddMember("$type", String(unsignedType ? "uint64" : "int64", allocator), allocator);
                result.AddMember("value", String(AZStd::string::format("%lld", number), allocator), allocator);
                return result;
            }
            return Json(static_cast<int64_t>(number));
        }
        if (PyFloat_Check(value.ptr()))
        {
            const double number = value.cast<double>();
            if (!std::isfinite(number)) { throw BridgeError("Native result contains a nonfinite number"); }
            return Json(number);
        }
        if (PyUnicode_Check(value.ptr()))
        {
            const auto text = value.cast<std::string>();
            if (text.size() > 65536) { throw BridgeError("String result exceeds conversion budget"); }
            return String({text.data(), text.size()}, allocator);
        }
        const auto contained = type.IsNull() ? AZStd::vector<AZ::Uuid>{} : AZ::Utils::GetContainedTypes(type);
        if (PyList_Check(value.ptr()) || PyTuple_Check(value.ptr()) || PyAnySet_Check(value.ptr()))
        {
            const auto sequence = pybind11::reinterpret_borrow<pybind11::iterable>(value);
            const size_t size = pybind11::len(value);
            if (size > 4096) { throw BridgeError("Sequence result exceeds conversion budget"); }
            const bool tuple = AZ::Utils::IsPairContainerType(type) || AZ::Utils::IsTupleContainerType(type);
            if (tuple && size != contained.size()) { throw BridgeError("Native tuple result does not match reflected metadata"); }
            Json result(rapidjson::kArrayType);
            size_t index = 0;
            for (const auto element : sequence)
            {
                AZ::BehaviorParameter item{};
                if (!contained.empty()) { item.m_typeId = contained[tuple ? index : 0]; }
                result.PushBack(Encode(pybind11::reinterpret_borrow<pybind11::object>(element),
                    contained.empty() ? nullptr : &item, allocator, depth + 1), allocator);
                ++index;
            }
            return result;
        }
        if (PyDict_Check(value.ptr()))
        {
            const auto dictionary = pybind11::reinterpret_borrow<pybind11::dict>(value);
            if (dictionary.size() > 4096) { throw BridgeError("Map result exceeds conversion budget"); }
            bool stringKeys = true;
            for (const auto& pair : dictionary) { stringKeys = stringKeys && PyUnicode_Check(pair.first.ptr()); }
            Json result(stringKeys ? rapidjson::kObjectType : rapidjson::kArrayType);
            AZ::BehaviorParameter keyType{};
            AZ::BehaviorParameter valueType{};
            if (contained.size() == 2) { keyType.m_typeId = contained[0]; valueType.m_typeId = contained[1]; }
            for (const auto& pair : dictionary)
            {
                auto key = pybind11::reinterpret_borrow<pybind11::object>(pair.first);
                auto item = pybind11::reinterpret_borrow<pybind11::object>(pair.second);
                Json encoded = Encode(item, contained.size() == 2 ? &valueType : nullptr, allocator, depth + 1);
                if (stringKeys)
                {
                    Json name = Encode(key, nullptr, allocator, depth + 1);
                    result.AddMember(name, encoded, allocator);
                }
                else
                {
                    Json entry(rapidjson::kArrayType);
                    entry.PushBack(Encode(key, contained.size() == 2 ? &keyType : nullptr, allocator, depth + 1), allocator);
                    entry.PushBack(encoded, allocator);
                    result.PushBack(entry, allocator);
                }
            }
            return result;
        }
        const auto name = EditorPythonBindings::Convert::GetPythonTypeName(value);
        const auto* owner = AZ::BehaviorContextHelper::GetClass(name);
        if (!owner) { throw BridgeError("Python result has no reflected owning value type"); }
        const auto actualType = owner->m_typeId;
        auto tagged = [&](const char* tag)
        {
            Json result(rapidjson::kObjectType);
            result.AddMember("$type", String(tag, allocator), allocator);
            return result;
        };
        auto vectorValue = [&](const char* tag, AZStd::initializer_list<float> coordinates)
        {
            Json result = tagged(tag);
            Json numbers(rapidjson::kArrayType);
            for (const auto number : coordinates)
            {
                if (!std::isfinite(number)) { throw BridgeError("Native math result contains nonfinite values"); }
                numbers.PushBack(number, allocator);
            }
            result.AddMember("value", numbers, allocator);
            return result;
        };
        if (actualType == azrtti_typeid<AZ::EntityId>())
        {
            Json result = tagged("EntityId");
            result.AddMember("value", String(AZStd::string::format("%llu", static_cast<AZ::u64>(NativeValue<AZ::EntityId>(value))), allocator), allocator);
            return result;
        }
        if (actualType == azrtti_typeid<AZ::EntityComponentIdPair>())
        {
            const auto pair = NativeValue<AZ::EntityComponentIdPair>(value);
            Json result = tagged("EntityComponentIdPair");
            result.AddMember("entity", String(AZStd::string::format("%llu", static_cast<AZ::u64>(pair.GetEntityId())), allocator), allocator);
            result.AddMember("component", String(AZStd::string::format("%llu", pair.GetComponentId()), allocator), allocator);
            return result;
        }
        if (actualType == azrtti_typeid<AZ::Uuid>()) { return String(NativeValue<AZ::Uuid>(value).ToString<AZStd::string>(), allocator); }
        if (actualType == azrtti_typeid<AZ::Data::AssetId>())
        {
            const auto asset = NativeValue<AZ::Data::AssetId>(value);
            Json result = tagged("AssetId");
            result.AddMember("guid", String(asset.m_guid.ToString<AZStd::string>(), allocator), allocator);
            result.AddMember("subId", asset.m_subId, allocator);
            return result;
        }
        if (actualType == azrtti_typeid<AZ::Vector2>())
        {
            const auto vector = NativeValue<AZ::Vector2>(value);
            return vectorValue("Vector2", {vector.GetX(), vector.GetY()});
        }
        if (actualType == azrtti_typeid<AZ::Vector3>())
        {
            const auto vector = NativeValue<AZ::Vector3>(value);
            return vectorValue("Vector3", {vector.GetX(), vector.GetY(), vector.GetZ()});
        }
        if (actualType == azrtti_typeid<AZ::Vector4>())
        {
            const auto vector = NativeValue<AZ::Vector4>(value);
            return vectorValue("Vector4", {vector.GetX(), vector.GetY(), vector.GetZ(), vector.GetW()});
        }
        if (actualType == azrtti_typeid<AZ::Quaternion>())
        {
            const auto quaternion = NativeValue<AZ::Quaternion>(value);
            return vectorValue("Quaternion", {quaternion.GetX(), quaternion.GetY(), quaternion.GetZ(), quaternion.GetW()});
        }
        if (actualType == azrtti_typeid<AZ::Transform>())
        {
            const auto transform = NativeValue<AZ::Transform>(value);
            if (!std::isfinite(transform.GetUniformScale())) { throw BridgeError("Native transform scale is nonfinite"); }
            Json result = tagged("Transform");
            AZ::BehaviorParameter translation{}; translation.m_typeId = azrtti_typeid<AZ::Vector3>();
            AZ::BehaviorParameter rotation{}; rotation.m_typeId = azrtti_typeid<AZ::Quaternion>();
            Json position = Encode(PythonValue(transform.GetTranslation()), &translation, allocator, depth + 1);
            Json orientation = Encode(PythonValue(transform.GetRotation()), &rotation, allocator, depth + 1);
            result.AddMember("translation", Json(position["value"], allocator), allocator);
            result.AddMember("rotation", Json(orientation["value"], allocator), allocator);
            result.AddMember("scale", transform.GetUniformScale(), allocator);
            return result;
        }
        if (IsOutcome(actualType))
        {
            auto call = [&](const char* member)
            {
                const auto found = owner->m_methods.find(member);
                if (found == owner->m_methods.end()) { throw BridgeError("Outcome accessor is not reflected"); }
                auto result = EditorPythonBindings::Call::StructuredMethod(found->second, value, pybind11::args(pybind11::make_tuple()));
                if (!result) { throw BridgeError(result.GetError().c_str()); }
                return result.TakeValue();
            };
            const bool success = call("IsSuccess").cast<bool>();
            const char* accessor = success ? "GetValue" : "GetError";
            const auto found = owner->m_methods.find(accessor);
            const AZ::BehaviorParameter* payload = found == owner->m_methods.end() ? nullptr : found->second->GetResult();
            Json result = tagged("Outcome");
            result.AddMember("type", String(actualType.ToString<AZStd::string>(), allocator), allocator);
            result.AddMember("success", success, allocator);
            const bool isVoid = !payload || payload->m_typeId.IsNull() || payload->m_typeId == azrtti_typeid<void>();
            result.AddMember(String(success ? "value" : "error", allocator), isVoid ? Json() : Encode(call(accessor), payload, allocator, depth + 1), allocator);
            return result;
        }
        return Store(value, allocator);
    }

}
