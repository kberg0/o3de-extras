/*
 * Copyright (c) Contributors to the Open 3D Engine Project.
 * For complete copyright and license terms please see the LICENSE at the root of this distribution.
 *
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 *
 */

#pragma once

#include "BehaviorBindings.h"
#include "BridgeValues.h"

#include <AzCore/std/containers/vector.h>
#include <AzCore/std/string/string.h>
#include <AzCore/std/string/string_view.h>

//! Groups BehaviorContext bindings by their Module attribute, the way azlmbr exposes them to editor Python.
namespace LocalMcp::Bindings
{
    //! Module of a binding: its owner's Module attribute, or the module azlmbr uses for an owner without one.
    AZStd::string GetModule(const BindingView& binding);

    //! True when the binding's owner is scoped for editor automation, which azlmbr requires.
    bool IsAutomation(const BindingView& binding);

    //! True when module is filter or one of its dotted sub-modules.
    bool InModule(AZStd::string_view module, AZStd::string_view filter);

    //! Lists the children of path among the automation bindings: modules first, then the classes, buses and global members
    //! of a module, then the members of a class or bus. Writes the page of children after the first after ones, sorted by
    //! name, as data.children with data.next and data.total. Fails when no automation binding is at or below path.
    BridgeResult<void> Browse(
        AZ::BehaviorContext& context, AZStd::string_view path, size_t after, size_t limit, Json& data, Allocator& allocator);
} // namespace LocalMcp::Bindings

namespace LocalMcp
{
    //! A typed tool generated from one callable binding: what it passes to binding_call, and its tools/list entry.
    struct ModuleTool
    {
        //! MCP tool name, made from the binding's module path by ModuleTools::MakeToolName().
        AZStd::string m_name;
        AZStd::string m_binding;
        AZStd::string m_description;
        //! The tool's inputSchema, serialized.
        AZStd::string m_inputSchema;
        //! Names of the native arguments, in order.
        AZStd::vector<AZStd::string> m_arguments;
        //! How many leading arguments have no default value.
        size_t m_requiredArguments = 0;
        //! An instance member: the call takes a target object handle.
        bool m_takesTarget = false;
        //! A bus event that can be sent to an address.
        bool m_takesAddress = false;
        //! The bus has no broadcast sender, so the address is required.
        bool m_requiresAddress = false;
        //! The binding has further overloads; the tool always calls the first.
        bool m_overloaded = false;
    };

    //! The typed tools of the modules an agent opened: one tool per callable automation binding, named by module path,
    //! whose calls run through binding_call. The open modules are global to the endpoint, so every client sees the same
    //! tools. Every function runs on the editor main thread.
    class ModuleTools
    {
    public:
        //! Most generated tools open at once, over all open modules.
        static constexpr size_t MaxOpenTools = 128;

        //! Longest tool name MCP allows.
        static constexpr size_t MaxToolNameLength = 128;

        //! Opens module and its sub-modules, and returns the names of the tools it added.
        //! Fails when the module has no callable automation binding, overlaps an open module, or would take the open
        //! tools past MaxOpenTools.
        BridgeResult<Json> Open(AZ::BehaviorContext& context, AZStd::string_view module, Allocator& allocator);

        //! Closes an open module and removes its tools.
        BridgeResult<Json> Close(AZStd::string_view module, Allocator& allocator);

        //! Appends the entries of the open tools to the tools/list array.
        void AddTools(Json& tools, Allocator& allocator) const;

        //! The open tool called name, or nullptr.
        const ModuleTool* Find(AZStd::string_view name) const;

        //! Changes whenever a module is opened or closed.
        AZ::u64 GetRevision() const;

        //! Writes the binding_call arguments for a call of tool with args into request.
        //! Fails, naming the argument, when args do not match the tool's schema.
        static BridgeResult<void> BuildCall(const ModuleTool& tool, const Json& args, Json& request, Allocator& allocator);

        //! The MCP tool name of the binding at a module path: the path with every character outside [A-Za-z0-9_.-] replaced
        //! by '_'. A name that had to change or is longer than MaxToolNameLength is cut and ends in '-' and a hash of
        //! bindingId instead, so it stays unique and stable.
        static AZStd::string MakeToolName(AZStd::string_view path, AZStd::string_view bindingId);

    private:
        //! One open module and the tools it added.
        struct OpenModule
        {
            AZStd::string m_name;
            AZStd::vector<ModuleTool> m_tools;
        };

        //! The tool name for path that ends in a hash of bindingId; used when the plain name is lossy, too long or taken.
        static AZStd::string HashedToolName(AZStd::string_view path, AZStd::string_view bindingId);

        //! Number of tools the open modules provide.
        size_t CountOpenTools() const;

        AZStd::vector<OpenModule> m_modules;
        AZ::u64 m_revision = 0;
    };
} // namespace LocalMcp
