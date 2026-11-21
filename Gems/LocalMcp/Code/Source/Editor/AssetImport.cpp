/*
 * Copyright (c) Contributors to the Open 3D Engine Project.
 * For complete copyright and license terms please see the LICENSE at the root of this distribution.
 *
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 *
 */

#include "AssetImport.h"

#include <AzCore/Asset/AssetManagerBus.h>
#include <AzCore/IO/Path/Path.h>
#include <AzCore/Utils/Utils.h>
#include <AzCore/std/algorithm.h>
#include <AzFramework/Asset/AssetSystemBus.h>

#include <QDir>
#include <QFile>
#include <QFileInfo>

namespace LocalMcp
{
    namespace
    {
        //! Import time limit when the request gives none, further capped by the connection deadline.
        constexpr AZ::s64 DefaultImportTimeoutMs = 60000;

        //! Interval between Asset Processor job queries.
        constexpr AZStd::chrono::milliseconds JobQueryInterval(250);

        //! Most bytes of one failed job's log, and most failed jobs, in a reply.
        constexpr size_t MaxJobLogBytes = 4096;
        constexpr rapidjson::SizeType MaxJobDiagnostics = 32;

        //! Most products one imported source may report.
        constexpr size_t MaxProducts = 4096;

        //! True when path is root or inside it.
        bool Within(const QDir& root, const QString& path)
        {
            const QByteArray nativeRoot = root.absolutePath().toUtf8();
            const QByteArray nativePath = path.toUtf8();
            return AZ::IO::PathView(nativePath.constData()).IsRelativeTo(AZ::IO::PathView(nativeRoot.constData()));
        }

        //! The project root with symbolic links resolved, or an empty string when it is unavailable.
        QString CanonicalProjectPath()
        {
            return QFileInfo(QString::fromUtf8(AZ::Utils::GetProjectPath().c_str())).canonicalFilePath();
        }
    } // namespace

    void AssetImport::Activate()
    {
        AzToolsFramework::AssetSystemBus::Handler::BusConnect();
    }

    void AssetImport::Deactivate()
    {
        AzToolsFramework::AssetSystemBus::Handler::BusDisconnect();
        Cancel();
    }

    ToolProvider::Result AssetImport::Begin(
        const AssetImportArgs& args, Json& data, Allocator& allocator, AZStd::string& detail, AZ::s64 connectionTimeoutMs)
    {
        const AZ::s64 maximumTimeoutMs = connectionTimeoutMs - ReplyMarginMs;
        const bool timeoutFits =
            !args.m_timeoutMs || (maximumTimeoutMs >= 1 && *args.m_timeoutMs <= aznumeric_cast<uint64_t>(maximumTimeoutMs));
        if (!timeoutFits)
        {
            detail = "timeout_ms: expected a timeout that ends before the connection deadline";
            return ToolProvider::Result::InvalidArguments;
        }

        if (m_active)
        {
            data.AddMember("error", "An asset import is already active", allocator);
            return ToolProvider::Result::Complete;
        }

        const QFileInfo source(QString::fromUtf8(args.m_sourcePath.c_str()));
        if (!source.isAbsolute() || !source.isFile() || !source.isReadable())
        {
            detail = "Source must be an absolute readable file";
            return ToolProvider::Result::InvalidArguments;
        }

        const QString projectPath = CanonicalProjectPath();
        if (projectPath.isEmpty())
        {
            data.AddMember("error", "The project root is unavailable", allocator);
            return ToolProvider::Result::Complete;
        }

        const QDir project(projectPath);
        const QString destination = QDir::cleanPath(project.absoluteFilePath(QString::fromUtf8(args.m_destPath.c_str())));
        if (!Within(project, destination))
        {
            detail = "Source must be an absolute readable file; destination must be inside the project";
            return ToolProvider::Result::InvalidArguments;
        }

        // Resolve existing ancestors too, so a project symlink cannot direct the copy outside the project.
        QFileInfo ancestor(destination);
        while (!ancestor.exists() && ancestor.absoluteFilePath() != ancestor.absolutePath())
        {
            ancestor.setFile(ancestor.absolutePath());
        }
        if (!Within(project, ancestor.canonicalFilePath()) || QFileInfo::exists(destination))
        {
            detail = "Destination escapes the project or already exists; import never overwrites a source";
            return ToolProvider::Result::InvalidArguments;
        }

        AZStd::vector<AZStd::string> folders;
        bool available = false;
        AzToolsFramework::AssetSystemRequestBus::BroadcastResult(
            available, &AzToolsFramework::AssetSystem::AssetSystemRequest::GetAssetSafeFolders, folders);
        const bool safe = AZStd::any_of(
            folders.begin(),
            folders.end(),
            [&destination](const AZStd::string& folder)
            {
                const QString absoluteFolder = QString::fromUtf8(folder.c_str());
                return QDir::isAbsolutePath(absoluteFolder) && Within(QDir(absoluteFolder), destination);
            });
        if (!available || !safe)
        {
            data.AddMember("error", "Asset Processor did not identify the destination as an asset-safe folder", allocator);
            return ToolProvider::Result::Complete;
        }

        const AZ::s64 timeoutMs =
            args.m_timeoutMs ? aznumeric_cast<AZ::s64>(*args.m_timeoutMs) : AZStd::min(DefaultImportTimeoutMs, maximumTimeoutMs);
        BeginObservation(destination.toUtf8().constData(), timeoutMs);

        const bool copied = QDir().mkpath(QFileInfo(destination).absolutePath()) && QFile::copy(source.absoluteFilePath(), destination);
        if (!copied)
        {
            return FinishError("Could not copy source into the project; inspect source/destination permissions", data, allocator);
        }
        m_copied = true;

        bool sent = false;
        AzFramework::AssetSystemRequestBus::BroadcastResult(
            sent, &AzFramework::AssetSystem::AssetSystemRequests::EscalateAssetBySearchTerm, AZStd::string_view(m_destination));
        if (!sent)
        {
            return FinishError("Source copied, but Asset Processor escalation could not be sent", data, allocator);
        }
        return ToolProvider::Result::Pending;
    }

    ToolProvider::Result AssetImport::Poll(Json& data, Allocator& allocator)
    {
        using namespace AzToolsFramework;

        if (!m_active)
        {
            return FinishError("No active asset import", data, allocator);
        }

        const auto now = AZStd::chrono::steady_clock::now();
        if (now >= m_until)
        {
            return FinishError("Asset Processor import timed out; copied source remains in the project", data, allocator);
        }
        if (now < m_nextQuery)
        {
            return ToolProvider::Result::Pending;
        }
        m_nextQuery = now + JobQueryInterval;

        AZ::Uuid source;
        bool failed = false;
        {
            AZStd::scoped_lock lock(m_noticeMutex);
            source = m_source;
            failed = m_failed;
        }

        // Still processing while the jobs are unknown or unfinished.
        AZ::Outcome<AssetSystem::JobInfoContainer> jobs;
        AssetSystemJobRequestBus::BroadcastResult(jobs, &AssetSystem::AssetSystemJobRequest::GetAssetJobsInfo, m_destination, false);
        bool processing = !jobs || jobs.GetValue().empty();

        Json diagnostics(rapidjson::kArrayType);
        if (jobs)
        {
            for (const auto& job : jobs.GetValue())
            {
                const bool running = job.m_status == AssetSystem::JobStatus::Queued || job.m_status == AssetSystem::JobStatus::InProgress ||
                    job.m_status == AssetSystem::JobStatus::Missing;
                if (running)
                {
                    processing = true;
                    continue;
                }
                if (job.m_status == AssetSystem::JobStatus::Completed)
                {
                    continue;
                }

                failed = true;
                Json diagnostic(rapidjson::kObjectType);
                diagnostic.AddMember("jobKey", String(job.m_jobKey, allocator), allocator);
                diagnostic.AddMember("platform", String(job.m_platform, allocator), allocator);
                diagnostic.AddMember("status", String(AssetSystem::JobStatusString(job.m_status), allocator), allocator);

                AZ::Outcome<AZStd::string> log;
                AssetSystemJobRequestBus::BroadcastResult(log, &AssetSystem::AssetSystemJobRequest::GetJobLog, job.m_jobRunKey);
                if (log)
                {
                    const AZStd::string_view text(log.GetValue());
                    diagnostic.AddMember("log", String(text.substr(0, MaxJobLogBytes), allocator), allocator);
                    diagnostic.AddMember("logTruncated", text.size() > MaxJobLogBytes, allocator);
                }

                diagnostics.PushBack(diagnostic, allocator);
                if (diagnostics.Size() == MaxJobDiagnostics)
                {
                    data.AddMember("jobDiagnosticsTruncated", true, allocator);
                    break;
                }
            }
        }

        if (failed)
        {
            data.AddMember("jobs", diagnostics, allocator);
            return FinishError("Asset Processor failed to process the imported source", data, allocator);
        }
        if (processing)
        {
            return ToolProvider::Result::Pending;
        }

        // Without a notice yet, the source UUID comes from the Asset Processor's source info.
        if (source.IsNull())
        {
            bool found = false;
            AZ::Data::AssetInfo info;
            AZStd::string folder;
            AssetSystemRequestBus::BroadcastResult(
                found, &AssetSystem::AssetSystemRequest::GetSourceInfoBySourcePath, m_destination.c_str(), info, folder);
            if (!found)
            {
                return ToolProvider::Result::Pending;
            }
            source = info.m_assetId.m_guid;
        }

        AZStd::vector<AZ::Data::AssetInfo> products;
        bool found = false;
        AssetSystemRequestBus::BroadcastResult(found, &AssetSystem::AssetSystemRequest::GetAssetsProducedBySourceUUID, source, products);
        if (!found)
        {
            return ToolProvider::Result::Pending;
        }
        if (products.size() > MaxProducts)
        {
            data.AddMember("productCount", static_cast<uint64_t>(products.size()), allocator);
            return FinishError("Source produced more products than the response budget", data, allocator);
        }

        // Every product must also be in the Asset Catalog before the import completes.
        Json output(rapidjson::kArrayType);
        for (const auto& product : products)
        {
            AZ::Data::AssetId catalog;
            AZ::Data::AssetCatalogRequestBus::BroadcastResult(
                catalog, &AZ::Data::AssetCatalogRequests::GetAssetIdByPath, product.m_relativePath.c_str(), product.m_assetType, false);
            if (catalog != product.m_assetId)
            {
                return ToolProvider::Result::Pending;
            }

            Json id(rapidjson::kObjectType);
            id.AddMember("$type", "AssetId", allocator);
            id.AddMember("guid", String(product.m_assetId.m_guid.ToString<AZStd::string>(), allocator), allocator);
            id.AddMember("subId", product.m_assetId.m_subId, allocator);

            Json item(rapidjson::kObjectType);
            item.AddMember("id", id, allocator);
            item.AddMember("assetType", String(product.m_assetType.ToString<AZStd::string>(), allocator), allocator);
            item.AddMember("path", String(product.m_relativePath, allocator), allocator);
            output.PushBack(item, allocator);
        }

        data.AddMember("products", output, allocator);
        data.AddMember("sourceUuid", String(source.ToString<AZStd::string>(), allocator), allocator);
        data.AddMember("sourcePath", String(m_destination, allocator), allocator);
        data.AddMember("sourceCopied", m_copied, allocator);
        Cancel();
        return ToolProvider::Result::Complete;
    }

    void AssetImport::Cancel()
    {
        m_active = false;
        m_copied = false;
        m_destination.clear();
        m_relativeSource.clear();

        AZStd::scoped_lock lock(m_noticeMutex);
        m_noticePath.clear();
        m_source = AZ::Uuid{};
        m_failed = false;
    }

    bool AssetImport::IsActive() const
    {
        return m_active;
    }

    void AssetImport::SourceNotice(const AZStd::string& relativePath, const AZStd::string& scanFolder, const AZ::Uuid& id, bool failed)
    {
        const QDir folder(QString::fromUtf8(scanFolder.c_str()));
        const QByteArray path = QDir::cleanPath(folder.absoluteFilePath(QString::fromUtf8(relativePath.c_str()))).toUtf8();

        AZStd::scoped_lock lock(m_noticeMutex);
        if (!m_noticePath.empty() && QFileInfo(QString::fromUtf8(m_noticePath.c_str())) == QFileInfo(QString::fromUtf8(path)))
        {
            m_source = id;
            m_failed = m_failed || failed;
        }
    }

    ToolProvider::Result AssetImport::FinishError(AZStd::string_view message, Json& data, Allocator& allocator)
    {
        data.AddMember("error", String(message, allocator), allocator);
        data.AddMember("sourcePath", String(m_destination, allocator), allocator);
        data.AddMember("sourceCopied", m_copied, allocator);
        Cancel();
        return ToolProvider::Result::Complete;
    }

    void AssetImport::BeginObservation(const AZStd::string& destination, AZ::s64 timeoutMs)
    {
        m_destination = destination;
        m_copied = false;
        {
            AZStd::scoped_lock lock(m_noticeMutex);
            m_noticePath = m_destination;
            m_source = AZ::Uuid{};
            m_failed = false;
        }

        m_active = true;
        const auto now = AZStd::chrono::steady_clock::now();
        m_until = now + AZStd::chrono::milliseconds(timeoutMs);
        m_nextQuery = now;
    }

    void AssetImport::SourceFileChanged(AZStd::string relativePath, AZStd::string scanFolder, AZ::Uuid sourceUUID)
    {
        SourceNotice(relativePath, scanFolder, sourceUUID, false);
    }

    void AssetImport::SourceFileFailed(AZStd::string relativePath, AZStd::string scanFolder, AZ::Uuid sourceUUID)
    {
        SourceNotice(relativePath, scanFolder, sourceUUID, true);
    }
} // namespace LocalMcp
