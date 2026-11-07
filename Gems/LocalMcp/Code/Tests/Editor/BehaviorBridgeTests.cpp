/* Copyright (c) Contributors to the Open 3D Engine Project.
 * SPDX-License-Identifier: Apache-2.0 OR MIT */
#include "Editor/BehaviorBridge.h"
#include "Editor/BindingRecipe.h"
#include <Tests/PythonTestingUtility.h>
#include <AzCore/Script/ScriptContextAttributes.h>
#include <AzCore/Math/MathReflection.h>
#include <AzCore/Math/Vector3.h>
#include <AzCore/std/parallel/thread.h>
#include <AzCore/std/limits.h>
#include <AzCore/std/containers/unordered_set.h>

namespace LocalMcp { enum class BridgeFixtureMode : AZ::u8 { Active = 7 }; }
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
        explicit BridgeFixtureValue(int value) : m_value(value) {}
        int Add(int value) { return m_value += value; }
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

    class BehaviorBridgeTests : public UnitTest::PythonTestingFixture, private BridgeFixtureBus::Handler
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
                ->Method("Ambiguous", [](int value) { return value; })
                ->Method("Ambiguous", [](double value) { return static_cast<int>(value); });
            context->Method("BridgeFixtureEcho", [](int value) { return value; })
                ->Attribute(AZ::Script::Attributes::Scope, AZ::Script::Attributes::ScopeFlags::Automation)
                ->Attribute(AZ::Script::Attributes::ToolTip, "fixture echo documentation");
            context->Method("BridgeFixtureCString", [](const char* value) { return AZStd::string_view(value).size(); })
                ->Attribute(AZ::Script::Attributes::Scope, AZ::Script::Attributes::ScopeFlags::Automation);
            context->Method("BridgeFixtureLauncherOnly", []() { return 42; })
                ->Attribute(AZ::Script::Attributes::Scope, AZ::Script::Attributes::ScopeFlags::Launcher);
            context->EnumProperty<BridgeFixtureMode::Active>("BridgeFixtureActive")
                ->Attribute(AZ::Script::Attributes::Scope, AZ::Script::Attributes::ScopeFlags::Automation);
            context->Method("BridgeFixtureMode", [](BridgeFixtureMode value) { return value; })
                ->Attribute(AZ::Script::Attributes::Scope, AZ::Script::Attributes::ScopeFlags::Automation);
            context->Method("BridgeFixtureNumbers", [](const BridgeNumbers& values) { return values; })
                ->Attribute(AZ::Script::Attributes::Scope, AZ::Script::Attributes::ScopeFlags::Automation);
            context->Method("BridgeFixtureSet", [](const BridgeSet& values) { return values; })
                ->Attribute(AZ::Script::Attributes::Scope, AZ::Script::Attributes::ScopeFlags::Automation);
            context->Method("BridgeFixtureOutcome", [](const BridgeOutcome& value) { return value; })
                ->Attribute(AZ::Script::Attributes::Scope, AZ::Script::Attributes::ScopeFlags::Automation);
            // Deliberately no SerializeContext registration: real editor Outcome events are behavior-only.
            context->Method("BehaviorBooleanOutcome", [](bool success) -> BehaviorBooleanOutcome
                { return success ? BehaviorBooleanOutcome(AZ::Success(true)) : BehaviorBooleanOutcome(AZ::Failure(AZStd::string("refused"))); })
                ->Attribute(AZ::Script::Attributes::Scope, AZ::Script::Attributes::ScopeFlags::Automation);
            context->Method("BehaviorNumbersOutcome", []() -> BehaviorNumbersOutcome { return AZ::Success(BridgeNumbers{2, 4}); })
                ->Attribute(AZ::Script::Attributes::Scope, AZ::Script::Attributes::ScopeFlags::Automation);
            context->Method("BehaviorVoidOutcome", []() -> BehaviorVoidOutcome { return AZ::Success(); })
                ->Attribute(AZ::Script::Attributes::Scope, AZ::Script::Attributes::ScopeFlags::Automation);
            context->EBus<BridgeFixtureBus>("BridgeFixtureBus")
                ->Attribute(AZ::Script::Attributes::Scope, AZ::Script::Attributes::ScopeFlags::Automation)
                ->Event("Add", &BridgeFixtureRequests::Add);
            m_python = AZStd::make_unique<AZ::Entity>();
            Activate(*m_python);
            SimulateEditorBecomingInitialized();
            m_bridge.Activate();
            BridgeFixtureBus::Handler::BusConnect(7);
        }
        void TearDown() override
        {
            BridgeFixtureBus::Handler::BusDisconnect();
            m_bridge.Deactivate();
            m_python.reset();
            PythonTestingFixture::TearDown();
        }
        int Add(int value) override { return value + 7; }
        rapidjson::Document Call(const char* tool, const char* arguments)
        {
            rapidjson::Document input;
            input.Parse(arguments);
            EXPECT_FALSE(input.HasParseError());
            rapidjson::Document result(rapidjson::kObjectType);
            AZStd::string detail;
            EXPECT_EQ(m_bridge.Invoke(tool, input, result, result.GetAllocator(), detail), ToolProvider::Result::Complete) << detail.c_str();
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
        auto result = recipe.Call("global/method/BridgeFixtureEcho", allocator, {&input});
        EXPECT_EQ(result.GetInt(), 42);
        const Json lossy(4.5);
        EXPECT_THROW(recipe.Call("global/method/BridgeFixtureEcho", allocator, {&lossy}), BridgeError);
        rapidjson::Document outcome;
        const auto type = azrtti_typeid<BridgeOutcome>().ToString<AZStd::string>();
        outcome.Parse(AZStd::string::format(R"({"$type":"Outcome","type":"%s","success":true,"value":9})", type.c_str()).c_str());
        EXPECT_EQ(recipe.Call("global/method/BridgeFixtureOutcome", allocator, {&outcome}).GetInt(), 9);
        outcome.Parse(AZStd::string::format(R"({"$type":"Outcome","type":"%s","success":false,"error":"native refusal"})", type.c_str()).c_str());
        EXPECT_THROW(recipe.Call("global/method/BridgeFixtureOutcome", allocator, {&outcome}), BridgeError);
    }

    TEST_F(BehaviorBridgeTests, RecipeNormalizationUsesTheExistingCodecBeforeMutation)
    {
        rapidjson::Document input;
        input.Parse(R"({"$type":"Vector3","value":[3,4,0]})");
        auto normalized = m_bridge.NormalizeValue(input, azrtti_typeid<AZ::Vector3>(), input.GetAllocator());
        EXPECT_EQ(normalized["value"][0].GetFloat(), 3.0f);
        EXPECT_EQ(normalized["value"][1].GetFloat(), 4.0f);
        EXPECT_THROW(m_bridge.NormalizeValue(input, azrtti_typeid<int>(), input.GetAllocator()), BridgeError);
        input.Parse(R"({"$type":"EntityId","value":"0"})");
        EXPECT_THROW(m_bridge.NormalizeValue(input, azrtti_typeid<AZ::EntityId>(), input.GetAllocator()), BridgeError);
    }

    TEST_F(BehaviorBridgeTests, CallsRejectLossyConversionAndRequireAmbiguousOverload)
    {
        auto echo = Call("binding_call", R"({"binding":"global/method/BridgeFixtureEcho","args":[42]})");
        ASSERT_FALSE(echo.HasMember("error"));
        EXPECT_EQ(echo["value"].GetInt(), 42);
        for (const char* value : {"1.5", "true", "2147483648", "9007199254740992"})
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
        auto getArgs = AZStd::string::format(R"({"binding":"class/BridgeFixtureValue/property/value","target":{"$handle":"%s"}})", handle.c_str());
        auto first = Call("binding_get", getArgs.c_str());
        ASSERT_FALSE(first.HasMember("error"));
        EXPECT_EQ(first["value"].GetInt(), 8);
        auto setArgs = AZStd::string::format(R"({"binding":"class/BridgeFixtureValue/property/value","target":{"$handle":"%s"},"value":12})", handle.c_str());
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
        python->ExecuteWithLock([&]()
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
            auto encoded = values.Encode(values.Decode(input, &expected), &expected, input.GetAllocator());
            ASSERT_TRUE(encoded.HasMember("value"));
            EXPECT_EQ(encoded["value"][2].GetFloat(), 3);
            input.Parse(R"({"$type":"uint64","value":"18446744073709551615"})");
            expected.m_typeId = azrtti_typeid<AZ::u64>();
            step = "uint64 roundtrip";
            encoded = values.Encode(values.Decode(input, &expected), &expected, input.GetAllocator());
            EXPECT_STREQ(encoded["value"].GetString(), "18446744073709551615");
            input.Parse(R"({"$type":"EntityId","value":"123"})");
            expected.m_typeId = azrtti_typeid<AZ::EntityId>();
            EXPECT_THROW(values.Decode(input, &expected), std::runtime_error);
            input.Parse(R"({"$type":"Vector3","value":[16777217,0,0]})");
            expected.m_typeId = azrtti_typeid<AZ::Vector3>();
            EXPECT_THROW(values.Decode(input, &expected), std::runtime_error);
            input.Parse(R"({"$type":"uint64","value":"18446744073709551616"})");
            expected.m_typeId = azrtti_typeid<AZ::u64>();
            EXPECT_THROW(values.Decode(input, &expected), std::runtime_error);
            values.Clear();
            }
            catch (const std::exception& error)
            {
                ADD_FAILURE() << step << ": " << error.what();
            }
        });
    }

    TEST_F(BehaviorBridgeTests, WrongThreadRefusesBeforeInterpreterAccess)
    {
        AZStd::thread worker([&]()
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
        for (const char* value : {"256", "-1", "true"})
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
        auto args = AZStd::string::format(R"({"binding":"global/method/BridgeFixtureOutcome","args":[{"$type":"Outcome","type":"%s","success":true,"value":42}]})",
            azrtti_typeid<BridgeOutcome>().ToString<AZStd::string>().c_str());
        auto outcome = Call("binding_call", args.c_str());
        ASSERT_FALSE(outcome.HasMember("error"));
        EXPECT_TRUE(outcome["value"]["success"].GetBool());
        EXPECT_EQ(outcome["value"]["value"].GetInt(), 42);
    }
}
