/* Copyright (c) Contributors to the Open 3D Engine Project.
 * SPDX-License-Identifier: Apache-2.0 OR MIT */
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
        bool Within(const QDir& root, const QString& path)
        {
            const auto nativeRoot = root.absolutePath().toUtf8();
            const auto nativePath = path.toUtf8();
            return AZ::IO::PathView(nativePath.constData()).IsRelativeTo(AZ::IO::PathView(nativeRoot.constData()));
        }
    }

    void AssetImport::Activate()
    {
        AzToolsFramework::AssetSystemBus::Handler::BusConnect();
    }

    void AssetImport::Deactivate()
    {
        AzToolsFramework::AssetSystemBus::Handler::BusDisconnect();
        Cancel();
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

    ToolProvider::Result AssetImport::FinishError(AZStd::string_view message, Json& data, Allocator& allocator)
    {
        data.AddMember("error", String(message, allocator), allocator);
        data.AddMember("sourcePath", String(m_destination, allocator), allocator);
        data.AddMember("sourceCopied", m_copied, allocator);
        Cancel();
        return ToolProvider::Result::Complete;
    }

    ToolProvider::Result AssetImport::Begin(const Json& args, Json& data, Allocator& allocator, AZStd::string& detail,
        AZ::s64 connectionTimeoutMs)
    {
        if (!IsString(args, "source_path", 4096) || !IsString(args, "dest_path", 4096) ||
            !args["source_path"].GetStringLength() || !args["dest_path"].GetStringLength() ||
            (args.HasMember("timeout_ms") && (!args["timeout_ms"].IsInt64() || args["timeout_ms"].GetInt64() < 1 ||
                args["timeout_ms"].GetInt64() > connectionTimeoutMs - 250)))
        {
            detail = "Expected source_path, dest_path and optional positive timeout_ms below the connection deadline";
            return ToolProvider::Result::InvalidArguments;
        }
        if (m_active)
        {
            data.AddMember("error", "An asset import is already active", allocator);
            return ToolProvider::Result::Complete;
        }
        const QFileInfo source(QString::fromUtf8(args["source_path"].GetString()));
        if (!source.isAbsolute() || !source.isFile() || !source.isReadable())
        {
            detail = "Source must be an absolute readable file";
            return ToolProvider::Result::InvalidArguments;
        }
        const QString projectPath = QFileInfo(QString::fromUtf8(AZ::Utils::GetProjectPath().c_str())).canonicalFilePath();
        if (projectPath.isEmpty())
        {
            data.AddMember("error", "The project root is unavailable", allocator);
            return ToolProvider::Result::Complete;
        }
        const QDir project(projectPath);
        const QString destination = QDir::cleanPath(project.absoluteFilePath(QString::fromUtf8(args["dest_path"].GetString())));
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
        AzToolsFramework::AssetSystemRequestBus::BroadcastResult(available,
            &AzToolsFramework::AssetSystem::AssetSystemRequest::GetAssetSafeFolders, folders);
        bool safe = false;
        for (const auto& folder : folders)
        {
            const QString absoluteFolder = QString::fromUtf8(folder.c_str());
            if (QDir::isAbsolutePath(absoluteFolder) && Within(QDir(absoluteFolder), destination)) { safe = true; break; }
        }
        if (!available || !safe)
        {
            data.AddMember("error", "Asset Processor did not identify the destination as an asset-safe folder", allocator);
            return ToolProvider::Result::Complete;
        }
        const AZ::s64 timeout = args.HasMember("timeout_ms") ? args["timeout_ms"].GetInt64() :
            AZStd::min<AZ::s64>(60000, connectionTimeoutMs - 250);
        BeginObservation(destination.toUtf8().constData(), timeout);
        if (!QDir().mkpath(QFileInfo(destination).absolutePath()) || !QFile::copy(source.absoluteFilePath(), destination))
        {
            return FinishError("Could not copy source into the project; inspect source/destination permissions", data, allocator);
        }
        m_copied = true;
        bool sent = false;
        AzFramework::AssetSystemRequestBus::BroadcastResult(sent,
            &AzFramework::AssetSystem::AssetSystemRequests::EscalateAssetBySearchTerm, AZStd::string_view(m_destination));
        if (!sent) { return FinishError("Source copied, but Asset Processor escalation could not be sent", data, allocator); }
        return ToolProvider::Result::Pending;
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

    ToolProvider::Result AssetImport::Observe(const AZStd::string& path, AZ::s64 timeoutMs, Json& data, Allocator& allocator)
    {
        const QFileInfo source(QString::fromUtf8(path.c_str()));
        if (m_active || timeoutMs <= 0 || !source.isAbsolute() || !source.isFile())
        {
            data.AddMember("error", "Cannot observe this saved source: expected an existing project file and an available deadline", allocator);
            return ToolProvider::Result::Complete;
        }
        const QDir project(QFileInfo(QString::fromUtf8(AZ::Utils::GetProjectPath().c_str())).canonicalFilePath());
        if (!Within(project, source.canonicalFilePath()))
        {
            data.AddMember("error", "Saved source is outside the project", allocator);
            return ToolProvider::Result::Complete;
        }
        BeginObservation(path, timeoutMs);
        bool mapped = false;
        AZStd::string scanFolder;
        AzToolsFramework::AssetSystemRequestBus::BroadcastResult(mapped,
            &AzToolsFramework::AssetSystem::AssetSystemRequest::GenerateRelativeSourcePath,
            path, m_relativeSource, scanFolder);
        if (!mapped || m_relativeSource.empty() || AZ::IO::PathView(m_relativeSource).IsAbsolute())
        { return FinishError("Asset Processor could not map the saved source to a scan-folder-relative path", data, allocator); }
        // Status-only request flushes the native watcher; Poll observes jobs and actual catalog products.
        AzFramework::AssetSystemRequestBus::Broadcast(
            &AzFramework::AssetSystem::AssetSystemRequests::GetAssetStatusSearchType_FlushIO, m_relativeSource,
            static_cast<int>(AzFramework::AssetSystem::RequestAssetStatus::SearchType::Exact));
        return ToolProvider::Result::Pending;
    }

    void AssetImport::SourceNotice(const AZStd::string& relativePath, const AZStd::string& scanFolder,
        const AZ::Uuid& id, bool failed)
    {
        const auto path = QDir::cleanPath(QDir(QString::fromUtf8(scanFolder.c_str())).absoluteFilePath(
            QString::fromUtf8(relativePath.c_str()))).toUtf8();
        AZStd::scoped_lock lock(m_noticeMutex);
        if (!m_noticePath.empty() && QFileInfo(QString::fromUtf8(m_noticePath.c_str())) == QFileInfo(QString::fromUtf8(path)))
        {
            m_source = id;
            m_failed = m_failed || failed;
        }
    }

    void AssetImport::SourceFileChanged(AZStd::string path, AZStd::string folder, AZ::Uuid id)
    {
        SourceNotice(path, folder, id, false);
    }

    void AssetImport::SourceFileFailed(AZStd::string path, AZStd::string folder, AZ::Uuid id)
    {
        SourceNotice(path, folder, id, true);
    }

    ToolProvider::Result AssetImport::Poll(Json& data, Allocator& allocator)
    {
        using namespace AzToolsFramework;
        if (!m_active) { return FinishError("No active asset import", data, allocator); }
        const auto now = AZStd::chrono::steady_clock::now();
        if (now >= m_until) { return FinishError("Asset Processor import timed out; copied source remains in the project", data, allocator); }
        if (now < m_nextQuery) { return ToolProvider::Result::Pending; }
        m_nextQuery = now + AZStd::chrono::milliseconds(250);
        AZ::Uuid source;
        bool failed = false;
        {
            AZStd::scoped_lock lock(m_noticeMutex);
            source = m_source;
            failed = m_failed;
        }
        AZ::Outcome<AssetSystem::JobInfoContainer> jobs;
        AssetSystemJobRequestBus::BroadcastResult(jobs, &AssetSystem::AssetSystemJobRequest::GetAssetJobsInfo, m_destination, false);
        bool processing = !jobs || jobs.GetValue().empty();
        if (!m_copied)
        {
            auto status = AzFramework::AssetSystem::AssetStatus_Unknown;
            AzFramework::AssetSystemRequestBus::BroadcastResult(status,
                &AzFramework::AssetSystem::AssetSystemRequests::GetAssetStatusSearchType, m_relativeSource,
                static_cast<int>(AzFramework::AssetSystem::RequestAssetStatus::SearchType::Exact));
            failed = failed || status == AzFramework::AssetSystem::AssetStatus_Failed;
            processing = processing || status != AzFramework::AssetSystem::AssetStatus_Compiled;
        }
        Json diagnostics(rapidjson::kArrayType);
        if (jobs)
        {
            for (const auto& job : jobs.GetValue())
            {
                if (job.m_status == AssetSystem::JobStatus::Queued || job.m_status == AssetSystem::JobStatus::InProgress ||
                    job.m_status == AssetSystem::JobStatus::Missing) { processing = true; }
                else if (job.m_status != AssetSystem::JobStatus::Completed)
                {
                    failed = true;
                    Json diagnostic(rapidjson::kObjectType);
                    diagnostic.AddMember("jobKey", String(job.m_jobKey, allocator), allocator);
                    diagnostic.AddMember("platform", String(job.m_platform, allocator), allocator);
                    diagnostic.AddMember("status", String(AssetSystem::JobStatusString(job.m_status), allocator), allocator);
                    AZ::Outcome<AZStd::string> log;
                    AssetSystemJobRequestBus::BroadcastResult(log, &AssetSystem::AssetSystemJobRequest::GetJobLog, job.m_jobRunKey);
                    if (log)
                    {
                        diagnostic.AddMember("log", String(AZStd::string_view(log.GetValue()).substr(0, 4096), allocator), allocator);
                        diagnostic.AddMember("logTruncated", log.GetValue().size() > 4096, allocator);
                    }
                    diagnostics.PushBack(diagnostic, allocator);
                    if (diagnostics.Size() == 32)
                    {
                        data.AddMember("jobDiagnosticsTruncated", true, allocator);
                        break;
                    }
                }
            }
        }
        if (failed)
        {
            data.AddMember("jobs", diagnostics, allocator);
            return FinishError("Asset Processor failed to process the imported source", data, allocator);
        }
        if (processing) { return ToolProvider::Result::Pending; }
        if (source.IsNull())
        {
            bool found = false;
            AZ::Data::AssetInfo info;
            AZStd::string folder;
            AssetSystemRequestBus::BroadcastResult(found, &AssetSystem::AssetSystemRequest::GetSourceInfoBySourcePath,
                m_destination.c_str(), info, folder);
            if (!found) { return ToolProvider::Result::Pending; }
            source = info.m_assetId.m_guid;
        }
        AZStd::vector<AZ::Data::AssetInfo> products;
        bool found = false;
        AssetSystemRequestBus::BroadcastResult(found, &AssetSystem::AssetSystemRequest::GetAssetsProducedBySourceUUID, source, products);
        if (!found) { return ToolProvider::Result::Pending; }
        if (products.size() > 4096)
        {
            data.AddMember("productCount", static_cast<uint64_t>(products.size()), allocator);
            return FinishError("Source produced more products than the response budget", data, allocator);
        }
        Json output(rapidjson::kArrayType);
        for (const auto& product : products)
        {
            AZ::Data::AssetId catalog;
            AZ::Data::AssetCatalogRequestBus::BroadcastResult(catalog, &AZ::Data::AssetCatalogRequests::GetAssetIdByPath,
                product.m_relativePath.c_str(), product.m_assetType, false);
            if (catalog != product.m_assetId) { return ToolProvider::Result::Pending; }
            Json item(rapidjson::kObjectType);
            Json id(rapidjson::kObjectType);
            id.AddMember("$type", "AssetId", allocator);
            id.AddMember("guid", String(product.m_assetId.m_guid.ToString<AZStd::string>(), allocator), allocator);
            id.AddMember("subId", product.m_assetId.m_subId, allocator);
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
}
