// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "common/engineruntimenamespace.h"
#include <PhosphorEngineRuntime/ISurfacePresentationHost.h>

#include <QMetaObject>
#include <QObject>

#include <atomic>
#include <memory>

namespace PlasmaZones {

class DmabufTextureProvider;

/**
 * ISurfacePresentationHost for plasmazonesd: overlays through Qt's Wayland
 * platform plugin with the graphics API and Vulkan instance main() chose
 * (daemon/rendering/vulkansupport.h), and the KWin effect's window thumbnails
 * imported from dma-bufs on the GPU (PHOSPHOR_DMABUF_THUMBNAILS, on unless
 * set to 0).
 */
class KWinSurfacePresentation : public QObject, public ISurfacePresentationHost
{
    Q_OBJECT

public:
    KWinSurfacePresentation();
    ~KWinSurfacePresentation() override;

    RenderBackend renderBackend() const override;
    void configureQmlEngine(QQmlEngine& engine) override;
    bool importThumbnail(const QString& compositorHandle, const DmabufThumbnailDesc& desc,
                         RevealThumbnail reveal) override;
    QString thumbnailUrlFor(const QString& compositorHandle) const override;
    void clearThumbnails() override;

private:
    // The provider is built eagerly so the borrowed pointer is non-null from
    // construction on; configureQmlEngine hands ownership to the QML engine
    // (QQmlEngine::addImageProvider takes it) and the engine's destroyed signal
    // nulls the borrowed pointer, so no late import reaches a dead provider.
    std::unique_ptr<DmabufTextureProvider> m_providerOwned;
    std::atomic<DmabufTextureProvider*> m_provider{nullptr};
    QMetaObject::Connection m_engineDestroyConnection;
};

} // namespace PlasmaZones
