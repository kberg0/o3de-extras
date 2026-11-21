/*
 * Copyright (c) Contributors to the Open 3D Engine Project.
 * For complete copyright and license terms please see the LICENSE at the root of this distribution.
 *
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 *
 */

#include "Screenshot.h"

#include "LocalMcpToolProvider.h"

#include <Atom/RPI.Public/ViewportContext.h>
#include <Atom/RPI.Public/ViewportContextBus.h>
#include <AzCore/IO/FileIO.h>
#include <AzCore/IO/SystemFile.h>
#include <AzCore/Math/Transform.h>
#include <AzToolsFramework/API/ToolsApplicationAPI.h>

#include <QBuffer>
#include <QByteArray>
#include <QPixmap>
#include <QWidget>

namespace LocalMcp
{
    namespace
    {
        // The target values of the screenshot tool.
        constexpr AZStd::string_view ViewportTarget = "viewport";
        constexpr AZStd::string_view PanelTarget = "panel";

        //! Where the frame capture system writes the viewport image before it is read back and deleted.
        constexpr const char* CapturePath = "@user@/LocalMcpScreenshot.png";

        constexpr const char* PngFormat = "PNG";
        constexpr const char* PngMimeType = "image/png";

        //! Reads a tagged Vector3 whose form the schema already checked.
        AZ::Vector3 ReadVector3(const Json& value)
        {
            const Json& components = value["value"];
            return AZ::Vector3(components[0].GetFloat(), components[1].GetFloat(), components[2].GetFloat());
        }
    } // namespace

    Screenshot::~Screenshot()
    {
        Cancel();
    }

    BridgeResult<void> Screenshot::Validate(const ScreenshotArgs& args)
    {
        const bool panel = args.m_target == PanelTarget;
        if (panel != args.m_panel.has_value())
        {
            return AZ::Failure(AZStd::string(panel ? "panel: required for target panel" : "panel: only for target panel"));
        }

        const bool camera = args.m_position || args.m_lookAt;
        if (!camera)
        {
            return AZ::Success();
        }
        if (!args.m_position || !args.m_lookAt)
        {
            return AZ::Failure(AZStd::string("position: give position and look_at together"));
        }
        if (args.m_target != ViewportTarget)
        {
            return AZ::Failure(AZStd::string("position: only for target viewport"));
        }
        if (ReadVector3(*args.m_position).IsClose(ReadVector3(*args.m_lookAt)))
        {
            return AZ::Failure(AZStd::string("look_at: must differ from position"));
        }
        return AZ::Success();
    }

    BridgeResult<void> Screenshot::Begin(const ScreenshotArgs& args)
    {
        Cancel();
        m_targetName = args.m_target;
        m_panel = args.m_panel.value_or(AZStd::string());
        m_target = args.m_target == ViewportTarget ? Target::Viewport : args.m_panel ? Target::Panel : Target::Window;

        if (m_target == Target::Viewport)
        {
            bool canCapture = false;
            AZ::Render::FrameCaptureRequestBus::BroadcastResult(canCapture, &AZ::Render::FrameCaptureRequests::CanCapture);
            auto* viewports = AZ::RPI::ViewportContextRequests::Get();
            AZ::RPI::ViewportContextPtr viewport = viewports ? viewports->GetDefaultViewportContext() : nullptr;
            if (!canCapture || !viewport)
            {
                return AZ::Failure(AZStd::string(
                    "The viewport cannot be captured: this editor has no renderer (null RHI) or no viewport; use target window"));
            }

            auto* fileIO = AZ::IO::FileIOBase::GetInstance();
            auto resolved = fileIO ? fileIO->ResolvePath(AZ::IO::PathView(CapturePath)) : AZStd::nullopt;
            if (!resolved)
            {
                return AZ::Failure(AZStd::string("The @user@ folder for the capture file cannot be resolved"));
            }
            m_capturePath = resolved->c_str();

            if (args.m_position)
            {
                viewport->SetCameraTransform(AZ::Transform::CreateLookAt(ReadVector3(*args.m_position), ReadVector3(*args.m_lookAt)));
            }
        }
        else if (m_target == Target::Panel)
        {
            AzToolsFramework::OpenViewPane(m_panel.c_str());
        }

        m_finished = false;
        m_framesLeft = SettleFrames;
        AZ::TickBus::Handler::BusConnect();
        return AZ::Success();
    }

    BridgeResult<bool> Screenshot::Poll(Json& data, Allocator& allocator)
    {
        if (!m_finished)
        {
            return AZ::Success(false);
        }

        QImage image = AZStd::move(m_image);
        const AZStd::string error = AZStd::move(m_error);
        const AZStd::string target = m_targetName;
        Cancel();
        if (!error.empty())
        {
            return AZ::Failure(error);
        }

        if (image.width() > MaxDimension || image.height() > MaxDimension)
        {
            image = image.scaled(MaxDimension, MaxDimension, Qt::KeepAspectRatio, Qt::SmoothTransformation);
        }

        QByteArray png;
        QBuffer buffer(&png);
        if (!buffer.open(QIODevice::WriteOnly) || !image.save(&buffer, PngFormat))
        {
            return AZ::Failure(AZStd::string("The capture could not be encoded as PNG"));
        }
        if (png.size() > MaxPngBytes)
        {
            return AZ::Failure(AZStd::string::format(
                "The PNG is %d bytes, over the %d byte limit; capture a smaller panel", aznumeric_cast<int>(png.size()), MaxPngBytes));
        }

        const QByteArray encoded = png.toBase64();
        Json picture(rapidjson::kObjectType);
        picture.AddMember("data", String({ encoded.constData(), aznumeric_cast<size_t>(encoded.size()) }, allocator), allocator);
        picture.AddMember("mimeType", rapidjson::StringRef(PngMimeType), allocator);
        data.AddMember(rapidjson::StringRef(ImageReplyMember), picture, allocator);
        data.AddMember("target", String(target, allocator), allocator);
        data.AddMember("width", image.width(), allocator);
        data.AddMember("height", image.height(), allocator);
        return AZ::Success(true);
    }

    void Screenshot::Cancel()
    {
        AZ::TickBus::Handler::BusDisconnect();
        AZ::Render::FrameCaptureNotificationBus::Handler::BusDisconnect();
        if (!m_capturePath.empty())
        {
            AZ::IO::SystemFile::Delete(m_capturePath.c_str());
            m_capturePath.clear();
        }
        m_finished = false;
        m_framesLeft = 0;
        m_image = QImage();
        m_error.clear();
    }

    void Screenshot::Finish(QImage image, AZStd::string error)
    {
        AZ::TickBus::Handler::BusDisconnect();
        AZ::Render::FrameCaptureNotificationBus::Handler::BusDisconnect();
        m_image = AZStd::move(image);
        m_error = AZStd::move(error);
        m_finished = true;
    }

    void Screenshot::Grab(QWidget* widget, AZStd::string_view missing)
    {
        if (!widget)
        {
            Finish(QImage(), AZStd::string(missing));
            return;
        }
        Finish(widget->grab().toImage());
    }

    void Screenshot::CaptureViewport()
    {
        AZ::Render::FrameCaptureOutcome outcome =
            AZ::Failure(AZ::Render::FrameCaptureError{ AZStd::string("No frame capture system is running") });
        AZ::Render::FrameCaptureRequestBus::BroadcastResult(outcome, &AZ::Render::FrameCaptureRequests::CaptureScreenshot, m_capturePath);
        if (!outcome)
        {
            Finish(QImage(), "Viewport capture failed: " + outcome.GetError().m_errorMessage);
            return;
        }
        AZ::Render::FrameCaptureNotificationBus::Handler::BusConnect(outcome.GetValue());
    }

    void Screenshot::OnTick([[maybe_unused]] float deltaTime, [[maybe_unused]] AZ::ScriptTimePoint time)
    {
        if (--m_framesLeft > 0)
        {
            return;
        }
        AZ::TickBus::Handler::BusDisconnect();

        switch (m_target)
        {
        case Target::Viewport:
            CaptureViewport();
            break;
        case Target::Panel:
            Grab(AzToolsFramework::GetViewPaneWidget<QWidget>(m_panel.c_str()), "The panel did not open");
            break;
        case Target::Window:
            {
                QWidget* window = nullptr;
                AzToolsFramework::EditorRequests::Bus::BroadcastResult(window, &AzToolsFramework::EditorRequests::GetMainWindow);
                Grab(window, "The editor has no main window");
            }
            break;
        }
    }

    void Screenshot::OnFrameCaptureFinished(AZ::Render::FrameCaptureResult result, const AZStd::string& info)
    {
        if (result != AZ::Render::FrameCaptureResult::Success)
        {
            Finish(QImage(), "Viewport capture failed: " + info);
            return;
        }

        QImage image(QString::fromUtf8(m_capturePath.c_str()));
        if (image.isNull())
        {
            Finish(QImage(), "The viewport capture could not be read back");
            return;
        }
        Finish(AZStd::move(image));
    }
} // namespace LocalMcp
