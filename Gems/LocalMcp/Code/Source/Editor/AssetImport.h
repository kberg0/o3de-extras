/*
 * Copyright (c) Contributors to the Open 3D Engine Project.
 * For complete copyright and license terms please see the LICENSE at the root of this distribution.
 *
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 *
 */

#pragma once

#include "LocalMcpJson.h"
#include "LocalMcpToolProvider.h"

#include <AzCore/std/chrono/chrono.h>
#include <AzCore/std/parallel/mutex.h>
#include <AzToolsFramework/API/EditorAssetSystemAPI.h>
#include <Source/Editor/AutoGen/LocalMcpEditorTools.McpTools.h>

namespace LocalMcp
{
    //! Time kept back from the connection deadline, so that a held request is answered before the client gives up.
    inline constexpr AZ::s64 ReplyMarginMs = 250;

    //! Copies one source into the project and watches the Asset Processor process it, until its products
    //! are in the Asset Catalog. At most one observation is active; the held editor request owns it.
    //! Every function runs on the editor main thread.
    class AssetImport final : private AzToolsFramework::AssetSystemBus::Handler
    {
    public:
        void Activate();
        void Deactivate();

        //! Starts asset_import: copies args.m_sourcePath to args.m_destPath inside an asset-safe project folder and asks
        //! the Asset Processor to process it. Returns Pending when the copy started, InvalidArguments with detail set
        //! for a bad path or timeout, and Complete with data.error for any other failure.
        ToolProvider::Result Begin(
            const AssetImportArgs& args, Json& data, Allocator& allocator, AZStd::string& detail, AZ::s64 connectionTimeoutMs);

        //! Reports Pending until the jobs finish and every product is in the catalog, then Complete with the products,
        //! or with data.error when a job failed or the observation timed out.
        ToolProvider::Result Poll(Json& data, Allocator& allocator);

        //! Stops observing. A copied source stays in the project.
        void Cancel();

        //! True while an observation is active.
        bool IsActive() const;

    private:
        //! Records an Asset Processor notice for the observed source.
        void SourceNotice(const AZStd::string& relativePath, const AZStd::string& scanFolder, const AZ::Uuid& id, bool failed);

        //! Reports message as data.error with the source path, ends the observation and returns Complete.
        ToolProvider::Result FinishError(AZStd::string_view message, Json& data, Allocator& allocator);

        //! Starts observing destination until timeoutMs from now.
        void BeginObservation(const AZStd::string& destination, AZ::s64 timeoutMs);

        // AssetSystemBus overrides; these can arrive on other threads.
        void SourceFileChanged(AZStd::string relativePath, AZStd::string scanFolder, AZ::Uuid sourceUUID) override;
        void SourceFileFailed(AZStd::string relativePath, AZStd::string scanFolder, AZ::Uuid sourceUUID) override;

        bool m_active = false;
        bool m_copied = false;
        AZStd::string m_destination;
        AZStd::string m_relativeSource;
        AZStd::chrono::steady_clock::time_point m_until;
        AZStd::chrono::steady_clock::time_point m_nextQuery;

        //! Guards the notice state below. Editor and Asset Processor APIs are never called while it is held.
        AZStd::mutex m_noticeMutex;
        AZStd::string m_noticePath;
        AZ::Uuid m_source;
        bool m_failed = false;
    };
} // namespace LocalMcp
