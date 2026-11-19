// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <PhosphorEngineRuntime/DmabufThumbnail.h>

#include <QByteArrayList>
#include <QString>
#include <QVersionNumber>

#include <functional>

class QQmlEngine;
class QVulkanInstance;

namespace PhosphorEngineRuntime {

/**
 * @brief What the overlays need from the host that presents them, beyond the
 * layer-shell transport.
 *
 * plasmazonesd presents through Qt's Wayland platform plugin, picks the
 * graphics API and Vulkan instance at startup, and imports the KWin effect's
 * dma-buf window thumbnails on the GPU. A host that renders the overlays into
 * its own scene answers with its own render backend and may import nothing.
 *
 * Given to OverlayService at construction (EngineRuntime::Platform::presentation)
 * and owned by it; it outlives the overlay QML engine, which the service
 * destroys first.
 */
class ISurfacePresentationHost
{
public:
    virtual ~ISurfacePresentationHost() = default;

    /// How the overlay windows render.
    struct RenderBackend
    {
        /// Where the graphics pipeline cache lives across runs. Empty: none.
        QString pipelineCachePath;
        /// The Vulkan instance the process already made. Null lets Qt make one
        /// when the graphics API is Vulkan.
        QVulkanInstance* vulkanInstance = nullptr;
        QVersionNumber vulkanApiVersion = QVersionNumber(1, 1);
        /// Extra device extensions the host's thumbnail import needs.
        QByteArrayList vulkanDeviceExtensions;
    };
    virtual RenderBackend renderBackend() const = 0;

    /// The overlay QML engine exists: register the host's image providers on
    /// it. Called once per engine.
    virtual void configureQmlEngine(QQmlEngine& engine) = 0;

    /// Called with the image:// URL of an imported thumbnail once it can be
    /// shown.
    using RevealThumbnail = std::function<void(const QString& url)>;

    /// A window thumbnail arrived as a dma-buf. Returns whether the host took
    /// it: false makes the producer fall back to raw pixels. When it is taken,
    /// @p reveal runs, now or once the buffer is ready, with its URL.
    virtual bool importThumbnail(const QString& compositorHandle, const DmabufThumbnailDesc& desc,
                                 RevealThumbnail reveal) = 0;

    /// The URL of an already-revealed imported thumbnail for the window, or
    /// empty, for the warm cache snap assist reads when it opens.
    virtual QString thumbnailUrlFor(const QString& compositorHandle) const = 0;

    /// Snap assist's thumbnail cache went cold: drop the imported thumbnails.
    virtual void clearThumbnails() = 0;
};

} // namespace PhosphorEngineRuntime
