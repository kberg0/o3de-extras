/* Copyright (c) Contributors to the Open 3D Engine Project.
 * SPDX-License-Identifier: Apache-2.0 OR MIT */
#pragma once
#include "LocalMcpJson.h"
#include "LocalMcpToolProvider.h"
#include <AzToolsFramework/API/EditorAssetSystemAPI.h>
#include <AzCore/std/chrono/chrono.h>
#include <AzCore/std/parallel/mutex.h>

namespace LocalMcp
{
    //! One active AP observation, with optional source copy, owned by the held editor request.
    class AssetImport final : private AzToolsFramework::AssetSystemBus::Handler
    {
    public:
        void Activate();
        void Deactivate();
        ToolProvider::Result Begin(const Json& args, Json& data, Allocator& allocator, AZStd::string& detail,
            AZ::s64 connectionTimeoutMs);
        ToolProvider::Result Poll(Json& data, Allocator& allocator);
        //! Observe an already saved source with the same AP job/catalog checks, without copying it.
        ToolProvider::Result Observe(const AZStd::string& path, AZ::s64 timeoutMs, Json& data, Allocator& allocator);
        void Cancel();
        bool IsActive() const { return m_active; } // Editor main thread only.
    private:
        void SourceFileChanged(AZStd::string relativePath, AZStd::string scanFolder, AZ::Uuid sourceUUID) override;
        void SourceFileFailed(AZStd::string relativePath, AZStd::string scanFolder, AZ::Uuid sourceUUID) override;
        void SourceNotice(const AZStd::string& relativePath, const AZStd::string& scanFolder, const AZ::Uuid& id, bool failed);
        ToolProvider::Result FinishError(AZStd::string_view message, Json& data, Allocator& allocator);
        void BeginObservation(const AZStd::string& destination, AZ::s64 timeoutMs);
        bool m_active = false;
        bool m_copied = false;
        AZStd::string m_destination;
        AZStd::string m_relativeSource;
        AZStd::chrono::steady_clock::time_point m_until;
        AZStd::chrono::steady_clock::time_point m_nextQuery;
        // AssetSystem notifications can be emitted by other threads; never call editor/AP APIs under this lock.
        AZStd::mutex m_noticeMutex;
        AZStd::string m_noticePath;
        AZ::Uuid m_source;
        bool m_failed = false;
    };
}
