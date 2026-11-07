/* Copyright (c) Contributors to the Open 3D Engine Project.
 * SPDX-License-Identifier: Apache-2.0 OR MIT */
#include "Editor/EditorTools.h"
#include "Editor/EditorToolSchemas.h"
#include <AzCore/UnitTest/TestTypes.h>
#include <AzCore/std/containers/unordered_set.h>
#include <AzTest/AzTest.h>

namespace LocalMcp
{
    class EditorToolsTests : public UnitTest::LeakDetectionFixture {};

    TEST_F(EditorToolsTests, EveryAdvertisedRecipeRejectsMalformedRequestsBeforeCallingAnOwner)
    {
        BehaviorBridge bridge; // Intentionally no interpreter/owners: invalid requests must never reach them.
        EditorTools tools(bridge);
        rapidjson::Document schemas;
        schemas.Parse(EditorToolSchemas);
        ASSERT_FALSE(schemas.HasParseError()) << "Schema parse code " << schemas.GetParseError() << " at " << schemas.GetErrorOffset();
        ASSERT_TRUE(schemas.IsArray());
        AZStd::unordered_set<AZStd::string> names;
        for (const auto& schema : schemas.GetArray())
        {
            const char* name = schema["name"].GetString();
            EXPECT_TRUE(names.emplace(name).second);
            rapidjson::Document data(rapidjson::kObjectType);
            AZStd::string detail;
            const Json invalid(17);
            EXPECT_EQ(tools.Invoke(name, invalid, data, data.GetAllocator(), detail), ToolProvider::Result::InvalidArguments) << name;
            EXPECT_FALSE(detail.empty());
            EXPECT_FALSE(data.HasMember("partialChangesPossible"));
        }
        rapidjson::Document args(rapidjson::kObjectType), data(rapidjson::kObjectType);
        AZStd::string detail;
        EXPECT_EQ(tools.Invoke("unknown_editor_recipe", args, data, data.GetAllocator(), detail), ToolProvider::Result::Unknown);
    }

    TEST_F(EditorToolsTests, InvalidPathsSpacesAndEmptyMutationsAreRefusedBeforeScheduling)
    {
        BehaviorBridge bridge;
        EditorTools tools(bridge);
        for (const auto& request : {
            R"({"tool":"level_create","args":{"path":"a\u0000b"}})",
            R"({"tool":"transform_set","args":{"entity":{},"space":"screen","transform":{}}})",
            R"({"tool":"component_add","args":{"entity":{},"types":[]}})",
            R"({"tool":"entity_delete","args":{"entities":[]}})",
            R"({"tool":"component_set","args":{"component":{},"path":"field"}})",
            R"({"tool":"editor_play","args":{"unexpected":true}})",
            R"({"tool":"sc_describe","args":{"graph":{"$type":"EntityId","value":"1"}}})",
            R"({"tool":"sc_connect","args":{"graph":"1","source_node":"2","source_slot":"invalid","target_node":"3","target_slot":"invalid"}})",
            R"({"tool":"sc_reload","args":{"graph":"1","discard":"yes"}})",
            R"({"tool":"sc_remove_nodes","args":{"graph":"1","nodes":[]}})",
            R"({"tool":"sc_palette","args":{"limit":257}})",
            R"({"tool":"sc_save","args":{"graph":"1","path":"a\u0000b"}})"})
        {
            rapidjson::Document input, data(rapidjson::kObjectType);
            input.Parse(request);
            ASSERT_FALSE(input.HasParseError());
            AZStd::string detail;
            EXPECT_EQ(tools.Invoke(input["tool"].GetString(), input["args"], data, data.GetAllocator(), detail),
                ToolProvider::Result::InvalidArguments) << request;
            EXPECT_FALSE(detail.empty());
        }
    }

    TEST_F(EditorToolsTests, MissingEditorServicesRemainAnExplicitFailure)
    {
        BehaviorBridge bridge;
        EditorTools tools(bridge);
        rapidjson::Document args(rapidjson::kObjectType), data(rapidjson::kObjectType);
        AZStd::string detail;
        EXPECT_EQ(tools.Invoke("editor_state", args, data, data.GetAllocator(), detail), ToolProvider::Result::Complete);
        ASSERT_TRUE(data.HasMember("error"));
        EXPECT_GT(data["error"].GetStringLength(), 0);
        EXPECT_FALSE(data.HasMember("state"));
    }

    TEST_F(EditorToolsTests, ImportValidatesConnectionDeadlineAndAbsoluteSourceContract)
    {
        AssetImport importer;
        for (const auto* text : {R"({})", R"({"source_path":"x","dest_path":"y","timeout_ms":10000})",
            R"({"source_path":"x","dest_path":"y","timeout_ms":0})", R"({"source_path":"x\u0000y","dest_path":"z"})",
            R"({"source_path":"relative.png","dest_path":"Assets/new.png"})"})
        {
            rapidjson::Document args, data(rapidjson::kObjectType);
            args.Parse(text);
            AZStd::string detail;
            EXPECT_EQ(importer.Begin(args, data, data.GetAllocator(), detail, 10000), ToolProvider::Result::InvalidArguments);
            EXPECT_FALSE(importer.IsActive());
        }
    }

    TEST_F(EditorToolsTests, ObservingAnInvalidSavedSourceDoesNotCopyOrScheduleWork)
    {
        AssetImport importer;
        rapidjson::Document data(rapidjson::kObjectType);
        EXPECT_EQ(importer.Observe("relative.scriptcanvas", 0, data, data.GetAllocator()), ToolProvider::Result::Complete);
        EXPECT_TRUE(data.HasMember("error"));
        EXPECT_FALSE(importer.IsActive());
    }
}
