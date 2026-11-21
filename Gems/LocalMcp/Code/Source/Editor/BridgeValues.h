/*
 * Copyright (c) Contributors to the Open 3D Engine Project.
 * For complete copyright and license terms please see the LICENSE at the root of this distribution.
 *
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 *
 */

#pragma once

#include "LocalMcpJson.h"

#include <AzCore/Outcome/Outcome.h>
#include <AzCore/std/containers/unordered_map.h>
#include <AzCore/std/containers/vector.h>
#include <AzCore/std/optional.h>
#include <AzCore/std/typetraits/is_void.h>
#include <EditorPythonBindings/PythonUtility.h>

namespace LocalMcp
{
    //! The value of a bridge or recipe step, or why it failed. The tool that runs the step reports the error as data.error.
    template<class T>
    using BridgeResult = AZ::Outcome<T, AZStd::string>;

    //! Runs operation, a call into pybind11, and returns its result. pybind11 reports a Python error or a failed cast by
    //! exception; the exception is caught here and its message returned as the error, so it never leaves the bridge.
    template<class Operation>
    auto TryPython(Operation&& operation) -> BridgeResult<decltype(operation())>
    {
        try
        {
            if constexpr (AZStd::is_void_v<decltype(operation())>)
            {
                operation();
                return AZ::Success();
            }
            else
            {
                return AZ::Success(operation());
            }
        }
        catch (const pybind11::error_already_set& error)
        {
            return AZ::Failure(AZStd::string(error.what()));
        }
        catch (const std::exception& error)
        {
            return AZ::Failure(AZStd::string(error.what()));
        }
    }

    //! Converts between tagged JSON values and Python values through the EditorPythonBindings marshaller, and owns the
    //! Python objects that replies refer to by handle.
    //! Every function runs on the editor main thread with the Python interpreter lock held, and returns an error when a
    //! value is malformed, out of range or exceeds a conversion budget.
    class BridgeValues
    {
    public:
        //! Most object handles alive at once.
        static constexpr size_t MaxHandles = 256;

        BridgeValues() = default;
        BridgeValues(const BridgeValues&) = delete;
        BridgeValues& operator=(const BridgeValues&) = delete;

        //! Starts a new handle scope, so handles from an earlier activation never resolve.
        void Activate();

        //! True when no object handle is alive.
        bool Empty() const;

        //! True when another object handle can be stored.
        bool CanStore() const;

        //! Releases every object handle.
        void Clear();

        //! Constructs type from the JSON args array, with the constructor overload when one is given.
        //! Without an overload, the one constructor that accepts the argument count is used.
        BridgeResult<pybind11::object> Construct(
            const AZ::BehaviorClass& type, const Json& args, AZStd::optional<size_t> overload, unsigned depth = 0);

        //! Keeps value alive and returns its handle object: {"$handle", "typename", "type"}.
        BridgeResult<Json> Store(pybind11::object value, Allocator& allocator);

        //! Releases handles and returns how many were alive. Refuses the whole list when one handle is unknown.
        BridgeResult<size_t> Release(const AZStd::vector<AZStd::string>& handles);

        //! Why parameter cannot be converted, or an empty string when it can.
        static AZStd::string Unsupported(const AZ::BehaviorParameter& parameter);

        //! Converts a tagged JSON value to the Python value expected; with no expected parameter the value is untyped.
        //! Entity, component and asset references are checked to exist in the editor.
        BridgeResult<pybind11::object> Decode(const Json& value, const AZ::BehaviorParameter* expected, unsigned depth = 0);

        //! Converts a Python value to tagged JSON. Values without a JSON form are stored and returned as a handle.
        BridgeResult<Json> Encode(pybind11::object value, const AZ::BehaviorParameter* expected, Allocator& allocator, unsigned depth = 0);

    private:
        //! True when type is an AZ::Outcome specialization.
        static bool IsOutcome(const AZ::TypeId& type);

        //! Decodes an object with a "$type" tag: a 64-bit integer, an editor reference, a math value, an Outcome, or a
        //! reflected value class built from constructor args or a container value.
        BridgeResult<pybind11::object> DecodeTagged(const Json& value, const AZ::TypeId& type, unsigned depth);

        //! Builds the Outcome a tagged value describes through its reflected Success or Failure factory.
        BridgeResult<pybind11::object> DecodeOutcome(const Json& value, const AZ::TypeId& type, unsigned depth);

        //! Encodes a reflected value object: an editor reference or math value as a tagged value, anything else as a handle.
        BridgeResult<Json> EncodeObject(pybind11::object value, Allocator& allocator, unsigned depth);

        //! Encodes an Outcome as {"$type": "Outcome", "type", "success", "value" or "error"}.
        BridgeResult<Json> EncodeOutcome(pybind11::object value, const AZ::BehaviorClass& owner, Allocator& allocator, unsigned depth);

        //! Live Python objects by handle ID.
        AZStd::unordered_map<AZStd::string, pybind11::object> m_handles;
        //! Random prefix of this activation's handle IDs.
        AZStd::string m_scope;
        AZ::u64 m_nextHandle = 0;
    };
} // namespace LocalMcp
