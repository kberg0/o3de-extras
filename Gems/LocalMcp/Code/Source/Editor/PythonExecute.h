/*
 * Copyright (c) Contributors to the Open 3D Engine Project.
 * For complete copyright and license terms please see the LICENSE at the root of this distribution.
 *
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 *
 */

#pragma once

#include "BridgeValues.h"

#include <AzCore/std/string/string_view.h>

//! Runs agent-supplied Python in the editor's EditorPythonBindings interpreter for the python_execute tool.
namespace LocalMcp::PythonExecute
{
    //! Most characters of stdout, of stderr and of the traceback each returned; the rest is dropped and flagged.
    inline constexpr size_t MaxOutputCharacters = 65536;

    //! Settings key that enables python_execute. It is read once when the editor endpoint starts, so a client cannot turn
    //! it on through the endpoint.
    inline constexpr const char* EnabledSetting = "/O3DE/Gems/LocalMcp/Editor/PythonExecute";

    //! Runs code as a script in a fresh namespace, with the Python lock held, on the calling (main) thread.
    //! Writes data.stdout and data.stderr, data.traceback when the code raised, data.timedOut when it ran past timeoutMs,
    //! and data.truncated when output was cut at MaxOutputCharacters. The timeout is checked between Python lines, so a
    //! long native call is not interrupted. Fails only when the interpreter is not running.
    BridgeResult<void> Run(AZStd::string_view code, AZ::s64 timeoutMs, Json& data, Allocator& allocator);
} // namespace LocalMcp::PythonExecute
