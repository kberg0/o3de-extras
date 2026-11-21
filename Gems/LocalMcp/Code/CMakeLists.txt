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

if(PAL_TRAIT_BUILD_TESTS_SUPPORTED)
    ly_add_target(
        NAME ${gem_name}.Tests ${PAL_TRAIT_TEST_TARGET_TYPE}
        NAMESPACE Gem
        FILES_CMAKE localmcp_tests_files.cmake
        BUILD_DEPENDENCIES PRIVATE Gem::${gem_name}.Static AZ::AzTest AZ::AzCoreTestCommon
    )
    ly_add_googletest(NAME Gem::${gem_name}.Tests)
endif()
