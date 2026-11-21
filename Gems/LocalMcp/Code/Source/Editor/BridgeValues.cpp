/*
 * Copyright (c) Contributors to the Open 3D Engine Project.
 * For complete copyright and license terms please see the LICENSE at the root of this distribution.
 *
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 *
 */

#include "BridgeValues.h"

#include <AzCore/Asset/AssetManagerBus.h>
#include <AzCore/Component/ComponentApplicationBus.h>
#include <AzCore/Component/ComponentBus.h>
#include <AzCore/Component/Entity.h>
#include <AzCore/Math/Quaternion.h>
#include <AzCore/Math/Transform.h>
#include <AzCore/Math/Vector2.h>
#include <AzCore/Math/Vector3.h>
#include <AzCore/Math/Vector4.h>
#include <AzCore/Outcome/Outcome.h>
#include <AzCore/Serialization/Utils.h>
#include <AzCore/std/any.h>
#include <AzCore/std/containers/array.h>
#include <AzCore/std/limits.h>
#include <AzCore/std/string/conversions.h>
#include <AzCore/std/typetraits/is_signed.h>
#include <AzCore/std/utility/charconv.h>
#include <AzToolsFramework/Component/EditorComponentAPIBus.h>
#include <AzToolsFramework/Entity/EditorEntityContextBus.h>

#include <cmath>

namespace LocalMcp
{
    namespace
    {
        // Conversion budgets. Values beyond them are refused, never truncated.
        constexpr unsigned MaxNestingDepth = 16;
        constexpr size_t MaxStringBytes = 65536;
        constexpr size_t MaxContainerSize = 4096;
        constexpr rapidjson::SizeType MaxConstructorArguments = 32;

        // Longest accepted text of a decimal integer, a UUID, a value type tag and an object handle ID.
        constexpr size_t MaxDecimalLength = 32;
        constexpr size_t MaxUuidLength = 64;
        constexpr size_t MaxTagLength = 128;
        constexpr size_t MaxHandleLength = 128;

        //! Largest integer magnitude a JSON number holds exactly (2^53 - 1). Larger 64-bit values travel as decimal strings.
        constexpr AZ::s64 MaxSafeInteger = 9007199254740991LL;

        //! Coordinates in the largest tagged math value, a Vector4 or Quaternion.
        constexpr size_t MaxCoordinates = 4;

        // Spellings of the null UUID that are accepted even though Uuid::CreateStringPermissive() reports them as invalid.
        constexpr AZStd::string_view NullUuidBraced = "{00000000-0000-0000-0000-000000000000}";
        constexpr AZStd::string_view NullUuid = "00000000-0000-0000-0000-000000000000";

        //! Takes the pending Python error, which clears it, and returns its message.
        AZStd::string TakePythonError()
        {
            pybind11::error_already_set error;
            return error.what();
        }

        //! Casts a Python value to T; pybind11's message for a failed cast is the error.
        template<class T>
        BridgeResult<T> Cast(pybind11::handle value)
        {
            return TryPython(
                [value]()
                {
                    return value.cast<T>();
                });
        }

        //! Converts a Python value to native T through the marshaller.
        template<class T>
        BridgeResult<T> NativeValue(pybind11::object value)
        {
            EditorPythonBindings::Convert::StackVariableAllocator variables;
            AZ::BehaviorParameter expected{};
            expected.m_typeId = azrtti_typeid<T>();
            AZ::BehaviorArgument argument;
            const bool converted = EditorPythonBindings::Convert::PythonToBehaviorValueParameter(expected, value, argument, variables);
            if (!converted || !argument.ConvertTo<T>())
            {
                return AZ::Failure(AZStd::string("Native Python marshaller refused the tagged value"));
            }
            return AZ::Success(*argument.GetAsUnsafe<T>());
        }

        //! Converts a native value to Python through the marshaller.
        template<class T>
        BridgeResult<pybind11::object> PythonValue(const T& value)
        {
            AZ::BehaviorArgument argument(&value);
            EditorPythonBindings::Convert::StackVariableAllocator variables;
            AZStd::string error;
            auto result = EditorPythonBindings::Convert::BehaviorValueParameterToPython(argument, variables, &error);
            if (!error.empty())
            {
                return AZ::Failure(AZStd::move(error));
            }
            return AZ::Success(AZStd::move(result));
        }

        //! Parses a 64-bit integer sent as a decimal string, refusing overflow and trailing text.
        template<class T>
        BridgeResult<T> Decimal(const Json& value)
        {
            if (!value.IsString() || value.GetStringLength() > MaxDecimalLength)
            {
                return AZ::Failure(AZStd::string("Expected typed decimal string"));
            }

            T result{};
            const char* begin = value.GetString();
            const char* end = begin + value.GetStringLength();
            const auto parsed = AZStd::from_chars(begin, end, result);
            if (parsed.ec != AZStd::errc{} || parsed.ptr != end)
            {
                return AZ::Failure(AZStd::string("Invalid decimal integer or overflow"));
            }
            return AZ::Success(AZStd::move(result));
        }

        //! Refuses a Python value that is not an integer or does not fit in T.
        template<class T>
        BridgeResult<void> IntegerRange(pybind11::object value)
        {
            if (!PyLong_Check(value.ptr()) || PyBool_Check(value.ptr()))
            {
                return AZ::Failure(AZStd::string("Integer parameter requires an integer"));
            }

            if constexpr (AZStd::is_signed_v<T>)
            {
                auto number = Cast<AZ::s64>(value);
                if (!number)
                {
                    return AZ::Failure(number.TakeError());
                }
                if (number.GetValue() < AZStd::numeric_limits<T>::min() || number.GetValue() > AZStd::numeric_limits<T>::max())
                {
                    return AZ::Failure(AZStd::string("Integer parameter overflow"));
                }
            }
            else
            {
                auto number = Cast<AZ::u64>(value);
                if (!number)
                {
                    return AZ::Failure(number.TakeError());
                }
                if (number.GetValue() > AZStd::numeric_limits<T>::max())
                {
                    return AZ::Failure(AZStd::string("Unsigned integer parameter overflow"));
                }
            }
            return AZ::Success();
        }

        //! Refuses a Python scalar that the marshaller would silently convert to the expected native type:
        //! a non-string for a string, a non-boolean for a bool, an integer out of range, or a float that loses value.
        BridgeResult<void> ScalarContract(pybind11::object value, const AZ::BehaviorParameter* expected)
        {
            if (!expected)
            {
                return AZ::Success();
            }

            AZ::TypeId type = expected->m_typeId;
            if (expected->m_azRtti)
            {
                type = AZ::Internal::GetUnderlyingTypeId(*expected->m_azRtti);
            }

            const bool pointer = (expected->m_traits & AZ::BehaviorParameter::TR_POINTER) != 0;
            const bool isString = type == azrtti_typeid<AZStd::string>() || type == azrtti_typeid<AZStd::string_view>() ||
                (type == azrtti_typeid<char>() && pointer);
            if (isString && !PyUnicode_Check(value.ptr()))
            {
                return AZ::Failure(AZStd::string("String parameter requires a string"));
            }
            if (type == azrtti_typeid<bool>() && !PyBool_Check(value.ptr()))
            {
                return AZ::Failure(AZStd::string("Boolean parameter requires true or false"));
            }

            if (type == azrtti_typeid<char>() && !pointer)
            {
                return IntegerRange<char>(value);
            }
            if (type == azrtti_typeid<AZ::s8>())
            {
                return IntegerRange<AZ::s8>(value);
            }
            if (type == azrtti_typeid<AZ::u8>())
            {
                return IntegerRange<AZ::u8>(value);
            }
            if (type == azrtti_typeid<AZ::s16>())
            {
                return IntegerRange<AZ::s16>(value);
            }
            if (type == azrtti_typeid<AZ::u16>())
            {
                return IntegerRange<AZ::u16>(value);
            }
            if (type == azrtti_typeid<AZ::s32>())
            {
                return IntegerRange<AZ::s32>(value);
            }
            if (type == azrtti_typeid<AZ::u32>())
            {
                return IntegerRange<AZ::u32>(value);
            }
            if (type == azrtti_typeid<long>())
            {
                return IntegerRange<long>(value);
            }
            if (type == azrtti_typeid<unsigned long>())
            {
                return IntegerRange<unsigned long>(value);
            }
            if (type == azrtti_typeid<AZ::s64>())
            {
                return IntegerRange<AZ::s64>(value);
            }
            if (type == azrtti_typeid<AZ::u64>())
            {
                return IntegerRange<AZ::u64>(value);
            }

            if (type == azrtti_typeid<float>() || type == azrtti_typeid<double>())
            {
                const bool isNumber = (PyLong_Check(value.ptr()) || PyFloat_Check(value.ptr())) && !PyBool_Check(value.ptr());
                if (!isNumber)
                {
                    return AZ::Failure(AZStd::string("Floating parameter requires a number"));
                }

                auto number = Cast<double>(value);
                if (!number)
                {
                    return AZ::Failure(number.TakeError());
                }
                const double represented = type == azrtti_typeid<float>() ? static_cast<float>(number.GetValue()) : number.GetValue();
                if (!std::isfinite(represented))
                {
                    return AZ::Failure(AZStd::string("Nonfinite float or floating parameter overflow"));
                }

                // An integer must survive the round trip through the floating type unchanged.
                if (PyLong_Check(value.ptr()))
                {
                    auto exact = pybind11::reinterpret_steal<pybind11::object>(PyLong_FromDouble(represented));
                    auto equal = TryPython(
                        [&exact, &value]()
                        {
                            return exact.equal(value);
                        });
                    if (!equal)
                    {
                        return AZ::Failure(equal.TakeError());
                    }
                    if (!equal.GetValue())
                    {
                        return AZ::Failure(AZStd::string("Integer loses precision in floating parameter"));
                    }
                }
            }
            return AZ::Success();
        }

        //! Reads one finite float coordinate of a tagged math value.
        BridgeResult<float> Coordinate(const Json& value)
        {
            if (!value.IsNumber() || !std::isfinite(value.GetDouble()) || !std::isfinite(static_cast<float>(value.GetDouble())))
            {
                return AZ::Failure(AZStd::string("Expected finite float coordinate"));
            }

            // Integers get the same precision check as a Python integer passed to a float parameter.
            if (value.IsInt64() || value.IsUint64())
            {
                auto integer = value.IsInt64() ? pybind11::int_(value.GetInt64()) : pybind11::int_(value.GetUint64());
                AZ::BehaviorParameter coordinate{};
                coordinate.m_typeId = azrtti_typeid<float>();
                auto contract = ScalarContract(integer, &coordinate);
                if (!contract)
                {
                    return AZ::Failure(contract.TakeError());
                }
            }
            return AZ::Success(static_cast<float>(value.GetDouble()));
        }

        //! Reads exactly count coordinates; count is at most MaxCoordinates.
        BridgeResult<AZStd::array<float, MaxCoordinates>> Coordinates(const Json& value, size_t count)
        {
            AZ_Assert(count <= MaxCoordinates, "Tagged math values have at most %zu coordinates", MaxCoordinates);
            if (!value.IsArray() || value.Size() != count)
            {
                return AZ::Failure(AZStd::string("Tagged coordinate count does not match type"));
            }

            AZStd::array<float, MaxCoordinates> result{};
            for (size_t index = 0; index < count; ++index)
            {
                auto coordinate = Coordinate(value[aznumeric_cast<rapidjson::SizeType>(index)]);
                if (!coordinate)
                {
                    return AZ::Failure(coordinate.TakeError());
                }
                result[index] = coordinate.GetValue();
            }
            return AZ::Success(AZStd::move(result));
        }

        BridgeResult<AZ::Uuid> UuidValue(const Json& value)
        {
            if (!IsBoundedString(value, 0, MaxUuidLength))
            {
                return AZ::Failure(AZStd::string("Expected UUID string"));
            }

            const AZStd::string_view text(value.GetString(), value.GetStringLength());
            auto uuid = AZ::Uuid::CreateStringPermissive(text, true);
            if (uuid.IsNull() && text != NullUuidBraced && text != NullUuid)
            {
                return AZ::Failure(AZStd::string("Invalid UUID string"));
            }
            return AZ::Success(AZStd::move(uuid));
        }

        //! Refuses an entity that is not in the editor entity context.
        BridgeResult<void> EditorReference(AZ::EntityId entity)
        {
            bool exists = false;
            AzToolsFramework::EditorEntityContextRequestBus::BroadcastResult(
                exists, &AzToolsFramework::EditorEntityContextRequests::IsEditorEntity, entity);
            if (!exists)
            {
                return AZ::Failure(AZStd::string("Entity does not exist in the current editor context"));
            }
            return AZ::Success();
        }

        //! Refuses a component that is not attached to an editor entity.
        BridgeResult<void> ComponentReference(const AZ::EntityComponentIdPair& component)
        {
            auto entity = EditorReference(component.GetEntityId());
            if (!entity)
            {
                return entity;
            }

            bool exists = false;
            AzToolsFramework::EditorComponentAPIBus::BroadcastResult(
                exists, &AzToolsFramework::EditorComponentAPIRequests::IsValid, component);
            if (!exists)
            {
                return AZ::Failure(AZStd::string("Component no longer exists in the editor context"));
            }
            return AZ::Success();
        }

        //! Refuses an asset ID that the Asset Catalog does not know. The empty ID is allowed.
        BridgeResult<void> AssetReference(const AZ::Data::AssetId& asset)
        {
            if (asset == AZ::Data::AssetId{})
            {
                return AZ::Success();
            }

            AZ::Data::AssetInfo info;
            AZ::Data::AssetCatalogRequestBus::BroadcastResult(info, &AZ::Data::AssetCatalogRequests::GetAssetInfoById, asset);
            if (!asset.IsValid() || info.m_assetId != asset)
            {
                return AZ::Failure(AZStd::string("Asset ID is not present in the Asset Catalog"));
            }
            return AZ::Success();
        }

        //! Converts handle to native T and refuses it unless check accepts the value.
        template<class T, class Check>
        BridgeResult<void> CheckReference(pybind11::object handle, Check check)
        {
            auto native = NativeValue<T>(handle);
            if (!native)
            {
                return AZ::Failure(native.TakeError());
            }
            return check(native.GetValue());
        }

        //! Converts a native editor reference or math value to Python after reference checked it.
        template<class T>
        BridgeResult<pybind11::object> CheckedPythonValue(const T& value, BridgeResult<void> reference)
        {
            if (!reference)
            {
                return AZ::Failure(reference.TakeError());
            }
            return PythonValue(value);
        }
    } // namespace

    void BridgeValues::Activate()
    {
        m_scope = AZ::Uuid::CreateRandom().ToString<AZStd::string>();
        m_nextHandle = 0;
    }

    bool BridgeValues::Empty() const
    {
        return m_handles.empty();
    }

    bool BridgeValues::CanStore() const
    {
        return m_handles.size() < MaxHandles;
    }

    void BridgeValues::Clear()
    {
        m_handles.clear();
    }

    BridgeResult<pybind11::object> BridgeValues::Construct(
        const AZ::BehaviorClass& type, const Json& args, AZStd::optional<size_t> overload, unsigned depth)
    {
        const size_t count = args.Size();

        // Reflected constructors take the object under construction as their first argument.
        AZStd::optional<size_t> selected;
        if (overload)
        {
            if (*overload >= type.m_constructors.size())
            {
                return AZ::Failure(AZStd::string("Constructor index is out of range"));
            }
            selected = overload;
        }
        else if (count || !type.m_defaultConstructor)
        {
            for (size_t index = 0; index < type.m_constructors.size(); ++index)
            {
                const auto* constructor = type.m_constructors[index];
                const bool accepts =
                    constructor && count + 1 >= constructor->GetMinNumberOfArguments() && count + 1 <= constructor->GetNumArguments();
                if (!accepts)
                {
                    continue;
                }
                if (selected)
                {
                    return AZ::Failure(AZStd::string("Constructor conversion may be ambiguous; select an overload"));
                }
                selected = index;
            }
            if (!selected)
            {
                return AZ::Failure(AZStd::string("No reflected constructor accepts that argument count"));
            }
        }

        if (selected)
        {
            const AZ::BehaviorMethod* constructor = type.m_constructors[*selected];
            for (size_t index = 1; index < constructor->GetNumArguments(); ++index)
            {
                auto reason = Unsupported(*constructor->GetArgument(index));
                if (!reason.empty())
                {
                    return AZ::Failure(AZStd::move(reason));
                }
            }
        }

        pybind11::tuple inputs(count);
        for (size_t index = 0; index < count; ++index)
        {
            const AZ::BehaviorParameter* expected = type.m_constructors[*selected]->GetArgument(index + 1);
            auto input = Decode(args[aznumeric_cast<rapidjson::SizeType>(index)], expected, depth + 1);
            if (!input)
            {
                return input;
            }
            inputs[index] = input.TakeValue();
        }

        return EditorPythonBindings::Call::StructuredConstruct(type, selected, pybind11::reinterpret_borrow<pybind11::args>(inputs));
    }

    AZStd::string BridgeValues::Unsupported(const AZ::BehaviorParameter& parameter)
    {
        using Parameter = AZ::BehaviorParameter;
        if (parameter.m_traits & Parameter::TR_THIS_PTR)
        {
            return {};
        }
        if (parameter.m_traits & (Parameter::TR_ARRAY_BEGIN | Parameter::TR_ARRAY_END | Parameter::TR_ARRAY_SIZE))
        {
            return "Native array pointer ranges require a dedicated reflected value API";
        }

        if (parameter.m_traits & Parameter::TR_POINTER)
        {
            // A const char* is a string.
            if (parameter.m_typeId == azrtti_typeid<char>() && (parameter.m_traits & Parameter::TR_CONST))
            {
                return {};
            }

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
        // Behavior-only automation types need not be serialized. Native RTTI retains the template identity.
        const auto* owner = AZ::BehaviorContextHelper::GetClass(type);
        return owner && owner->m_azRtti && owner->m_azRtti->GetGenericTypeId() == AZ::AzTypeInfo<AZ::Outcome<void, void>>::GetTemplateId();
    }

    BridgeResult<Json> BridgeValues::Store(pybind11::object value, Allocator& allocator)
    {
        if (!CanStore())
        {
            return AZ::Failure(AZStd::string("Active object handle limit reached; release objects before continuing"));
        }

        ++m_nextHandle;
        const AZStd::string id = m_scope + ":" + AZStd::to_string(m_nextHandle);
        const AZStd::string typeName = EditorPythonBindings::Convert::GetPythonTypeName(value);

        Json result(rapidjson::kObjectType);
        result.AddMember("$handle", String(id, allocator), allocator);
        result.AddMember("typename", String(typeName, allocator), allocator);

        const auto* type = AZ::BehaviorContextHelper::GetClass(typeName);
        if (!type)
        {
            return AZ::Failure(AZStd::string("Object handle requires a registered owning type"));
        }
        result.AddMember("type", String(type->m_typeId.ToString<AZStd::string>(), allocator), allocator);

        m_handles.emplace(id, AZStd::move(value));
        return AZ::Success(AZStd::move(result));
    }

    BridgeResult<size_t> BridgeValues::Release(const AZStd::vector<AZStd::string>& handles)
    {
        if (handles.size() > MaxHandles)
        {
            return AZ::Failure(AZStd::string("Expected a bounded object handle list"));
        }

        for (const AZStd::string& handle : handles)
        {
            if (!m_handles.contains(handle))
            {
                return AZ::Failure(AZStd::string("Unknown or expired object handle"));
            }
        }

        size_t released = 0;
        for (const AZStd::string& handle : handles)
        {
            released += m_handles.erase(handle);
        }
        return AZ::Success(AZStd::move(released));
    }

    BridgeResult<pybind11::object> BridgeValues::Decode(const Json& value, const AZ::BehaviorParameter* expected, unsigned depth)
    {
        if (depth > MaxNestingDepth)
        {
            return AZ::Failure(AZStd::string("JSON value nesting exceeds 16"));
        }
        if (expected)
        {
            auto reason = Unsupported(*expected);
            if (!reason.empty())
            {
                return AZ::Failure(AZStd::move(reason));
            }
        }

        const AZ::TypeId type = expected ? expected->m_typeId : AZ::TypeId{};

        // Every scalar result is checked against the expected native type before it is returned.
        auto finish = [&](pybind11::object result) -> BridgeResult<pybind11::object>
        {
            auto contract = ScalarContract(result, expected);
            if (!contract)
            {
                return AZ::Failure(contract.TakeError());
            }
            return AZ::Success(AZStd::move(result));
        };

        if (value.IsNull())
        {
            if (expected && type != azrtti_typeid<AZStd::any>() && type != azrtti_typeid<void>())
            {
                return AZ::Failure(
                    AZStd::string("Null requires a nullable value; native pointer arguments are not supported by this marshaller"));
            }
            return AZ::Success(pybind11::object(pybind11::none()));
        }

        if (value.IsBool())
        {
            return finish(pybind11::bool_(value.GetBool()));
        }

        if (value.IsInt64() || value.IsUint64())
        {
            const bool safe = value.IsInt64() ? value.GetInt64() >= -MaxSafeInteger && value.GetInt64() <= MaxSafeInteger
                                              : value.GetUint64() <= static_cast<AZ::u64>(MaxSafeInteger);
            if (!safe)
            {
                return AZ::Failure(AZStd::string("64-bit values outside interoperable JSON range require a typed decimal string"));
            }
            return finish(value.IsInt64() ? pybind11::int_(value.GetInt64()) : pybind11::int_(value.GetUint64()));
        }

        if (value.IsNumber())
        {
            if (!std::isfinite(value.GetDouble()))
            {
                return AZ::Failure(AZStd::string("Nonfinite JSON number"));
            }
            return finish(pybind11::float_(value.GetDouble()));
        }

        if (value.IsString())
        {
            if (value.GetStringLength() > MaxStringBytes)
            {
                return AZ::Failure(AZStd::string("String value exceeds conversion budget"));
            }

            // The length is in budget, so IsBoundedString() fails only on an embedded NUL, which a C string cannot hold.
            const bool cString = expected && type == azrtti_typeid<char>() && (expected->m_traits & AZ::BehaviorParameter::TR_POINTER);
            if (cString && !IsBoundedString(value, 0, MaxStringBytes))
            {
                return AZ::Failure(AZStd::string("Native C string cannot preserve embedded NUL characters"));
            }

            if (type == azrtti_typeid<AZ::Uuid>())
            {
                auto uuid = UuidValue(value);
                if (!uuid)
                {
                    return AZ::Failure(uuid.TakeError());
                }
                return PythonValue(uuid.GetValue());
            }
            return finish(pybind11::str(value.GetString(), value.GetStringLength()));
        }

        if (value.IsObject() && value.HasMember("$handle"))
        {
            if (!IsString(value, "$handle", MaxHandleLength))
            {
                return AZ::Failure(AZStd::string("Malformed object handle"));
            }

            const auto found = m_handles.find(value["$handle"].GetString());
            if (found == m_handles.end())
            {
                return AZ::Failure(AZStd::string("Unknown or expired object handle"));
            }

            auto result = found->second;
            const auto* actual = AZ::BehaviorContextHelper::GetClass(EditorPythonBindings::Convert::GetPythonTypeName(result));
            if (!actual)
            {
                return AZ::Failure(AZStd::string("Object handle type is no longer registered"));
            }

            // A handle may outlive the editor object it refers to.
            BridgeResult<void> reference = AZ::Success();
            if (actual->m_typeId == azrtti_typeid<AZ::EntityId>())
            {
                reference = CheckReference<AZ::EntityId>(result, EditorReference);
            }
            if (actual->m_typeId == azrtti_typeid<AZ::EntityComponentIdPair>())
            {
                reference = CheckReference<AZ::EntityComponentIdPair>(result, ComponentReference);
            }
            if (actual->m_typeId == azrtti_typeid<AZ::Data::AssetId>())
            {
                reference = CheckReference<AZ::Data::AssetId>(result, AssetReference);
            }
            if (!reference)
            {
                return AZ::Failure(reference.TakeError());
            }
            return finish(result);
        }

        if (value.IsObject() && value.HasMember("$type"))
        {
            auto tagged = DecodeTagged(value, type, depth);
            if (!tagged)
            {
                return tagged;
            }
            return finish(tagged.TakeValue());
        }

        const auto contained = type.IsNull() ? AZStd::vector<AZ::Uuid>{} : AZ::Utils::GetContainedTypes(type);
        if (value.IsArray())
        {
            if (value.Size() > MaxContainerSize)
            {
                return AZ::Failure(AZStd::string("Container exceeds conversion budget"));
            }

            // A map whose keys are not strings travels as an array of [key, value] pairs.
            if (AZ::Utils::IsMapContainerType(type))
            {
                if (contained.size() != 2)
                {
                    return AZ::Failure(AZStd::string("Map key/value type metadata is unavailable"));
                }

                AZ::BehaviorParameter keyType{};
                keyType.m_typeId = contained[0];
                AZ::BehaviorParameter valueType{};
                valueType.m_typeId = contained[1];
                pybind11::dict result;
                for (const auto& pair : value.GetArray())
                {
                    if (!pair.IsArray() || pair.Size() != 2)
                    {
                        return AZ::Failure(AZStd::string("Non-string map requires key/value pair arrays"));
                    }

                    // The value is decoded before its key, and an unhashable key is a Python error.
                    auto item = Decode(pair[1], &valueType, depth + 1);
                    if (!item)
                    {
                        return item;
                    }
                    auto key = Decode(pair[0], &keyType, depth + 1);
                    if (!key)
                    {
                        return key;
                    }
                    auto stored = TryPython(
                        [&result, &key, &item]()
                        {
                            result[key.GetValue()] = item.GetValue();
                        });
                    if (!stored)
                    {
                        return AZ::Failure(stored.TakeError());
                    }
                }
                return AZ::Success(pybind11::object(result));
            }

            const bool tuple = AZ::Utils::IsPairContainerType(type) || AZ::Utils::IsTupleContainerType(type);
            if (tuple && value.Size() != contained.size())
            {
                return AZ::Failure(AZStd::string("Tuple length does not match its native signature"));
            }

            const bool typed = !contained.empty();
            pybind11::list result;
            for (rapidjson::SizeType index = 0; index < value.Size(); ++index)
            {
                AZ::BehaviorParameter itemType{};
                if (typed)
                {
                    itemType.m_typeId = contained[tuple ? index : 0];
                }
                auto item = Decode(value[index], typed ? &itemType : nullptr, depth + 1);
                if (!item)
                {
                    return item;
                }
                result.append(item.TakeValue());
            }

            // An unhashable set element is a Python error.
            if (AZ::Utils::IsSetContainerType(type))
            {
                return TryPython(
                    [&result]()
                    {
                        return pybind11::object(pybind11::set(result));
                    });
            }
            return AZ::Success(tuple ? pybind11::object(pybind11::tuple(result)) : pybind11::object(result));
        }

        if (value.IsObject() && AZ::Utils::IsMapContainerType(type))
        {
            if (value.MemberCount() > MaxContainerSize || contained.size() != 2)
            {
                return AZ::Failure(AZStd::string("Map budget or metadata is invalid"));
            }

            AZ::BehaviorParameter keyType{};
            keyType.m_typeId = contained[0];
            AZ::BehaviorParameter valueType{};
            valueType.m_typeId = contained[1];
            pybind11::dict result;
            for (auto member = value.MemberBegin(); member != value.MemberEnd(); ++member)
            {
                // The value is decoded before its key, and an unhashable key is a Python error.
                auto item = Decode(member->value, &valueType, depth + 1);
                if (!item)
                {
                    return item;
                }
                auto key = Decode(member->name, &keyType, depth + 1);
                if (!key)
                {
                    return key;
                }
                auto stored = TryPython(
                    [&result, &key, &item]()
                    {
                        result[key.GetValue()] = item.GetValue();
                    });
                if (!stored)
                {
                    return AZ::Failure(stored.TakeError());
                }
            }
            return AZ::Success(pybind11::object(result));
        }

        return AZ::Failure(AZStd::string("JSON value is not supported by the described native signature"));
    }

    BridgeResult<pybind11::object> BridgeValues::DecodeTagged(const Json& value, const AZ::TypeId& type, unsigned depth)
    {
        if (!IsString(value, "$type", MaxTagLength))
        {
            return AZ::Failure(AZStd::string("Malformed value type tag"));
        }
        const AZStd::string_view tag(value["$type"].GetString());

        if (tag == "int64" || tag == "uint64")
        {
            if (!value.HasMember("value"))
            {
                return AZ::Failure(AZStd::string("Typed integer requires value"));
            }
            if (tag == "uint64" && type == azrtti_typeid<AZStd::any>())
            {
                return AZ::Failure(AZStd::string(
                    "Native Python any conversion cannot preserve an unsigned integer type; use a concrete uint64 signature"));
            }
            if (tag == "int64")
            {
                auto number = Decimal<AZ::s64>(value["value"]);
                if (!number)
                {
                    return AZ::Failure(number.TakeError());
                }
                return AZ::Success(pybind11::object(pybind11::int_(number.GetValue())));
            }
            auto number = Decimal<AZ::u64>(value["value"]);
            if (!number)
            {
                return AZ::Failure(number.TakeError());
            }
            return AZ::Success(pybind11::object(pybind11::int_(number.GetValue())));
        }

        if (tag == "EntityId")
        {
            if (!value.HasMember("value"))
            {
                return AZ::Failure(AZStd::string("EntityId requires decimal value"));
            }
            auto id = Decimal<AZ::u64>(value["value"]);
            if (!id)
            {
                return AZ::Failure(id.TakeError());
            }
            const AZ::EntityId entity(id.GetValue());
            return CheckedPythonValue(entity, EditorReference(entity));
        }

        if (tag == "EntityComponentIdPair")
        {
            if (!value.HasMember("entity") || !value.HasMember("component"))
            {
                return AZ::Failure(AZStd::string("Component pair requires entity/component IDs"));
            }
            auto entityId = Decimal<AZ::u64>(value["entity"]);
            if (!entityId)
            {
                return AZ::Failure(entityId.TakeError());
            }
            auto componentId = Decimal<AZ::u64>(value["component"]);
            if (!componentId)
            {
                return AZ::Failure(componentId.TakeError());
            }
            const AZ::EntityComponentIdPair component(AZ::EntityId(entityId.GetValue()), componentId.GetValue());
            return CheckedPythonValue(component, ComponentReference(component));
        }

        if (tag == "Uuid")
        {
            if (!value.HasMember("value"))
            {
                return AZ::Failure(AZStd::string("Uuid requires value"));
            }
            auto uuid = UuidValue(value["value"]);
            if (!uuid)
            {
                return AZ::Failure(uuid.TakeError());
            }
            return PythonValue(uuid.GetValue());
        }

        if (tag == "AssetId")
        {
            if (!value.HasMember("guid") || !value.HasMember("subId") || !value["subId"].IsUint())
            {
                return AZ::Failure(AZStd::string("AssetId requires guid and uint32 subId"));
            }
            auto guid = UuidValue(value["guid"]);
            if (!guid)
            {
                return AZ::Failure(guid.TakeError());
            }
            const AZ::Data::AssetId asset(guid.GetValue(), value["subId"].GetUint());
            return CheckedPythonValue(asset, AssetReference(asset));
        }

        if (tag == "Vector2" || tag == "Vector3" || tag == "Vector4" || tag == "Quaternion")
        {
            if (!value.HasMember("value"))
            {
                return AZ::Failure(AZStd::string("Tagged math value requires numeric array"));
            }

            const size_t count = tag == "Vector2" ? 2 : (tag == "Vector3" ? 3 : 4);
            auto coordinates = Coordinates(value["value"], count);
            if (!coordinates)
            {
                return AZ::Failure(coordinates.TakeError());
            }

            const auto& numbers = coordinates.GetValue();
            if (tag == "Vector2")
            {
                return PythonValue(AZ::Vector2(numbers[0], numbers[1]));
            }
            if (tag == "Vector3")
            {
                return PythonValue(AZ::Vector3(numbers[0], numbers[1], numbers[2]));
            }
            if (tag == "Vector4")
            {
                return PythonValue(AZ::Vector4(numbers[0], numbers[1], numbers[2], numbers[3]));
            }
            return PythonValue(AZ::Quaternion(numbers[0], numbers[1], numbers[2], numbers[3]));
        }

        if (tag == "Transform")
        {
            if (!value.HasMember("translation") || !value.HasMember("rotation") || !value.HasMember("scale"))
            {
                return AZ::Failure(AZStd::string("Transform requires translation, rotation and uniform scale"));
            }

            auto translation = Coordinates(value["translation"], 3);
            if (!translation)
            {
                return AZ::Failure(translation.TakeError());
            }
            auto rotation = Coordinates(value["rotation"], 4);
            if (!rotation)
            {
                return AZ::Failure(rotation.TakeError());
            }

            const auto& position = translation.GetValue();
            const auto& orientation = rotation.GetValue();
            const AZ::Quaternion quaternion(orientation[0], orientation[1], orientation[2], orientation[3]);
            if (!AZ::IsClose(quaternion.GetLengthSq(), 1.0f))
            {
                return AZ::Failure(AZStd::string("Transform rotation must be a normalized quaternion"));
            }

            auto scale = Coordinate(value["scale"]);
            if (!scale)
            {
                return AZ::Failure(scale.TakeError());
            }
            auto transform =
                AZ::Transform::CreateFromQuaternionAndTranslation(quaternion, AZ::Vector3(position[0], position[1], position[2]));
            transform.SetUniformScale(scale.GetValue());
            return PythonValue(transform);
        }

        if (tag == "Outcome")
        {
            return DecodeOutcome(value, type, depth);
        }

        // Any other tag is the UUID of a reflected value class, built from constructor args or a container value.
        auto tagged = UuidValue(value["$type"]);
        if (!tagged)
        {
            return AZ::Failure(tagged.TakeError());
        }
        const AZ::TypeId taggedType = tagged.GetValue();
        const auto* owner = AZ::BehaviorContextHelper::GetClass(taggedType);
        if (!owner || !EditorPythonBindings::Scope::IsBehaviorFlaggedForEditor(owner->m_attributes))
        {
            return AZ::Failure(AZStd::string("Tagged type is not a reflected Automation/Common value class"));
        }

        if (value.HasMember("args"))
        {
            const Json& args = value["args"];
            const bool overloadValid = !value.HasMember("overload") || value["overload"].IsUint();
            if (!args.IsArray() || args.Size() > MaxConstructorArguments || !overloadValid)
            {
                return AZ::Failure(AZStd::string("Tagged constructor requires bounded args and optional overload index"));
            }

            AZStd::optional<size_t> overload;
            if (value.HasMember("overload"))
            {
                overload = value["overload"].GetUint();
            }
            return Construct(*owner, args, overload, depth + 1);
        }

        if (value.HasMember("value") && AZ::Utils::IsContainerType(taggedType))
        {
            AZ::BehaviorParameter element{};
            element.m_typeId = taggedType;
            auto decoded = Decode(value["value"], &element, depth + 1);
            if (!decoded)
            {
                return decoded;
            }

            EditorPythonBindings::Convert::StackVariableAllocator variables;
            AZ::BehaviorArgument native;
            if (!EditorPythonBindings::Convert::PythonToBehaviorValueParameter(element, decoded.GetValue(), native, variables))
            {
                return AZ::Failure(AZStd::string("Native marshaller refused typed container"));
            }
            if (!owner->m_cloner)
            {
                return AZ::Failure(AZStd::string("Typed container has no owning native clone"));
            }

            auto proxy = EditorPythonBindings::PythonProxyObjectManagement::CreatePythonProxyObject(taggedType, native.GetValueAddress());
            if (PyErr_Occurred())
            {
                return AZ::Failure(TakePythonError());
            }
            if (proxy.is_none())
            {
                return AZ::Failure(AZStd::string("Native typed container clone failed"));
            }
            return AZ::Success(AZStd::move(proxy));
        }

        return AZ::Failure(
            AZStd::string("Tagged class requires constructor args or a supported container value; use object handles for member edits"));
    }

    BridgeResult<pybind11::object> BridgeValues::DecodeOutcome(const Json& value, const AZ::TypeId& type, unsigned depth)
    {
        AZ::TypeId outcomeType = type;
        if (value.HasMember("type"))
        {
            auto declared = UuidValue(value["type"]);
            if (!declared)
            {
                return AZ::Failure(declared.TakeError());
            }
            outcomeType = declared.GetValue();
        }
        const bool typeMatches = type == outcomeType || type == azrtti_typeid<AZStd::any>() || type.IsNull();
        if (!IsOutcome(outcomeType) || !typeMatches)
        {
            return AZ::Failure(AZStd::string("Outcome tag does not match its native signature"));
        }
        if (!value.HasMember("success") || !value["success"].IsBool())
        {
            return AZ::Failure(AZStd::string("Outcome requires success flag"));
        }

        const auto* owner = AZ::BehaviorContextHelper::GetClass(outcomeType);
        if (!owner)
        {
            return AZ::Failure(AZStd::string("Outcome type has no reflected factory"));
        }

        // The Outcome is built by its reflected Success or Failure factory, which takes the payload when it has one.
        const bool success = value["success"].GetBool();
        const auto factory = owner->m_methods.find(success ? "Success" : "Failure");
        if (factory == owner->m_methods.end())
        {
            return AZ::Failure(AZStd::string("Outcome factory is not reflected"));
        }

        const size_t count = factory->second->GetNumArguments();
        pybind11::tuple inputs(count);
        if (count)
        {
            const char* payload = success ? "value" : "error";
            if (count != 1 || !value.HasMember(payload))
            {
                return AZ::Failure(AZStd::string("Outcome payload does not match factory"));
            }
            auto input = Decode(value[payload], factory->second->GetArgument(0), depth + 1);
            if (!input)
            {
                return input;
            }
            inputs[0] = input.TakeValue();
        }

        return EditorPythonBindings::Call::StructuredMethod(
            factory->second, pybind11::none(), pybind11::reinterpret_borrow<pybind11::args>(inputs));
    }

    BridgeResult<Json> BridgeValues::Encode(
        pybind11::object value, const AZ::BehaviorParameter* expected, Allocator& allocator, unsigned depth)
    {
        if (depth > MaxNestingDepth)
        {
            return AZ::Failure(AZStd::string("Result nesting exceeds conversion budget"));
        }

        const AZ::TypeId type = expected ? expected->m_typeId : AZ::TypeId{};
        if (value.is_none())
        {
            return AZ::Success(Json());
        }

        // A checked bool or float always casts.
        if (PyBool_Check(value.ptr()))
        {
            return AZ::Success(Json(value.cast<bool>()));
        }

        if (PyLong_Check(value.ptr()))
        {
            int overflow = 0;
            const long long number = PyLong_AsLongLongAndOverflow(value.ptr(), &overflow);
            if (PyErr_Occurred())
            {
                return AZ::Failure(TakePythonError());
            }
            if (overflow < 0)
            {
                return AZ::Failure(AZStd::string("Integer result is outside int64 range"));
            }

            // Above the int64 range only a uint64 remains.
            if (overflow > 0)
            {
                const unsigned long long unsignedValue = PyLong_AsUnsignedLongLong(value.ptr());
                if (PyErr_Occurred())
                {
                    return AZ::Failure(TakePythonError());
                }
                Json result(rapidjson::kObjectType);
                result.AddMember("$type", "uint64", allocator);
                result.AddMember("value", String(AZStd::to_string(unsignedValue), allocator), allocator);
                return AZ::Success(AZStd::move(result));
            }

            if (number > MaxSafeInteger || number < -MaxSafeInteger)
            {
                const bool unsignedType = type == azrtti_typeid<AZ::u64>();
                Json result(rapidjson::kObjectType);
                result.AddMember("$type", String(unsignedType ? "uint64" : "int64", allocator), allocator);
                result.AddMember("value", String(AZStd::to_string(number), allocator), allocator);
                return AZ::Success(AZStd::move(result));
            }
            return AZ::Success(Json(static_cast<int64_t>(number)));
        }

        if (PyFloat_Check(value.ptr()))
        {
            const double number = value.cast<double>();
            if (!std::isfinite(number))
            {
                return AZ::Failure(AZStd::string("Native result contains a nonfinite number"));
            }
            return AZ::Success(Json(number));
        }

        if (PyUnicode_Check(value.ptr()))
        {
            // Measure the UTF-8 form Python caches before copying any of it.
            Py_ssize_t size = 0;
            const char* text = PyUnicode_AsUTF8AndSize(value.ptr(), &size);
            if (!text)
            {
                return AZ::Failure(TakePythonError());
            }
            const size_t length = aznumeric_cast<size_t>(size);
            if (length > MaxStringBytes)
            {
                return AZ::Failure(AZStd::string("String result exceeds conversion budget"));
            }
            return AZ::Success(String({ text, length }, allocator));
        }

        const auto contained = type.IsNull() ? AZStd::vector<AZ::Uuid>{} : AZ::Utils::GetContainedTypes(type);
        if (PyList_Check(value.ptr()) || PyTuple_Check(value.ptr()) || PyAnySet_Check(value.ptr()))
        {
            const size_t size = pybind11::len(value);
            if (size > MaxContainerSize)
            {
                return AZ::Failure(AZStd::string("Sequence result exceeds conversion budget"));
            }

            const bool tuple = AZ::Utils::IsPairContainerType(type) || AZ::Utils::IsTupleContainerType(type);
            if (tuple && size != contained.size())
            {
                return AZ::Failure(AZStd::string("Native tuple result does not match reflected metadata"));
            }

            Json result(rapidjson::kArrayType);
            size_t index = 0;
            for (const auto element : pybind11::reinterpret_borrow<pybind11::iterable>(value))
            {
                AZ::BehaviorParameter item{};
                if (!contained.empty())
                {
                    item.m_typeId = contained[tuple ? index : 0];
                }
                const AZ::BehaviorParameter* itemType = contained.empty() ? nullptr : &item;
                auto encoded = Encode(pybind11::reinterpret_borrow<pybind11::object>(element), itemType, allocator, depth + 1);
                if (!encoded)
                {
                    return encoded;
                }
                result.PushBack(encoded.GetValue(), allocator);
                ++index;
            }
            return AZ::Success(AZStd::move(result));
        }

        if (PyDict_Check(value.ptr()))
        {
            const auto dictionary = pybind11::reinterpret_borrow<pybind11::dict>(value);
            if (dictionary.size() > MaxContainerSize)
            {
                return AZ::Failure(AZStd::string("Map result exceeds conversion budget"));
            }

            // A map with string keys becomes an object; any other map becomes an array of [key, value] pairs.
            bool stringKeys = true;
            for (const auto& pair : dictionary)
            {
                stringKeys = stringKeys && PyUnicode_Check(pair.first.ptr());
            }

            const bool typed = contained.size() == 2;
            AZ::BehaviorParameter keyType{};
            AZ::BehaviorParameter valueType{};
            if (typed)
            {
                keyType.m_typeId = contained[0];
                valueType.m_typeId = contained[1];
            }

            Json result(stringKeys ? rapidjson::kObjectType : rapidjson::kArrayType);
            for (const auto& pair : dictionary)
            {
                auto key = pybind11::reinterpret_borrow<pybind11::object>(pair.first);
                auto item = pybind11::reinterpret_borrow<pybind11::object>(pair.second);
                auto encoded = Encode(item, typed ? &valueType : nullptr, allocator, depth + 1);
                if (!encoded)
                {
                    return encoded;
                }
                const AZ::BehaviorParameter* nameType = typed && !stringKeys ? &keyType : nullptr;
                auto name = Encode(key, nameType, allocator, depth + 1);
                if (!name)
                {
                    return name;
                }
                if (stringKeys)
                {
                    result.AddMember(name.GetValue(), encoded.GetValue(), allocator);
                }
                else
                {
                    Json entry(rapidjson::kArrayType);
                    entry.PushBack(name.GetValue(), allocator);
                    entry.PushBack(encoded.GetValue(), allocator);
                    result.PushBack(entry, allocator);
                }
            }
            return AZ::Success(AZStd::move(result));
        }

        return EncodeObject(value, allocator, depth);
    }

    BridgeResult<Json> BridgeValues::EncodeObject(pybind11::object value, Allocator& allocator, unsigned depth)
    {
        const auto* owner = AZ::BehaviorContextHelper::GetClass(EditorPythonBindings::Convert::GetPythonTypeName(value));
        if (!owner)
        {
            return AZ::Failure(AZStd::string("Python result has no reflected owning value type"));
        }
        const AZ::TypeId actualType = owner->m_typeId;

        auto tagged = [&](const char* tag)
        {
            Json result(rapidjson::kObjectType);
            result.AddMember("$type", String(tag, allocator), allocator);
            return result;
        };
        auto vectorValue = [&](const char* tag, AZStd::initializer_list<float> coordinates) -> BridgeResult<Json>
        {
            Json numbers(rapidjson::kArrayType);
            for (const float number : coordinates)
            {
                if (!std::isfinite(number))
                {
                    return AZ::Failure(AZStd::string("Native math result contains nonfinite values"));
                }
                numbers.PushBack(number, allocator);
            }
            Json result = tagged(tag);
            result.AddMember("value", numbers, allocator);
            return AZ::Success(AZStd::move(result));
        };

        if (actualType == azrtti_typeid<AZ::EntityId>())
        {
            auto native = NativeValue<AZ::EntityId>(value);
            if (!native)
            {
                return AZ::Failure(native.TakeError());
            }
            const AZ::u64 entity = static_cast<AZ::u64>(native.GetValue());
            Json result = tagged("EntityId");
            result.AddMember("value", String(AZStd::to_string(entity), allocator), allocator);
            return AZ::Success(AZStd::move(result));
        }

        if (actualType == azrtti_typeid<AZ::EntityComponentIdPair>())
        {
            auto native = NativeValue<AZ::EntityComponentIdPair>(value);
            if (!native)
            {
                return AZ::Failure(native.TakeError());
            }
            const AZ::u64 entity = static_cast<AZ::u64>(native.GetValue().GetEntityId());
            const AZ::u64 component = native.GetValue().GetComponentId();
            Json result = tagged("EntityComponentIdPair");
            result.AddMember("entity", String(AZStd::to_string(entity), allocator), allocator);
            result.AddMember("component", String(AZStd::to_string(component), allocator), allocator);
            return AZ::Success(AZStd::move(result));
        }

        if (actualType == azrtti_typeid<AZ::Uuid>())
        {
            auto native = NativeValue<AZ::Uuid>(value);
            if (!native)
            {
                return AZ::Failure(native.TakeError());
            }
            return AZ::Success(String(native.GetValue().ToString<AZStd::string>(), allocator));
        }

        if (actualType == azrtti_typeid<AZ::Data::AssetId>())
        {
            auto native = NativeValue<AZ::Data::AssetId>(value);
            if (!native)
            {
                return AZ::Failure(native.TakeError());
            }
            const AZ::Data::AssetId& asset = native.GetValue();
            Json result = tagged("AssetId");
            result.AddMember("guid", String(asset.m_guid.ToString<AZStd::string>(), allocator), allocator);
            result.AddMember("subId", asset.m_subId, allocator);
            return AZ::Success(AZStd::move(result));
        }

        if (actualType == azrtti_typeid<AZ::Vector2>())
        {
            auto native = NativeValue<AZ::Vector2>(value);
            if (!native)
            {
                return AZ::Failure(native.TakeError());
            }
            const AZ::Vector2& vector = native.GetValue();
            return vectorValue("Vector2", { vector.GetX(), vector.GetY() });
        }

        if (actualType == azrtti_typeid<AZ::Vector3>())
        {
            auto native = NativeValue<AZ::Vector3>(value);
            if (!native)
            {
                return AZ::Failure(native.TakeError());
            }
            const AZ::Vector3& vector = native.GetValue();
            return vectorValue("Vector3", { vector.GetX(), vector.GetY(), vector.GetZ() });
        }

        if (actualType == azrtti_typeid<AZ::Vector4>())
        {
            auto native = NativeValue<AZ::Vector4>(value);
            if (!native)
            {
                return AZ::Failure(native.TakeError());
            }
            const AZ::Vector4& vector = native.GetValue();
            return vectorValue("Vector4", { vector.GetX(), vector.GetY(), vector.GetZ(), vector.GetW() });
        }

        if (actualType == azrtti_typeid<AZ::Quaternion>())
        {
            auto native = NativeValue<AZ::Quaternion>(value);
            if (!native)
            {
                return AZ::Failure(native.TakeError());
            }
            const AZ::Quaternion& quaternion = native.GetValue();
            return vectorValue("Quaternion", { quaternion.GetX(), quaternion.GetY(), quaternion.GetZ(), quaternion.GetW() });
        }

        if (actualType == azrtti_typeid<AZ::Transform>())
        {
            auto native = NativeValue<AZ::Transform>(value);
            if (!native)
            {
                return AZ::Failure(native.TakeError());
            }
            const AZ::Transform& transform = native.GetValue();
            if (!std::isfinite(transform.GetUniformScale()))
            {
                return AZ::Failure(AZStd::string("Native transform scale is nonfinite"));
            }

            AZ::BehaviorParameter translation{};
            translation.m_typeId = azrtti_typeid<AZ::Vector3>();
            AZ::BehaviorParameter rotation{};
            rotation.m_typeId = azrtti_typeid<AZ::Quaternion>();

            auto translationValue = PythonValue(transform.GetTranslation());
            if (!translationValue)
            {
                return AZ::Failure(translationValue.TakeError());
            }
            auto position = Encode(translationValue.TakeValue(), &translation, allocator, depth + 1);
            if (!position)
            {
                return position;
            }
            auto rotationValue = PythonValue(transform.GetRotation());
            if (!rotationValue)
            {
                return AZ::Failure(rotationValue.TakeError());
            }
            auto orientation = Encode(rotationValue.TakeValue(), &rotation, allocator, depth + 1);
            if (!orientation)
            {
                return orientation;
            }

            Json result = tagged("Transform");
            result.AddMember("translation", Json(position.GetValue()["value"], allocator), allocator);
            result.AddMember("rotation", Json(orientation.GetValue()["value"], allocator), allocator);
            result.AddMember("scale", transform.GetUniformScale(), allocator);
            return AZ::Success(AZStd::move(result));
        }

        if (IsOutcome(actualType))
        {
            return EncodeOutcome(value, *owner, allocator, depth);
        }

        // Anything else stays in Python and is returned as a handle.
        return Store(value, allocator);
    }

    BridgeResult<Json> BridgeValues::EncodeOutcome(
        pybind11::object value, const AZ::BehaviorClass& owner, Allocator& allocator, unsigned depth)
    {
        auto invoke = [&](const char* member) -> BridgeResult<pybind11::object>
        {
            const auto found = owner.m_methods.find(member);
            if (found == owner.m_methods.end())
            {
                return AZ::Failure(AZStd::string("Outcome accessor is not reflected"));
            }
            return EditorPythonBindings::Call::StructuredMethod(found->second, value, pybind11::args(pybind11::make_tuple()));
        };

        auto succeeded = invoke("IsSuccess");
        if (!succeeded)
        {
            return AZ::Failure(succeeded.TakeError());
        }
        auto isSuccess = Cast<bool>(succeeded.GetValue());
        if (!isSuccess)
        {
            return AZ::Failure(isSuccess.TakeError());
        }

        const bool success = isSuccess.GetValue();
        const char* accessor = success ? "GetValue" : "GetError";
        const auto found = owner.m_methods.find(accessor);
        const AZ::BehaviorParameter* payload = found == owner.m_methods.end() ? nullptr : found->second->GetResult();
        const bool isVoid = !payload || payload->m_typeId.IsNull() || payload->m_typeId == azrtti_typeid<void>();

        Json result(rapidjson::kObjectType);
        result.AddMember("$type", "Outcome", allocator);
        result.AddMember("type", String(owner.m_typeId.ToString<AZStd::string>(), allocator), allocator);
        result.AddMember("success", success, allocator);
        Json payloadName = String(success ? "value" : "error", allocator);
        Json payloadValue;
        if (!isVoid)
        {
            auto payloadObject = invoke(accessor);
            if (!payloadObject)
            {
                return AZ::Failure(payloadObject.TakeError());
            }
            auto encoded = Encode(payloadObject.TakeValue(), payload, allocator, depth + 1);
            if (!encoded)
            {
                return encoded;
            }
            payloadValue = encoded.GetValue();
        }
        result.AddMember(payloadName, payloadValue, allocator);
        return AZ::Success(AZStd::move(result));
    }
} // namespace LocalMcp
