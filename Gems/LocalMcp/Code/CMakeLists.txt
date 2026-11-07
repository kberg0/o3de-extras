# Copyright (c) Contributors to the Open 3D Engine Project.
# SPDX-License-Identifier: Apache-2.0 OR MIT

if(NOT PAL_PLATFORM_NAME STREQUAL "Windows")
    return()
endif()

ly_add_target(
    NAME ${gem_name}.Static STATIC
    NAMESPACE Gem
    FILES_CMAKE localmcp_files.cmake
    INCLUDE_DIRECTORIES PUBLIC Source
    BUILD_DEPENDENCIES PUBLIC AZ::AzCore AZ::AzNetworking 3rdParty::OpenSSL
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
if(PAL_TRAIT_BUILD_HOST_TOOLS)
    ly_add_target(
        NAME ${gem_name}.Editor.Static STATIC
        NAMESPACE Gem
        FILES_CMAKE localmcp_editor_files.cmake
        INCLUDE_DIRECTORIES PUBLIC Source
        BUILD_DEPENDENCIES PUBLIC Gem::${gem_name}.Static AZ::AzToolsFramework Gem::EditorPythonBindings.Static
    )
    ly_add_target(
        NAME ${gem_name}.Editor GEM_MODULE
        NAMESPACE Gem
        FILES_CMAKE localmcp_editor_shared_files.cmake
        BUILD_DEPENDENCIES PRIVATE Gem::${gem_name}.Editor.Static
        RUNTIME_DEPENDENCIES Gem::EditorPythonBindings.Editor
    )
    if(MSVC)
        target_compile_options(${gem_name}.Editor.Static PRIVATE /EHsc)
        target_compile_options(${gem_name}.Editor PRIVATE /EHsc)
    endif()
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
        if(MSVC)
            target_compile_options(${gem_name}.Editor.Tests PRIVATE /EHsc)
        endif()
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
