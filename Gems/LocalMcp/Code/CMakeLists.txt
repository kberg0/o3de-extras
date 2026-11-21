#
# Copyright (c) Contributors to the Open 3D Engine Project.
# For complete copyright and license terms please see the LICENSE at the root of this distribution.
#
# SPDX-License-Identifier: Apache-2.0 OR MIT
#
#

ly_add_target(
    NAME ${gem_name}.Static STATIC
    NAMESPACE Gem
    FILES_CMAKE localmcp_files.cmake
    INCLUDE_DIRECTORIES PUBLIC Source
    BUILD_DEPENDENCIES PUBLIC AZ::AzCore AZ::AzNetworking 3rdParty::OpenSSL
    AUTOGEN_RULES
        *.McpTools.xml,McpTools_Header.jinja,$path/$fileprefix.McpTools.h
        *.McpTools.xml,McpTools_Source.jinja,$path/$fileprefix.McpTools.cpp
)

ly_add_target(
    NAME ${gem_name} ${PAL_TRAIT_MONOLITHIC_DRIVEN_MODULE_TYPE}
    NAMESPACE Gem
    FILES_CMAKE localmcp_shared_files.cmake
    BUILD_DEPENDENCIES PRIVATE Gem::${gem_name}.Static
)

ly_create_alias(NAME ${gem_name}.Clients NAMESPACE Gem TARGETS Gem::${gem_name})
ly_create_alias(NAME ${gem_name}.Servers NAMESPACE Gem TARGETS Gem::${gem_name})
ly_create_alias(NAME ${gem_name}.Unified NAMESPACE Gem TARGETS Gem::${gem_name})

# The editor endpoint: binding bridge and editor tools, served by its own LocalMcp server inside the Editor.
if(PAL_TRAIT_BUILD_HOST_TOOLS)
    ly_add_target(
        NAME ${gem_name}.Editor.Static STATIC
        NAMESPACE Gem
        FILES_CMAKE localmcp_editor_files.cmake
        INCLUDE_DIRECTORIES PUBLIC Source
        BUILD_DEPENDENCIES
            PUBLIC
                Gem::${gem_name}.Static
                AZ::AzToolsFramework
                Gem::EditorPythonBindings.Static
                # Viewport screenshots use the Atom frame capture system and the default viewport context.
                Gem::Atom_Feature_Common.Public
        AUTOGEN_RULES
            *.McpTools.xml,McpTools_Header.jinja,$path/$fileprefix.McpTools.h
            *.McpTools.xml,McpTools_Source.jinja,$path/$fileprefix.McpTools.cpp
    )

    ly_add_target(
        NAME ${gem_name}.Editor GEM_MODULE
        NAMESPACE Gem
        FILES_CMAKE localmcp_editor_shared_files.cmake
        BUILD_DEPENDENCIES PRIVATE Gem::${gem_name}.Editor.Static
        RUNTIME_DEPENDENCIES Gem::EditorPythonBindings.Editor
    )

    # pybind11 reports failures by exception.
    target_compile_options(${gem_name}.Editor.Static ${O3DE_COMPILE_OPTION_ENABLE_EXCEPTIONS})

    ly_create_alias(NAME ${gem_name}.Tools NAMESPACE Gem TARGETS Gem::${gem_name}.Editor)

    if(PAL_TRAIT_BUILD_TESTS_SUPPORTED)
        ly_add_target(
            NAME ${gem_name}.Editor.Tests ${PAL_TRAIT_TEST_TARGET_TYPE}
            NAMESPACE Gem
            FILES_CMAKE localmcp_editor_tests_files.cmake
            INCLUDE_DIRECTORIES PRIVATE ${LY_ROOT_FOLDER}/Gems/EditorPythonBindings/Code
            COMPILE_DEFINITIONS PRIVATE EDITOR_PYTHON_BINDINGS_TEST_ENGINE_ROOT="${LY_ROOT_FOLDER}"
            BUILD_DEPENDENCIES PRIVATE Gem::${gem_name}.Editor.Static AZ::AzTest AZ::AzCoreTestCommon
        )
        ly_add_googletest(NAME Gem::${gem_name}.Editor.Tests)
    endif()
endif()

if(PAL_TRAIT_BUILD_TESTS_SUPPORTED)
    ly_add_target(
        NAME ${gem_name}.Tests ${PAL_TRAIT_TEST_TARGET_TYPE}
        NAMESPACE Gem
        FILES_CMAKE localmcp_tests_files.cmake
        BUILD_DEPENDENCIES PRIVATE Gem::${gem_name}.Static AZ::AzTest AZ::AzCoreTestCommon
    )
    ly_add_googletest(NAME Gem::${gem_name}.Tests)
endif()
