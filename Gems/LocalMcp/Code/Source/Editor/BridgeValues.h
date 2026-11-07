/* Copyright (c) Contributors to the Open 3D Engine Project.
 * SPDX-License-Identifier: Apache-2.0 OR MIT */
#pragma once
#include "LocalMcpJson.h"
#include <EditorPythonBindings/PythonUtility.h>
#include <AzCore/std/containers/unordered_map.h>
#include <stdexcept>
namespace LocalMcp
{
    // O3DE's _HAS_EXCEPTIONS=0 STL exception retains only a borrowed message pointer.
    class BridgeError final : public std::runtime_error
    {
    public:
        explicit BridgeError(AZStd::string message)
            : std::runtime_error("LocalMcp bridge error"), m_message(AZStd::move(message)) {}
        const char* what() const noexcept override { return m_message.c_str(); }
    private:
        AZStd::string m_message;
    };

    //! Owns process-local Python values. Every method runs on the editor main thread with the interpreter lock held.
    class BridgeValues
    {
    public:
        static constexpr size_t MaxHandles = 256;
        BridgeValues() = default;
        BridgeValues(const BridgeValues&) = delete;
        BridgeValues& operator=(const BridgeValues&) = delete;
        void Activate();
        bool Empty() const { return m_handles.empty(); }
        bool CanStore() const { return m_handles.size() < MaxHandles; }
        void Clear() { m_handles.clear(); }
        pybind11::object Construct(const AZ::BehaviorClass& type, const Json& arguments, unsigned depth = 0);
        Json Store(pybind11::object value, Allocator& allocator);
        size_t Release(const Json& handles);
        static AZStd::string Unsupported(const AZ::BehaviorParameter& parameter);
        pybind11::object Decode(const Json& value, const AZ::BehaviorParameter* expected, unsigned depth = 0);
        Json Encode(pybind11::object value, const AZ::BehaviorParameter* expected, Allocator& allocator, unsigned depth = 0);
    private:
        static bool IsOutcome(const AZ::TypeId& type);
        using ObjectHandles = AZStd::unordered_map<AZStd::string, pybind11::object>;
        ObjectHandles m_handles;
        AZStd::string m_scope;
        AZ::u64 m_nextHandle = 0;
    };
}
