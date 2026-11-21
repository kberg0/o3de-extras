/*
 * Copyright (c) Contributors to the Open 3D Engine Project.
 * For complete copyright and license terms please see the LICENSE at the root of this distribution.
 *
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 *
 */

#include "Editor/BehaviorBridge.h"
#include "Editor/BindingRecipe.h"
#include "Editor/EditorTools.h"
#include "Editor/PythonExecute.h"
#include <Tests/PythonTestingUtility.h>
#include <AzCore/Script/ScriptContextAttributes.h>
#include <AzCore/Math/MathReflection.h>
#include <AzCore/Math/Vector3.h>
#include <AzCore/std/parallel/thread.h>
#include <AzCore/std/limits.h>
#include <AzCore/std/containers/unordered_set.h>

namespace LocalMcp
{
    enum class BridgeFixtureMode : AZ::u8
    {
        Active = 7
    };
}
AZ_TYPE_INFO_SPECIALIZE(LocalMcp::BridgeFixtureMode, "{A47AE9A2-7F08-49CD-B830-AF2563B67DD8}");

namespace LocalMcp
{
    using BridgeNumbers = AZStd::vector<int>;
    using BridgeSet = AZStd::unordered_set<int>;
    using BehaviorBooleanOutcome = AZ::Outcome<bool, AZStd::string>;
    using BehaviorNumbersOutcome = AZ::Outcome<BridgeNumbers, AZStd::string>;
    using BehaviorVoidOutcome = AZ::Outcome<void, AZStd::string>;
    using BridgeOutcome = AZ::Outcome<int, AZStd::string>;
    struct BridgeFixtureValue
    {
        AZ_TYPE_INFO(BridgeFixtureValue, "{165239CF-3B4E-44F6-9E99-883143DBA210}");
        BridgeFixtureValue() = default;
        explicit BridgeFixtureValue(int value)
            : m_value(value)
        {
        }
        int Add(int value)
        {
            return m_value += value;
        }
        int m_value = 0;
    };

    struct BridgeFixtureRequests : AZ::EBusTraits
    {
        using BusIdType = AZ::u32;
        static constexpr AZ::EBusAddressPolicy AddressPolicy = AZ::EBusAddressPolicy::ById;
        static constexpr AZ::EBusHandlerPolicy HandlerPolicy = AZ::EBusHandlerPolicy::Single;
        virtual int Add(int value) = 0;
    };
    using BridgeFixtureBus = AZ::EBus<BridgeFixtureRequests>;

    //! Shaped like ScriptCanvas GraphAutomationRequestBus: a broadcast bus reflected for automation in module
    //! scriptcanvas.automation, so that the module tools are exercised the way an agent drives Script Canvas graphs.
    struct GraphFixtureRequests : AZ::EBusTraits
    {
        virtual AZStd::string Describe(const AZStd::string& graph) = 0;
        virtual int Count() = 0;
    };
    using GraphFixtureBus = AZ::EBus<GraphFixtureRequests>;

    using PaneNames = AZStd::vector<AZStd::string>;

    class BehaviorBridgeTests
        : public UnitTest::PythonTestingFixture
        , private BridgeFixtureBus::Handler
        , private GraphFixtureBus::Handler
    {
    protected:
        void SetUp() override
        {
            PythonTestingFixture::SetUp();
            RegisterComponentDescriptors();
            m_app.GetSerializeContext()->RegisterGenericType<BridgeNumbers>();
            m_app.GetSerializeContext()->RegisterGenericType<BridgeSet>();
            m_app.GetSerializeContext()->RegisterGenericType<BridgeOutcome>();
            auto* context = m_app.GetBehaviorContext();
            context->Class<BridgeFixtureValue>("BridgeFixtureValue")
                ->Attribute(AZ::Script::Attributes::Scope, AZ::Script::Attributes::ScopeFlags::Automation)
                ->Attribute(AZ::Script::Attributes::Module, "bridge_test")
                ->Constructor<int>()
                ->Property("value", BehaviorValueProperty(&BridgeFixtureValue::m_value))
                ->Method("Add", &BridgeFixtureValue::Add)
                ->Method(
                    "Ambiguous",
                    [](int value)
                    {
                        return value;
                    })
                ->Method(
                    "Ambiguous",
                    [](double value)
                    {
                        return static_cast<int>(value);
                    });
            context
                ->Method(
                    "BridgeFixtureEcho",
                    [](int value)
                    {
                        return value;
                    })
                ->Attribute(AZ::Script::Attributes::Scope, AZ::Script::Attributes::ScopeFlags::Automation)
                ->Attribute(AZ::Script::Attributes::ToolTip, "fixture echo documentation");
            context
                ->Method(
                    "BridgeFixtureCString",
                    [](const char* value)
                    {
                        return AZStd::string_view(value).size();
                    })
                ->Attribute(AZ::Script::Attributes::Scope, AZ::Script::Attributes::ScopeFlags::Automation);
            context
                ->Method(
                    "BridgeFixtureLauncherOnly",
                    []()
                    {
                        return 42;
                    })
                ->Attribute(AZ::Script::Attributes::Scope, AZ::Script::Attributes::ScopeFlags::Launcher);
            context->EnumProperty<BridgeFixtureMode::Active>("BridgeFixtureActive")
                ->Attribute(AZ::Script::Attributes::Scope, AZ::Script::Attributes::ScopeFlags::Automation);
            context
                ->Method(
                    "BridgeFixtureMode",
                    [](BridgeFixtureMode value)
                    {
                        return value;
                    })
                ->Attribute(AZ::Script::Attributes::Scope, AZ::Script::Attributes::ScopeFlags::Automation);
            context
                ->Method(
                    "BridgeFixtureNumbers",
                    [](const BridgeNumbers& values)
                    {
                        return values;
                    })
                ->Attribute(AZ::Script::Attributes::Scope, AZ::Script::Attributes::ScopeFlags::Automation);
            context
                ->Method(
                    "BridgeFixtureSet",
                    [](const BridgeSet& values)
                    {
                        return values;
                    })
                ->Attribute(AZ::Script::Attributes::Scope, AZ::Script::Attributes::ScopeFlags::Automation);
            context
                ->Method(
                    "BridgeFixtureOutcome",
                    [](const BridgeOutcome& value)
                    {
                        return value;
                    })
                ->Attribute(AZ::Script::Attributes::Scope, AZ::Script::Attributes::ScopeFlags::Automation);
            // Deliberately no SerializeContext registration: real editor Outcome events are behavior-only.
            context
                ->Method(
                    "BehaviorBooleanOutcome",
                    [](bool success) -> BehaviorBooleanOutcome
                    {
                        return success ? BehaviorBooleanOutcome(AZ::Success(true))
                                       : BehaviorBooleanOutcome(AZ::Failure(AZStd::string("refused")));
                    })
                ->Attribute(AZ::Script::Attributes::Scope, AZ::Script::Attributes::ScopeFlags::Automation);
            context
                ->Method(
                    "BehaviorNumbersOutcome",
                    []() -> BehaviorNumbersOutcome
                    {
                        return AZ::Success(BridgeNumbers{ 2, 4 });
                    })
                ->Attribute(AZ::Script::Attributes::Scope, AZ::Script::Attributes::ScopeFlags::Automation);
            context
                ->Method(
                    "BehaviorVoidOutcome",
                    []() -> BehaviorVoidOutcome
                    {
                        return AZ::Success();
                    })
                ->Attribute(AZ::Script::Attributes::Scope, AZ::Script::Attributes::ScopeFlags::Automation);
            context->EBus<BridgeFixtureBus>("BridgeFixtureBus")
                ->Attribute(AZ::Script::Attributes::Scope, AZ::Script::Attributes::ScopeFlags::Automation)
                ->Event("Add", &BridgeFixtureRequests::Add);
            context->EBus<GraphFixtureBus>("GraphFixtureBus")
                ->Attribute(AZ::Script::Attributes::Scope, AZ::Script::Attributes::ScopeFlags::Automation)
                ->Attribute(AZ::Script::Attributes::Module, "scriptcanvas.automation")
                ->Event("Describe", &GraphFixtureRequests::Describe)
                ->Attribute(AZ::Script::Attributes::ToolTip, "Describes a graph")
                ->Event("Count", &GraphFixtureRequests::Count);

            // One more callable binding than a module may open at once.
            for (size_t index = 0; index <= ModuleTools::MaxOpenTools; ++index)
            {
                context
                    ->Method(
                        AZStd::string::format("CapFixture%03zu", index).c_str(),
                        []()
                        {
                            return 1;
                        })
                    ->Attribute(AZ::Script::Attributes::Scope, AZ::Script::Attributes::ScopeFlags::Automation)
                    ->Attribute(AZ::Script::Attributes::Module, "bridge_cap");
            }

            // Stands in for the editor's reflected list of view pane names.
            if (!m_app.GetSerializeContext()->FindGenericClassInfo(azrtti_typeid<PaneNames>()))
            {
                m_app.GetSerializeContext()->RegisterGenericType<PaneNames>();
            }
            context
                ->Method(
                    "get_pane_class_names",
                    []()
                    {
                        return PaneNames{ "Console", "Entity Inspector" };
                    })
                ->Attribute(AZ::Script::Attributes::Scope, AZ::Script::Attributes::ScopeFlags::Automation);
            m_python = AZStd::make_unique<AZ::Entity>();
            Activate(*m_python);
            SimulateEditorBecomingInitialized();
            m_bridge.Activate();
            BridgeFixtureBus::Handler::BusConnect(7);
            GraphFixtureBus::Handler::BusConnect();
        }
        void TearDown() override
        {
            GraphFixtureBus::Handler::BusDisconnect();
            BridgeFixtureBus::Handler::BusDisconnect();
            m_bridge.Deactivate();
            m_python.reset();
            PythonTestingFixture::TearDown();
        }
        int Add(int value) override
        {
            return value + 7;
        }
        AZStd::string Describe(const AZStd::string& graph) override
        {
            return "graph " + graph;
        }
        int Count() override
        {
            return 3;
        }
        rapidjson::Document Call(const char* tool, const char* arguments)
        {
            rapidjson::Document input;
            input.Parse(arguments);
            EXPECT_FALSE(input.HasParseError());
            rapidjson::Document result(rapidjson::kObjectType);
            AZStd::string detail;
            EXPECT_EQ(m_bridge.Invoke(tool, input, result, result.GetAllocator(), detail), ToolProvider::Result::Complete)
                << detail.c_str();
            return result;
        }
        BehaviorBridge m_bridge;
        AZStd::unique_ptr<AZ::Entity> m_python;
    };

    TEST_F(BehaviorBridgeTests, BehaviorOnlyOutcomesUseNativeTemplateAndAccessorMetadata)
    {
        EXPECT_EQ(m_app.GetSerializeContext()->FindGenericClassInfo(azrtti_typeid<BehaviorBooleanOutcome>()), nullptr);
        EXPECT_EQ(m_app.GetSerializeContext()->FindGenericClassInfo(azrtti_typeid<BehaviorNumbersOutcome>()), nullptr);
        auto truth = Call("binding_call", R"({"binding":"global/method/BehaviorBooleanOutcome","args":[true]})");
        ASSERT_FALSE(truth.HasMember("error"));
        ASSERT_TRUE(truth["value"].HasMember("$type"));
        EXPECT_STREQ(truth["value"]["$type"].GetString(), "Outcome");
        ASSERT_TRUE(truth["value"].HasMember("success"));
        EXPECT_TRUE(truth["value"]["success"].GetBool());
        EXPECT_TRUE(truth["value"]["value"].GetBool());
        auto refused = Call("binding_call", R"({"binding":"global/method/BehaviorBooleanOutcome","args":[false]})");
        ASSERT_FALSE(refused.HasMember("error"));
        EXPECT_FALSE(refused["value"]["success"].GetBool());
        EXPECT_STREQ(refused["value"]["error"].GetString(), "refused");
        auto sequence = Call("binding_call", R"({"binding":"global/method/BehaviorNumbersOutcome","args":[]})");
        ASSERT_FALSE(sequence.HasMember("error"));
        ASSERT_TRUE(sequence["value"]["value"].IsArray());
        EXPECT_EQ(sequence["value"]["value"].Size(), 2);
        auto empty = Call("binding_call", R"({"binding":"global/method/BehaviorVoidOutcome","args":[]})");
        ASSERT_FALSE(empty.HasMember("error"));
        EXPECT_TRUE(empty["value"]["success"].GetBool());
        EXPECT_TRUE(empty["value"]["value"].IsNull());
    }

    TEST_F(BehaviorBridgeTests, DiscoveryIncludesUnavailableBindingsAndNativeSignatures)
    {
        auto search = Call("binding_search", R"({"query":"BridgeFixtureLauncherOnly","limit":1})");
        ASSERT_TRUE(search.HasMember("matches"));
        ASSERT_EQ(search["matches"].Size(), 1);
        EXPECT_STREQ(search["matches"][0]["name"].GetString(), "BridgeFixtureLauncherOnly");
        EXPECT_FALSE(search["matches"][0]["callable"].GetBool());
        EXPECT_GT(search["matches"][0]["reason"].GetStringLength(), 0);
        auto describe = Call("binding_describe", R"({"binding":"global/method/BridgeFixtureEcho"})");
        ASSERT_FALSE(describe.HasMember("error"));
        ASSERT_TRUE(describe.HasMember("overloads"));
        EXPECT_EQ(describe["overloads"].Size(), 1);
        EXPECT_EQ(describe["overloads"][0]["arguments"].Size(), 1);
        EXPECT_STREQ(describe["documentation"].GetString(), "fixture echo documentation");
        auto denied = Call("binding_call", R"({"binding":"global/method/BridgeFixtureLauncherOnly","args":[]})");
        EXPECT_TRUE(denied.HasMember("error"));
        EXPECT_FALSE(denied.HasMember("nativeCallCompleted"));
    }

    TEST_F(BehaviorBridgeTests, RecipeCallsUseNativeConversionAndUnwrapOutcomeErrors)
    {
        BindingRecipe recipe(m_bridge);
        rapidjson::Document storage;
        auto& allocator = storage.GetAllocator();
        const Json input(42);
        auto result = recipe.Call("global/method/BridgeFixtureEcho", allocator, { &input });
        ASSERT_TRUE(result.IsSuccess()) << result.GetError().c_str();
        EXPECT_EQ(result.GetValue().GetInt(), 42);
        const Json lossy(4.5);
        EXPECT_FALSE(recipe.Call("global/method/BridgeFixtureEcho", allocator, { &lossy }).IsSuccess());
        rapidjson::Document outcome;
        const auto type = azrtti_typeid<BridgeOutcome>().ToString<AZStd::string>();
        outcome.Parse(AZStd::string::format(R"({"$type":"Outcome","type":"%s","success":true,"value":9})", type.c_str()).c_str());
        auto unwrapped = recipe.Call("global/method/BridgeFixtureOutcome", allocator, { &outcome });
        ASSERT_TRUE(unwrapped.IsSuccess()) << unwrapped.GetError().c_str();
        EXPECT_EQ(unwrapped.GetValue().GetInt(), 9);
        outcome.Parse(
            AZStd::string::format(R"({"$type":"Outcome","type":"%s","success":false,"error":"native refusal"})", type.c_str()).c_str());
        EXPECT_FALSE(recipe.Call("global/method/BridgeFixtureOutcome", allocator, { &outcome }).IsSuccess());
    }

    TEST_F(BehaviorBridgeTests, RecipeNormalizationUsesTheExistingCodecBeforeMutation)
    {
        rapidjson::Document input;
        input.Parse(R"({"$type":"Vector3","value":[3,4,0]})");
        auto normalized = m_bridge.NormalizeValue(input, azrtti_typeid<AZ::Vector3>(), input.GetAllocator());
        ASSERT_TRUE(normalized.IsSuccess()) << normalized.GetError().c_str();
        EXPECT_EQ(normalized.GetValue()["value"][0].GetFloat(), 3.0f);
        EXPECT_EQ(normalized.GetValue()["value"][1].GetFloat(), 4.0f);
        EXPECT_FALSE(m_bridge.NormalizeValue(input, azrtti_typeid<int>(), input.GetAllocator()).IsSuccess());
        input.Parse(R"({"$type":"EntityId","value":"0"})");
        EXPECT_FALSE(m_bridge.NormalizeValue(input, azrtti_typeid<AZ::EntityId>(), input.GetAllocator()).IsSuccess());
    }

    TEST_F(BehaviorBridgeTests, CallsRejectLossyConversionAndRequireAmbiguousOverload)
    {
        auto echo = Call("binding_call", R"({"binding":"global/method/BridgeFixtureEcho","args":[42]})");
        ASSERT_FALSE(echo.HasMember("error"));
        EXPECT_EQ(echo["value"].GetInt(), 42);
        for (const char* value : { "1.5", "true", "2147483648", "9007199254740992" })
        {
            auto args = AZStd::string::format(R"({"binding":"global/method/BridgeFixtureEcho","args":[%s]})", value);
            auto refused = Call("binding_call", args.c_str());
            EXPECT_TRUE(refused.HasMember("error")) << value;
            EXPECT_FALSE(refused.HasMember("nativeCallCompleted"));
        }
        auto ambiguous = Call("binding_call", R"({"binding":"class/BridgeFixtureValue/method/Ambiguous","args":[3]})");
        EXPECT_TRUE(ambiguous.HasMember("error"));
        auto selected = Call("binding_call", R"({"binding":"class/BridgeFixtureValue/method/Ambiguous","overload":0,"args":[3]})");
        ASSERT_FALSE(selected.HasMember("error"));
        EXPECT_EQ(selected["value"].GetDouble(), 3);
        auto event = Call("binding_call", R"({"binding":"bus/BridgeFixtureBus/event/Add","address":7,"args":[5]})");
        ASSERT_FALSE(event.HasMember("error"));
        EXPECT_EQ(event["value"].GetInt(), 12);
        auto text = Call("binding_call", R"({"binding":"global/method/BridgeFixtureCString","args":["abc"]})");
        ASSERT_FALSE(text.HasMember("error"));
        EXPECT_EQ(text["value"].GetInt(), 3);
        auto truncated = Call("binding_call", R"({"binding":"global/method/BridgeFixtureCString","args":["a\u0000b"]})");
        ASSERT_TRUE(truncated.HasMember("error"));
        EXPECT_STREQ(truncated["error"].GetString(), "Native C string cannot preserve embedded NUL characters");
        EXPECT_FALSE(truncated.HasMember("nativeCallCompleted"));
    }

    TEST_F(BehaviorBridgeTests, OwnedHandlesSupportPropertiesAndExpireAtOwnerFence)
    {
        auto made = Call("binding_call", R"({"binding":"class/BridgeFixtureValue","args":[8]})");
        ASSERT_FALSE(made.HasMember("error"));
        ASSERT_TRUE(made["value"].HasMember("$handle"));
        const AZStd::string handle(made["value"]["$handle"].GetString());
        auto getArgs =
            AZStd::string::format(R"({"binding":"class/BridgeFixtureValue/property/value","target":{"$handle":"%s"}})", handle.c_str());
        auto first = Call("binding_get", getArgs.c_str());
        ASSERT_FALSE(first.HasMember("error"));
        EXPECT_EQ(first["value"].GetInt(), 8);
        auto setArgs = AZStd::string::format(
            R"({"binding":"class/BridgeFixtureValue/property/value","target":{"$handle":"%s"},"value":12})", handle.c_str());
        auto set = Call("binding_set", setArgs.c_str());
        ASSERT_FALSE(set.HasMember("error"));
        auto changed = Call("binding_get", getArgs.c_str());
        EXPECT_EQ(changed["value"].GetInt(), 12);
        AzToolsFramework::EditorEntityContextNotificationBus::Broadcast(
            &AzToolsFramework::EditorEntityContextNotificationBus::Events::OnPrepareForContextReset);
        auto expired = Call("binding_get", getArgs.c_str());
        EXPECT_TRUE(expired.HasMember("error"));
        EXPECT_STREQ(expired["error"].GetString(), "Unknown or expired object handle");
    }

    TEST_F(BehaviorBridgeTests, TaggedValuesRoundTripAndRejectInvalidReferences)
    {
        auto* python = AZ::Interface<AzToolsFramework::EditorPythonEventsInterface>::Get();
        python->ExecuteWithLock(
            [&]()
            {
                const char* step = "import";
                try
                {
                    pybind11::module::import("azlmbr.object");
                    BridgeValues values;
                    values.Activate();
                    rapidjson::Document input;
                    input.Parse(R"({"$type":"Vector3","value":[1,2,3]})");
                    AZ::BehaviorParameter expected{};
                    expected.m_typeId = azrtti_typeid<AZ::Vector3>();
                    step = "Vector3 roundtrip";
                    auto decoded = values.Decode(input, &expected);
                    ASSERT_TRUE(decoded.IsSuccess()) << decoded.GetError().c_str();
                    auto encoded = values.Encode(decoded.GetValue(), &expected, input.GetAllocator());
                    ASSERT_TRUE(encoded.IsSuccess()) << encoded.GetError().c_str();
                    ASSERT_TRUE(encoded.GetValue().HasMember("value"));
                    EXPECT_EQ(encoded.GetValue()["value"][2].GetFloat(), 3);
                    input.Parse(R"({"$type":"uint64","value":"18446744073709551615"})");
                    expected.m_typeId = azrtti_typeid<AZ::u64>();
                    step = "uint64 roundtrip";
                    decoded = values.Decode(input, &expected);
                    ASSERT_TRUE(decoded.IsSuccess()) << decoded.GetError().c_str();
                    encoded = values.Encode(decoded.GetValue(), &expected, input.GetAllocator());
                    ASSERT_TRUE(encoded.IsSuccess()) << encoded.GetError().c_str();
                    EXPECT_STREQ(encoded.GetValue()["value"].GetString(), "18446744073709551615");
                    input.Parse(R"({"$type":"EntityId","value":"123"})");
                    expected.m_typeId = azrtti_typeid<AZ::EntityId>();
                    EXPECT_FALSE(values.Decode(input, &expected).IsSuccess());
                    input.Parse(R"({"$type":"Vector3","value":[16777217,0,0]})");
                    expected.m_typeId = azrtti_typeid<AZ::Vector3>();
                    EXPECT_FALSE(values.Decode(input, &expected).IsSuccess());
                    input.Parse(R"({"$type":"uint64","value":"18446744073709551616"})");
                    expected.m_typeId = azrtti_typeid<AZ::u64>();
                    EXPECT_FALSE(values.Decode(input, &expected).IsSuccess());

                    // pybind11 refuses to cast a uint64 above the int64 range; its exception becomes the error.
                    input.Parse(R"({"$type":"uint64","value":"18446744073709551615"})");
                    expected.m_typeId = azrtti_typeid<AZ::s64>();
                    step = "uint64 into int64";
                    auto refused = values.Decode(input, &expected);
                    ASSERT_FALSE(refused.IsSuccess());
                    EXPECT_NE(refused.GetError().find("Unable to cast"), AZStd::string::npos) << refused.GetError().c_str();
                    values.Clear();
                } catch (const std::exception& error)
                {
                    ADD_FAILURE() << step << ": " << error.what();
                }
            });
    }

    TEST_F(BehaviorBridgeTests, WrongThreadRefusesBeforeInterpreterAccess)
    {
        AZStd::thread worker(
            [&]()
            {
                auto result = Call("binding_call", R"({"binding":"global/method/BridgeFixtureEcho","args":[42]})");
                EXPECT_TRUE(result.HasMember("error"));
                EXPECT_FALSE(result.HasMember("nativeCallCompleted"));
            });
        worker.join();
    }

    TEST_F(BehaviorBridgeTests, EnumsUseNativeIntegersAndPreserveTypeNames)
    {
        auto describe = Call("binding_describe", R"({"binding":"global/method/BridgeFixtureMode"})");
        ASSERT_FALSE(describe.HasMember("error"));
        auto& parameter = describe["overloads"][0]["arguments"][0];
        EXPECT_STREQ(parameter["nativeTypeName"].GetString(), AZ::AzTypeInfo<BridgeFixtureMode>::Name());
        EXPECT_STREQ(parameter["type"].GetString(), azrtti_typeid<AZ::u8>().ToString<AZStd::string>().c_str());
        EXPECT_FALSE(parameter.HasMember("symbols"));
        auto mode = Call("binding_call", R"({"binding":"global/method/BridgeFixtureMode","args":[7]})");
        ASSERT_FALSE(mode.HasMember("error"));
        EXPECT_EQ(mode["value"].GetInt(), 7);
        auto unnamed = Call("binding_call", R"({"binding":"global/method/BridgeFixtureMode","args":[3]})");
        ASSERT_FALSE(unnamed.HasMember("error"));
        EXPECT_EQ(unnamed["value"].GetInt(), 3);
        auto constant = Call("binding_get", R"({"binding":"global/property/BridgeFixtureActive"})");
        ASSERT_FALSE(constant.HasMember("error"));
        EXPECT_EQ(constant["value"].GetInt(), 7);
        for (const char* value : { "256", "-1", "true" })
        {
            auto args = AZStd::string::format(R"({"binding":"global/method/BridgeFixtureMode","args":[%s]})", value);
            auto refused = Call("binding_call", args.c_str());
            EXPECT_TRUE(refused.HasMember("error")) << value;
            EXPECT_FALSE(refused.HasMember("nativeCallCompleted"));
        }
    }

    TEST_F(BehaviorBridgeTests, ContainersAndOutcomeUseNativeMarshal)
    {
        auto numbers = Call("binding_call", R"({"binding":"global/method/BridgeFixtureNumbers","args":[[1,2,3]]})");
        ASSERT_FALSE(numbers.HasMember("error"));
        ASSERT_EQ(numbers["value"].Size(), 3);
        EXPECT_EQ(numbers["value"][2].GetInt(), 3);
        auto unique = Call("binding_call", R"({"binding":"global/method/BridgeFixtureSet","args":[[1,2,2]]})");
        ASSERT_FALSE(unique.HasMember("error"));
        EXPECT_EQ(unique["value"].Size(), 2);
        auto refused = Call("binding_call", R"({"binding":"global/method/BridgeFixtureNumbers","args":[[1,true]]})");
        EXPECT_TRUE(refused.HasMember("error"));
        auto args = AZStd::string::format(
            R"({"binding":"global/method/BridgeFixtureOutcome","args":[{"$type":"Outcome","type":"%s","success":true,"value":42}]})",
            azrtti_typeid<BridgeOutcome>().ToString<AZStd::string>().c_str());
        auto outcome = Call("binding_call", args.c_str());
        ASSERT_FALSE(outcome.HasMember("error"));
        EXPECT_TRUE(outcome["value"]["success"].GetBool());
        EXPECT_EQ(outcome["value"]["value"].GetInt(), 42);
    }

    TEST_F(BehaviorBridgeTests, SearchFiltersByGeneratedKindNames)
    {
        EXPECT_EQ(Bindings::ToString(Bindings::Kind::BusProperty), "BusProperty");
        EXPECT_EQ(Bindings::FromStringToKind("BusProperty"), Bindings::Kind::BusProperty);
        EXPECT_FALSE(Bindings::FromStringToKind("bus_property").has_value());

        auto methods = Call("binding_search", R"({"query":"BridgeFixtureEcho","kind":"Method"})");
        ASSERT_TRUE(methods.HasMember("matches"));
        ASSERT_GE(methods["matches"].Size(), 1u);
        for (const Json& match : methods["matches"].GetArray())
        {
            EXPECT_STREQ(match["kind"].GetString(), "Method");
        }

        auto buses = Call("binding_search", R"({"query":"BridgeFixtureBus","kind":"Bus"})");
        ASSERT_GE(buses["matches"].Size(), 1u);
        EXPECT_STREQ(buses["matches"][0]["kind"].GetString(), "Bus");

        // Kind names are matched exactly; the old snake_case names are refused.
        rapidjson::Document input;
        input.Parse(R"({"query":"BridgeFixtureEcho","kind":"method"})");
        rapidjson::Document result(rapidjson::kObjectType);
        AZStd::string detail;
        EXPECT_EQ(m_bridge.Invoke(BindingSearchTool, input, result, result.GetAllocator(), detail), ToolProvider::Result::InvalidArguments);
        EXPECT_FALSE(detail.empty());
    }

    TEST_F(BehaviorBridgeTests, ReleaseRefusesMalformedOversizedAndUnknownHandles)
    {
        auto made = Call("binding_call", R"({"binding":"class/BridgeFixtureValue","args":[8]})");
        ASSERT_TRUE(made["value"].HasMember("$handle"));
        const AZStd::string handle(made["value"]["$handle"].GetString());

        auto unknown = Call("binding_release", R"({"handles":["missing"]})");
        ASSERT_TRUE(unknown.HasMember("error"));
        EXPECT_STREQ(unknown["error"].GetString(), "Unknown or expired object handle");

        // A list over the handle budget, a non-string item and an over-long handle never reach the handle table.
        AZStd::string oversized = R"({"handles":[)";
        for (size_t index = 0; index <= BridgeValues::MaxHandles; ++index)
        {
            oversized += index ? R"(,"h")" : R"("h")";
        }
        oversized += "]}";
        const AZStd::string longHandle = AZStd::string::format(R"({"handles":["%s"]})", AZStd::string(129, 'h').c_str());
        for (const AZStd::string& text :
             { oversized, AZStd::string(R"({"handles":[1]})"), AZStd::string(R"({"handles":"h"})"), longHandle })
        {
            rapidjson::Document input;
            input.Parse(text.c_str());
            ASSERT_FALSE(input.HasParseError());
            rapidjson::Document result(rapidjson::kObjectType);
            AZStd::string detail;
            EXPECT_EQ(
                m_bridge.Invoke(BindingReleaseTool, input, result, result.GetAllocator(), detail), ToolProvider::Result::InvalidArguments);
            EXPECT_FALSE(detail.empty());
        }

        auto released = Call("binding_release", AZStd::string::format(R"({"handles":["%s"]})", handle.c_str()).c_str());
        ASSERT_FALSE(released.HasMember("error"));
        EXPECT_EQ(released["released"].GetUint64(), 1u);
    }

    TEST_F(BehaviorBridgeTests, PropertyAccessAcceptsAnOverloadIndex)
    {
        auto made = Call("binding_call", R"({"binding":"class/BridgeFixtureValue","args":[5]})");
        ASSERT_TRUE(made["value"].HasMember("$handle"));
        const auto args = AZStd::string::format(
            R"({"binding":"class/BridgeFixtureValue/property/value","overload":0,"target":{"$handle":"%s"}})",
            made["value"]["$handle"].GetString());
        auto value = Call("binding_get", args.c_str());
        ASSERT_FALSE(value.HasMember("error"));
        EXPECT_EQ(value["value"].GetInt(), 5);
    }

    TEST_F(BehaviorBridgeTests, StringResultsAreBoundedBeforeCopying)
    {
        constexpr size_t MaxStringBytes = 65536;
        auto* python = AZ::Interface<AzToolsFramework::EditorPythonEventsInterface>::Get();
        python->ExecuteWithLock(
            [&]()
            {
                BridgeValues values;
                values.Activate();
                rapidjson::Document document;
                const AZStd::string atLimit(MaxStringBytes, 'a');
                auto encoded = values.Encode(pybind11::str(atLimit.c_str(), atLimit.size()), nullptr, document.GetAllocator());
                ASSERT_TRUE(encoded.IsSuccess()) << encoded.GetError().c_str();
                EXPECT_EQ(encoded.GetValue().GetStringLength(), MaxStringBytes);

                const AZStd::string tooLong(MaxStringBytes + 1, 'a');
                EXPECT_FALSE(values.Encode(pybind11::str(tooLong.c_str(), tooLong.size()), nullptr, document.GetAllocator()).IsSuccess());
            });
    }

    TEST_F(BehaviorBridgeTests, BrowseWalksModulesOwnersAndMembersInPages)
    {
        // The root lists modules, including azlmbr's fallback module for buses without a Module attribute.
        auto root = Call("binding_browse", R"({"limit":100})");
        ASSERT_FALSE(root.HasMember("error"));
        AZStd::unordered_set<AZStd::string> modules;
        for (const Json& child : root["children"].GetArray())
        {
            EXPECT_STREQ(child["kind"].GetString(), "Module");
            modules.emplace(child["name"].GetString());
        }
        for (const char* module : { "bridge_test", "bridge_cap", "scriptcanvas", "bus", "globals" })
        {
            EXPECT_TRUE(modules.contains(module)) << module;
        }

        auto scriptCanvas = Call("binding_browse", R"({"path":"scriptcanvas"})");
        ASSERT_EQ(scriptCanvas["children"].Size(), 1);
        EXPECT_STREQ(scriptCanvas["children"][0]["name"].GetString(), "automation");
        EXPECT_STREQ(scriptCanvas["children"][0]["path"].GetString(), "scriptcanvas.automation");
        EXPECT_EQ(scriptCanvas["children"][0]["childCount"].GetUint64(), 1);

        auto bus = Call("binding_browse", R"({"path":"scriptcanvas.automation"})");
        ASSERT_EQ(bus["children"].Size(), 1);
        EXPECT_STREQ(bus["children"][0]["kind"].GetString(), "Bus");
        EXPECT_STREQ(bus["children"][0]["binding"].GetString(), "bus/GraphFixtureBus");
        EXPECT_EQ(bus["children"][0]["childCount"].GetUint64(), 2);

        // Members come one page at a time, sorted by name, with their tooltips.
        auto first = Call("binding_browse", R"({"path":"scriptcanvas.automation.GraphFixtureBus","limit":1})");
        ASSERT_EQ(first["children"].Size(), 1);
        EXPECT_STREQ(first["children"][0]["name"].GetString(), "Count");
        EXPECT_STREQ(first["children"][0]["kind"].GetString(), "Event");
        EXPECT_EQ(first["next"].GetUint64(), 1);
        EXPECT_EQ(first["total"].GetUint64(), 2);
        auto second = Call("binding_browse", R"({"path":"scriptcanvas.automation.GraphFixtureBus","limit":1,"after":1})");
        ASSERT_EQ(second["children"].Size(), 1);
        EXPECT_STREQ(second["children"][0]["name"].GetString(), "Describe");
        EXPECT_STREQ(second["children"][0]["tooltip"].GetString(), "Describes a graph");
        EXPECT_STREQ(second["children"][0]["binding"].GetString(), "bus/GraphFixtureBus/event/Describe");
        auto past = Call("binding_browse", R"({"path":"scriptcanvas.automation.GraphFixtureBus","after":5})");
        EXPECT_EQ(past["children"].Size(), 0);
        EXPECT_EQ(past["next"].GetUint64(), 2);

        // Launcher-only bindings are not editor automation, and a partial name is not a path.
        for (const char* path : { "globals.BridgeFixtureLauncherOnly", "scriptcanvas.auto", "missing" })
        {
            auto refused = Call("binding_browse", AZStd::string::format(R"({"path":"%s"})", path).c_str());
            EXPECT_TRUE(refused.HasMember("error")) << path;
        }
    }

    TEST_F(BehaviorBridgeTests, SearchFiltersByModuleAndItsSubModules)
    {
        for (const char* module : { "scriptcanvas.automation", "scriptcanvas" })
        {
            auto found = Call("binding_search", AZStd::string::format(R"({"query":"Describe","module":"%s"})", module).c_str());
            ASSERT_GE(found["matches"].Size(), 1u) << module;
            for (const Json& match : found["matches"].GetArray())
            {
                EXPECT_EQ(AZStd::string_view(match["binding"].GetString()).find("bus/GraphFixtureBus"), 0u) << module;
            }
        }
        auto partial = Call("binding_search", R"({"query":"Describe","module":"scriptcanvas.auto"})");
        EXPECT_EQ(partial["total"].GetUint64(), 0);
    }

    TEST_F(BehaviorBridgeTests, OpenModuleAddsTypedToolsThatCallLikeBindingCall)
    {
        constexpr const char* DescribeTool = "scriptcanvas.automation.GraphFixtureBus.Describe";
        auto countTools = [this]()
        {
            rapidjson::Document tools(rapidjson::kArrayType);
            m_bridge.AddTools(tools, tools.GetAllocator());
            return tools.Size();
        };
        const auto revision = m_bridge.GetToolsRevision();
        const auto baseTools = countTools();

        auto opened = Call("binding_open_module", R"({"module":"scriptcanvas.automation"})");
        ASSERT_FALSE(opened.HasMember("error")) << opened["error"].GetString();
        ASSERT_EQ(opened["tools"].Size(), 2);
        EXPECT_STREQ(opened["tools"][0].GetString(), "scriptcanvas.automation.GraphFixtureBus.Count");
        EXPECT_STREQ(opened["tools"][1].GetString(), DescribeTool);
        EXPECT_NE(m_bridge.GetToolsRevision(), revision);
        EXPECT_EQ(countTools(), baseTools + 2);

        // The tools/list entry is typed from the native signature and documented from the tooltip.
        rapidjson::Document tools(rapidjson::kArrayType);
        m_bridge.AddTools(tools, tools.GetAllocator());
        const Json& describe = tools[tools.Size() - 1];
        EXPECT_STREQ(describe["name"].GetString(), DescribeTool);
        EXPECT_EQ(AZStd::string_view(describe["description"].GetString()).find("Describes a graph"), 0u);
        EXPECT_NE(AZStd::string_view(describe["description"].GetString()).find("bus/GraphFixtureBus/event/Describe"), AZStd::string::npos);
        const Json& schema = describe["inputSchema"];
        EXPECT_FALSE(schema["additionalProperties"].GetBool());
        ASSERT_EQ(schema["properties"].MemberCount(), 1);
        const Json& argument = schema["properties"].MemberBegin()->value;
        EXPECT_STREQ(argument["type"].GetString(), "string");
        ASSERT_EQ(schema["required"].Size(), 1);

        // A typed call and the equivalent binding_call reach the same native event.
        const AZStd::string name = schema["properties"].MemberBegin()->name.GetString();
        auto typed = Call(DescribeTool, AZStd::string::format(R"({"%s":"g1"})", name.c_str()).c_str());
        ASSERT_FALSE(typed.HasMember("error"));
        EXPECT_STREQ(typed["value"].GetString(), "graph g1");
        auto direct = Call("binding_call", R"({"binding":"bus/GraphFixtureBus/event/Describe","args":["g1"]})");
        ASSERT_FALSE(direct.HasMember("error"));
        EXPECT_STREQ(direct["value"].GetString(), "graph g1");
        auto count = Call("scriptcanvas.automation.GraphFixtureBus.Count", "{}");
        EXPECT_EQ(count["value"].GetInt(), 3);

        for (const AZStd::string& text : { AZStd::string("{}"), AZStd::string::format(R"({"%s":"g1","extra":1})", name.c_str()),
                                           AZStd::string("[]") })
        {
            rapidjson::Document input;
            input.Parse(text.c_str());
            rapidjson::Document result(rapidjson::kObjectType);
            AZStd::string detail;
            EXPECT_EQ(m_bridge.Invoke(DescribeTool, input, result, result.GetAllocator(), detail), ToolProvider::Result::InvalidArguments)
                << text.c_str();
            EXPECT_FALSE(detail.empty());
        }

        // An open module, or one overlapping it, cannot be opened again.
        EXPECT_TRUE(Call("binding_open_module", R"({"module":"scriptcanvas.automation"})").HasMember("error"));
        EXPECT_TRUE(Call("binding_open_module", R"({"module":"scriptcanvas"})").HasMember("error"));
        EXPECT_TRUE(Call("binding_open_module", R"({"module":"missing"})").HasMember("error"));

        const auto openRevision = m_bridge.GetToolsRevision();
        auto closed = Call("binding_close_module", R"({"module":"scriptcanvas.automation"})");
        ASSERT_FALSE(closed.HasMember("error"));
        EXPECT_EQ(closed["closedTools"].GetUint64(), 2);
        EXPECT_NE(m_bridge.GetToolsRevision(), openRevision);
        EXPECT_EQ(countTools(), baseTools);
        EXPECT_TRUE(Call("binding_close_module", R"({"module":"scriptcanvas.automation"})").HasMember("error"));

        rapidjson::Document input;
        input.Parse(R"({})");
        rapidjson::Document result(rapidjson::kObjectType);
        AZStd::string detail;
        EXPECT_EQ(m_bridge.Invoke(DescribeTool, input, result, result.GetAllocator(), detail), ToolProvider::Result::Unknown);
    }

    TEST_F(BehaviorBridgeTests, OpenToolsAreCappedAndNamesAreValidStableAndUnique)
    {
        const auto revision = m_bridge.GetToolsRevision();
        auto capped = Call("binding_open_module", R"({"module":"bridge_cap"})");
        ASSERT_TRUE(capped.HasMember("error"));
        EXPECT_NE(AZStd::string_view(capped["error"].GetString()).find("at most 128"), AZStd::string::npos);
        EXPECT_EQ(m_bridge.GetToolsRevision(), revision);

        EXPECT_EQ(ModuleTools::MakeToolName("scriptcanvas.automation.Bus.Event", "bus/Bus/event/Event"), "scriptcanvas.automation.Bus.Event");

        // Characters MCP does not allow, and names over the length limit, end in a hash of the binding ID instead.
        const AZStd::string spaced = ModuleTools::MakeToolName("my module.Bus.Get Value", "bus/Bus/event/Get Value");
        EXPECT_EQ(spaced.find("my_module.Bus.Get_Value-"), 0u);
        const AZStd::string longPath(200, 'a');
        const AZStd::string first = ModuleTools::MakeToolName(longPath, "global/method/one");
        const AZStd::string second = ModuleTools::MakeToolName(longPath, "global/method/two");
        EXPECT_EQ(first.size(), ModuleTools::MaxToolNameLength);
        EXPECT_NE(first, second);
        EXPECT_EQ(first, ModuleTools::MakeToolName(longPath, "global/method/one"));
        for (const AZStd::string& name : { spaced, first })
        {
            for (const char character : name)
            {
                const bool letter = (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z');
                const bool digit = character >= '0' && character <= '9';
                EXPECT_TRUE(letter || digit || character == '_' || character == '.' || character == '-') << name.c_str();
            }
        }
    }

    TEST_F(BehaviorBridgeTests, PythonExecuteCapturesOutputErrorsAndTimeouts)
    {
        constexpr AZ::s64 TimeoutMs = 10000;
        constexpr AZ::s64 ShortTimeoutMs = 200;
        auto run = [](const char* code, AZ::s64 timeoutMs)
        {
            rapidjson::Document data(rapidjson::kObjectType);
            auto ran = PythonExecute::Run(code, timeoutMs, data, data.GetAllocator());
            EXPECT_TRUE(ran.IsSuccess()) << ran.GetError().c_str();
            return data;
        };

        auto printed = run("import sys\nprint('hi')\nsys.stderr.write('warn')", TimeoutMs);
        EXPECT_STREQ(printed["stdout"].GetString(), "hi\n");
        EXPECT_STREQ(printed["stderr"].GetString(), "warn");
        EXPECT_FALSE(printed.HasMember("traceback"));
        EXPECT_FALSE(printed["timedOut"].GetBool());
        EXPECT_FALSE(printed["truncated"].GetBool());

        auto raised = run("print('before')\nraise ValueError('bad value')", TimeoutMs);
        EXPECT_STREQ(raised["stdout"].GetString(), "before\n");
        EXPECT_NE(AZStd::string_view(raised["traceback"].GetString()).find("ValueError: bad value"), AZStd::string::npos);

        // Each run gets a fresh namespace.
        run("leftover = 1", TimeoutMs);
        auto fresh = run("print(leftover)", TimeoutMs);
        EXPECT_NE(AZStd::string_view(fresh["traceback"].GetString()).find("NameError"), AZStd::string::npos);

        auto looped = run("while True:\n    pass", ShortTimeoutMs);
        EXPECT_TRUE(looped["timedOut"].GetBool());
        EXPECT_NE(AZStd::string_view(looped["traceback"].GetString()).find("PythonExecuteTimeout"), AZStd::string::npos);

        auto flooded = run("print('x' * 70000)", TimeoutMs);
        EXPECT_TRUE(flooded["truncated"].GetBool());
        EXPECT_EQ(flooded["stdout"].GetStringLength(), PythonExecute::MaxOutputCharacters);

        // The editor's own streams and trace function are restored afterwards.
        auto* python = AZ::Interface<AzToolsFramework::EditorPythonEventsInterface>::Get();
        python->ExecuteWithLock(
            []()
            {
                auto sys = pybind11::module::import("sys");
                EXPECT_NE(sys.attr("stdout").attr("__class__").attr("__name__").cast<std::string>(), "BoundedOutput");
                EXPECT_TRUE(sys.attr("gettrace")().is_none());
            });
    }

    TEST_F(BehaviorBridgeTests, ScreenshotRefusesAnUnknownPanelWithTheKnownNames)
    {
        EditorTools tools(m_bridge);
        rapidjson::Document input;
        input.Parse(R"({"target":"panel","panel":"Missing Pane"})");
        rapidjson::Document data(rapidjson::kObjectType);
        AZStd::string detail;
        EXPECT_EQ(tools.Invoke(ScreenshotTool, input, data, data.GetAllocator(), detail), ToolProvider::Result::Complete) << detail.c_str();
        ASSERT_TRUE(data.HasMember("error"));
        ASSERT_TRUE(data.HasMember("panels"));
        ASSERT_EQ(data["panels"].Size(), 2);
        EXPECT_STREQ(data["panels"][1].GetString(), "Entity Inspector");
        EXPECT_FALSE(data.HasMember("partialChangesPossible"));
    }
} // namespace LocalMcp
