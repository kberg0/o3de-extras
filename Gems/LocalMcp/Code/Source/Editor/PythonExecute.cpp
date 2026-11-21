/*
 * Copyright (c) Contributors to the Open 3D Engine Project.
 * For complete copyright and license terms please see the LICENSE at the root of this distribution.
 *
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 *
 */

#include "PythonExecute.h"

#include <AzCore/Interface/Interface.h>
#include <AzToolsFramework/API/EditorPythonConsoleBus.h>

#include <pybind11/embed.h>

#include <string>

namespace LocalMcp::PythonExecute
{
    namespace
    {
        constexpr double MillisecondsPerSecond = 1000.0;

        //! Defines run(code, timeout_seconds, limit). It swaps sys.stdout and sys.stderr for bounded buffers, stops the code
        //! with a BaseException from a trace function once the deadline passes, and restores both streams and the previous
        //! trace function whatever the code does.
        constexpr const char* RunnerSource = R"(
import sys
import time
import traceback

class PythonExecuteTimeout(BaseException):
    pass

class BoundedOutput:
    def __init__(self, limit):
        self.parts = []
        self.size = 0
        self.limit = limit
        self.truncated = False

    def write(self, text):
        text = str(text)
        room = max(self.limit - self.size, 0)
        if len(text) > room:
            self.truncated = True
        kept = text[:room]
        if kept:
            self.parts.append(kept)
            self.size += len(kept)
        return len(text)

    def flush(self):
        pass

    def value(self):
        return ''.join(self.parts)

def run(code, timeout_seconds, limit):
    out = BoundedOutput(limit)
    err = BoundedOutput(limit)
    state = {'timedOut': False}
    deadline = time.monotonic() + timeout_seconds

    def trace(frame, event, arg):
        if time.monotonic() > deadline:
            state['timedOut'] = True
            raise PythonExecuteTimeout('python_execute timed out after %g s' % timeout_seconds)
        return trace

    error = ''
    saved_out, saved_err, saved_trace = sys.stdout, sys.stderr, sys.gettrace()
    sys.stdout, sys.stderr = out, err
    sys.settrace(trace)
    try:
        exec(compile(code, '<python_execute>', 'exec'), {'__name__': '__main__'})
    except BaseException:
        error = traceback.format_exc()
    finally:
        sys.settrace(saved_trace)
        sys.stdout, sys.stderr = saved_out, saved_err

    truncated = out.truncated or err.truncated or len(error) > limit
    return {
        'stdout': out.value(),
        'stderr': err.value(),
        'traceback': error[-limit:],
        'timedOut': state['timedOut'],
        'truncated': truncated,
    }
)";
    } // namespace

    BridgeResult<void> Run(AZStd::string_view code, AZ::s64 timeoutMs, Json& data, Allocator& allocator)
    {
        auto* python = AZ::Interface<AzToolsFramework::EditorPythonEventsInterface>::Get();
        if (!python || !python->IsPythonActive())
        {
            return AZ::Failure(AZStd::string("EditorPythonBindings interpreter is not active"));
        }

        BridgeResult<void> result = AZ::Success();
        python->ExecuteWithLock(
            [&]()
            {
                result = TryPython(
                    [&]()
                    {
                        pybind11::dict scope;
                        scope["__builtins__"] = pybind11::module::import("builtins");
                        pybind11::exec(RunnerSource, scope);

                        const double timeoutSeconds = static_cast<double>(timeoutMs) / MillisecondsPerSecond;
                        const auto reply = scope["run"](pybind11::str(code.data(), code.size()), timeoutSeconds, MaxOutputCharacters)
                                               .cast<pybind11::dict>();

                        auto text = [&](const char* member)
                        {
                            const std::string value = reply[member].cast<std::string>();
                            return String({ value.data(), value.size() }, allocator);
                        };
                        data.AddMember("stdout", text("stdout"), allocator);
                        data.AddMember("stderr", text("stderr"), allocator);
                        Json traceback = text("traceback");
                        if (traceback.GetStringLength() > 0)
                        {
                            data.AddMember("traceback", traceback, allocator);
                        }
                        data.AddMember("timedOut", reply["timedOut"].cast<bool>(), allocator);
                        data.AddMember("truncated", reply["truncated"].cast<bool>(), allocator);
                    });
            });
        return result;
    }
} // namespace LocalMcp::PythonExecute
