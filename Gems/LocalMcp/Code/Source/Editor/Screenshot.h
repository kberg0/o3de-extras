/*
 * Copyright (c) Contributors to the Open 3D Engine Project.
 * For complete copyright and license terms please see the LICENSE at the root of this distribution.
 *
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 *
 */

#pragma once

#include "BridgeValues.h"

#include <Atom/Feature/Utils/FrameCaptureBus.h>
#include <AzCore/Component/TickBus.h>
#include <AzCore/Math/Vector3.h>
#include <AzCore/std/optional.h>
#include <Source/Editor/AutoGen/LocalMcpEditorTools.McpTools.h>

#include <QImage>

class QWidget;

namespace LocalMcp
{
    //! Captures the editor viewport, a view pane or the main window as a bounded PNG for the screenshot tool.
    //! Begin() starts a capture that finishes over later frames; Poll() returns the image once it has.
    //! Every call happens on the editor main thread.
    class Screenshot final
        : private AZ::TickBus::Handler
        , private AZ::Render::FrameCaptureNotificationBus::Handler
    {
    public:
        //! Longest side of the returned image; a larger capture is scaled down, keeping its aspect ratio.
        static constexpr int MaxDimension = 2048;

        //! Largest PNG returned, before base64 encoding.
        static constexpr int MaxPngBytes = 8 * 1024 * 1024;

        //! Frames rendered after moving the camera or opening a pane, before the capture.
        static constexpr int SettleFrames = 3;

        ~Screenshot() override;

        //! Checks the argument combinations the schema cannot express: panel goes with target panel, and position and
        //! look_at go together, with target viewport, at different points. The error names the argument.
        static BridgeResult<void> Validate(const ScreenshotArgs& args);

        //! Starts capturing the target of args, which Validate() accepted; a panel must name an existing view pane.
        //! Fails at once when the target cannot be captured, such as the viewport of an editor without a renderer.
        BridgeResult<void> Begin(const ScreenshotArgs& args);

        //! Returns false while the capture runs. Then returns true with the reply in data: the PNG as the image reply member,
        //! with its width, height and target; or fails with the reason the capture failed.
        BridgeResult<bool> Poll(Json& data, Allocator& allocator);

        //! Stops a capture in progress.
        void Cancel();

    private:
        //! What Begin() captures.
        enum class Target
        {
            Viewport,
            Panel,
            Window
        };

        //! Finishes the capture with image, or with error when it is not empty.
        void Finish(QImage image, AZStd::string error = {});

        //! Grabs the contents of widget, or fails when it is missing.
        void Grab(QWidget* widget, AZStd::string_view missing);

        //! Asks the frame capture system for the next viewport frame.
        void CaptureViewport();

        // TickBus overrides
        void OnTick(float deltaTime, AZ::ScriptTimePoint time) override;

        // FrameCaptureNotificationBus overrides
        void OnFrameCaptureFinished(AZ::Render::FrameCaptureResult result, const AZStd::string& info) override;

        Target m_target = Target::Viewport;
        AZStd::string m_targetName;
        AZStd::string m_panel;
        //! Absolute path the viewport capture is written to and read back from.
        AZStd::string m_capturePath;
        int m_framesLeft = 0;
        bool m_finished = false;
        QImage m_image;
        AZStd::string m_error;
    };
} // namespace LocalMcp
