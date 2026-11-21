/*
 * Copyright (c) Contributors to the Open 3D Engine Project.
 * For complete copyright and license terms please see the LICENSE at the root of this distribution.
 *
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 *
 */

#include "Editor/EditorTools.h"

#include <AzCore/UnitTest/TestTypes.h>
#include <AzCore/std/containers/unordered_set.h>
#include <AzTest/AzTest.h>

namespace LocalMcp
{
    class EditorToolsTests : public UnitTest::LeakDetectionFixture
    {
    protected:
        //! Invokes tool with the JSON arguments text on tools and returns the result; detail receives the refusal reason.
        static ToolProvider::Result Invoke(EditorTools& tools, AZStd::string_view tool, const char* arguments, AZStd::string& detail)
        {
            rapidjson::Document args;
            args.Parse(arguments);
            EXPECT_FALSE(args.HasParseError()) << arguments;
            rapidjson::Document data(rapidjson::kObjectType);
            return tools.Invoke(tool, args, data, data.GetAllocator(), detail);
        }
    };

    TEST_F(EditorToolsTests, EveryAdvertisedRecipeRejectsMalformedRequestsBeforeCallingAnOwner)
    {
        BehaviorBridge bridge; // Intentionally no interpreter/owners: invalid requests must never reach them.
        EditorTools tools(bridge);
        rapidjson::Document schemas;
        schemas.Parse(EditorToolsSchemas);
        ASSERT_FALSE(schemas.HasParseError()) << "Schema parse code " << schemas.GetParseError() << " at " << schemas.GetErrorOffset();
        ASSERT_TRUE(schemas.HasMember("tools"));
        ASSERT_TRUE(schemas["tools"].IsArray());

        AZStd::unordered_set<AZStd::string> names;
        for (const Json& schema : schemas["tools"].GetArray())
        {
            const char* name = schema["name"].GetString();
            EXPECT_TRUE(names.emplace(name).second);

            // python_execute is disabled by default, so it is not a tool at all.
            rapidjson::Document data(rapidjson::kObjectType);
            AZStd::string detail;
            const Json invalid(17);
            const ToolProvider::Result result = tools.Invoke(name, invalid, data, data.GetAllocator(), detail);
            if (AZStd::string_view(name) == PythonExecuteTool)
            {
                EXPECT_EQ(result, ToolProvider::Result::Unknown);
                continue;
            }
            EXPECT_EQ(result, ToolProvider::Result::InvalidArguments) << name;
            EXPECT_FALSE(detail.empty());
            EXPECT_FALSE(data.HasMember("partialChangesPossible"));
        }

        rapidjson::Document args(rapidjson::kObjectType);
        rapidjson::Document data(rapidjson::kObjectType);
        AZStd::string detail;
        EXPECT_EQ(tools.Invoke("unknown_editor_recipe", args, data, data.GetAllocator(), detail), ToolProvider::Result::Unknown);
    }

    TEST_F(EditorToolsTests, InvalidPathsSpacesAndEmptyMutationsAreRefusedBeforeScheduling)
    {
        BehaviorBridge bridge;
        EditorTools tools(bridge);
        for (
            const char* request :
            { R"({"tool":"level_create","args":{"path":"a\u0000b"}})",
              R"({"tool":"transform_set","args":{"entity":{},"space":"screen","transform":{}}})",
              R"({"tool":"component_add","args":{"entity":{},"types":[]}})",
              R"({"tool":"entity_delete","args":{"entities":[]}})",
              R"({"tool":"component_set","args":{"component":{},"path":"field"}})",
              R"({"tool":"editor_play","args":{"unexpected":true}})",
              R"({"tool":"editor_info","args":{"unexpected":true}})",
              R"({"tool":"asset_import","args":{"source_path":"","dest_path":"Assets/new.png"}})",
              R"({"tool":"screenshot","args":{"target":"screen"}})",
              R"({"tool":"screenshot","args":{"target":"panel"}})",
              R"({"tool":"screenshot","args":{"target":"panel","panel":""}})",
              R"({"tool":"screenshot","args":{"target":"window","panel":"Console"}})",
              R"({"tool":"screenshot","args":{"target":"viewport","position":{"$type":"Vector3","value":[0,0,0]}}})",
              R"({"tool":"screenshot","args":{"target":"window","position":{"$type":"Vector3","value":[0,0,0]},"look_at":{"$type":"Vector3","value":[1,0,0]}}})",
              R"({"tool":"screenshot","args":{"target":"viewport","position":{"$type":"Vector3","value":[1,2,3]},"look_at":{"$type":"Vector3","value":[1,2,3]}}})",
              R"({"tool":"screenshot","args":{"target":"viewport","position":{"$type":"Vector3","value":[1,2]},"look_at":{"$type":"Vector3","value":[1,0,0]}}})",
              R"({"tool":"screenshot","args":{"target":"viewport","position":{"$type":"Vector2","value":[1,2,3]},"look_at":{"$type":"Vector3","value":[1,0,0]}}})",
              R"({"tool":"screenshot","args":{"target":"viewport","position":{"$type":"Vector3","value":[1,"2",3]},"look_at":{"$type":"Vector3","value":[1,0,0]}}})",
              R"({"tool":"screenshot","args":{"target":"viewport","position":{"$type":"Vector3","value":[0,0,0],"z":1},"look_at":{"$type":"Vector3","value":[1,0,0]}}})" })
        {
            rapidjson::Document input;
            input.Parse(request);
            ASSERT_FALSE(input.HasParseError());

            rapidjson::Document data(rapidjson::kObjectType);
            AZStd::string detail;
            EXPECT_EQ(
                tools.Invoke(input["tool"].GetString(), input["args"], data, data.GetAllocator(), detail),
                ToolProvider::Result::InvalidArguments)
                << request;
            EXPECT_FALSE(detail.empty());
        }
    }

    TEST_F(EditorToolsTests, FreeFormValuesAreBoundedAsStrings)
    {
        constexpr size_t MaxValueLength = 4096;
        BehaviorBridge bridge;
        EditorTools tools(bridge);

        const AZStd::string tooLong(MaxValueLength + 1, 'v');
        const AZStd::string request = AZStd::string::format(R"({"component":{},"path":"field","value":"%s"})", tooLong.c_str());
        AZStd::string detail;
        EXPECT_EQ(Invoke(tools, ComponentSetTool, request.c_str(), detail), ToolProvider::Result::InvalidArguments);
        EXPECT_FALSE(detail.empty());

        // At the bound the arguments parse; the call then fails only because no editor owner is running.
        const AZStd::string atLimit(MaxValueLength, 'v');
        rapidjson::Document args;
        args.Parse(AZStd::string::format(R"({"component":{},"path":"field","value":"%s"})", atLimit.c_str()).c_str());
        EXPECT_TRUE(ComponentSetArgs::Parse(args).IsSuccess());
    }

    TEST_F(EditorToolsTests, GeneratedSchemaMatchesGeneratedParseLimits)
    {
        rapidjson::Document schemas;
        schemas.Parse(BindingToolsSchemas);
        ASSERT_FALSE(schemas.HasParseError());

        const Json* browse = nullptr;
        for (const Json& schema : schemas["tools"].GetArray())
        {
            if (AZStd::string_view(schema["name"].GetString()) == BindingBrowseTool)
            {
                browse = &schema["inputSchema"]["properties"];
            }
        }
        ASSERT_NE(browse, nullptr);

        const uint64_t minimumLimit = (*browse)["limit"]["minimum"].GetUint64();
        const uint64_t maximumLimit = (*browse)["limit"]["maximum"].GetUint64();
        const uint64_t maximumAfter = (*browse)["after"]["maximum"].GetUint64();
        const size_t maximumPath = aznumeric_cast<size_t>((*browse)["path"]["maxLength"].GetUint64());
        auto parses = [](const AZStd::string& text)
        {
            rapidjson::Document args;
            args.Parse(text.c_str());
            return BindingBrowseArgs::Parse(args).IsSuccess();
        };

        EXPECT_TRUE(parses(AZStd::string::format(R"({"limit":%llu})", static_cast<unsigned long long>(minimumLimit))));
        EXPECT_FALSE(parses(AZStd::string::format(R"({"limit":%llu})", static_cast<unsigned long long>(minimumLimit - 1))));
        EXPECT_TRUE(parses(AZStd::string::format(R"({"limit":%llu})", static_cast<unsigned long long>(maximumLimit))));
        EXPECT_FALSE(parses(AZStd::string::format(R"({"limit":%llu})", static_cast<unsigned long long>(maximumLimit + 1))));
        EXPECT_TRUE(parses(AZStd::string::format(R"({"after":%llu})", static_cast<unsigned long long>(maximumAfter))));
        EXPECT_FALSE(parses(AZStd::string::format(R"({"after":%llu})", static_cast<unsigned long long>(maximumAfter + 1))));
        EXPECT_TRUE(parses(AZStd::string::format(R"({"path":"%s"})", AZStd::string(maximumPath, 'p').c_str())));
        EXPECT_FALSE(parses(AZStd::string::format(R"({"path":"%s"})", AZStd::string(maximumPath + 1, 'p').c_str())));
    }

    TEST_F(EditorToolsTests, PythonExecuteIsAbsentUnlessTheSettingEnablesIt)
    {
        constexpr AZ::s64 ConnectionTimeoutMs = 10000;
        constexpr size_t MaxCodeLength = 32768;
        BehaviorBridge bridge;
        EditorTools tools(bridge);
        auto listed = [&tools]()
        {
            rapidjson::Document list(rapidjson::kArrayType);
            tools.AddTools(list, list.GetAllocator());
            for (const Json& tool : list.GetArray())
            {
                if (AZStd::string_view(tool["name"].GetString()) == PythonExecuteTool)
                {
                    return true;
                }
            }
            return false;
        };

        AZStd::string detail;
        EXPECT_FALSE(listed());
        EXPECT_EQ(Invoke(tools, PythonExecuteTool, R"({"code":"pass"})", detail), ToolProvider::Result::Unknown);

        tools.Activate(ConnectionTimeoutMs, true);
        EXPECT_TRUE(listed());

        // The code length is bounded, and the timeout must end before the connection deadline.
        const AZStd::string tooLong = AZStd::string::format(R"({"code":"%s"})", AZStd::string(MaxCodeLength + 1, 'x').c_str());
        for (const char* text : { R"({})", R"({"code":""})", R"({"code":"pass","timeout_ms":0})", R"({"code":"pass","timeout_ms":10000})",
                                  R"({"code":"pass","unexpected":1})", tooLong.c_str() })
        {
            detail.clear();
            EXPECT_EQ(Invoke(tools, PythonExecuteTool, text, detail), ToolProvider::Result::InvalidArguments) << text;
            EXPECT_FALSE(detail.empty());
        }

        // A valid call is queued for the main loop; this test runs none, so deactivation cancels it.
        EXPECT_EQ(Invoke(tools, PythonExecuteTool, R"({"code":"pass","timeout_ms":1000})", detail), ToolProvider::Result::Pending);
        tools.Deactivate();
    }

    TEST_F(EditorToolsTests, MissingEditorServicesRemainAnExplicitFailure)
    {
        BehaviorBridge bridge;
        EditorTools tools(bridge);
        rapidjson::Document args(rapidjson::kObjectType);
        rapidjson::Document data(rapidjson::kObjectType);
        AZStd::string detail;
        EXPECT_EQ(tools.Invoke(EditorStateTool, args, data, data.GetAllocator(), detail), ToolProvider::Result::Complete);
        ASSERT_TRUE(data.HasMember("error"));
        EXPECT_GT(data["error"].GetStringLength(), 0);
        EXPECT_FALSE(data.HasMember("state"));
    }

    TEST_F(EditorToolsTests, ImportValidatesConnectionDeadlineAndAbsoluteSourceContract)
    {
        constexpr AZ::s64 ConnectionTimeoutMs = 10000;
        AssetImport importer;
        for (const char* text : { R"({})",
                                  R"({"source_path":"x","dest_path":"y","timeout_ms":10000})",
                                  R"({"source_path":"x","dest_path":"y","timeout_ms":0})",
                                  R"({"source_path":"x\u0000y","dest_path":"z"})",
                                  R"({"source_path":"relative.png","dest_path":"Assets/new.png"})" })
        {
            rapidjson::Document args;
            args.Parse(text);
            ASSERT_FALSE(args.HasParseError());

            // Arguments the schema refuses never reach the importer; the rest must be refused by it.
            auto parsed = AssetImportArgs::Parse(args);
            if (parsed.IsSuccess())
            {
                rapidjson::Document data(rapidjson::kObjectType);
                AZStd::string detail;
                EXPECT_EQ(
                    importer.Begin(parsed.GetValue(), data, data.GetAllocator(), detail, ConnectionTimeoutMs),
                    ToolProvider::Result::InvalidArguments)
                    << text;
                EXPECT_FALSE(detail.empty());
            }
            EXPECT_FALSE(importer.IsActive());
        }
    }
} // namespace LocalMcp
