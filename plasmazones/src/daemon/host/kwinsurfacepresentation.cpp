// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#include "kwinsurfacepresentation.h"

#include "daemon/rendering/dmabuffencewaiter.h"
#include "daemon/rendering/dmabuftextureprovider.h"
#include "daemon/rendering/vulkansupport.h"

#include <PhosphorEngineRuntime/Logging.h>

#include <PhosphorProtocol/ServiceConstants.h>

#include <QCoreApplication>
#include <QQmlEngine>
#include <PhosphorPaths/SuiteDirectories.h>

#include <QVulkanInstance>

#include <unistd.h>

namespace PlasmaZones {

KWinSurfacePresentation::KWinSurfacePresentation()
    : m_providerOwned(std::make_unique<DmabufTextureProvider>())
{
    m_provider.store(m_providerOwned.get(), std::memory_order_release);
}

KWinSurfacePresentation::~KWinSurfacePresentation()
{
    // The overlay QML engine dies before this host (OverlayService destroys
    // its surfaces first), but retire the null-out connection anyway so it
    // can never write through destructed storage.
    QObject::disconnect(m_engineDestroyConnection);
    m_provider.store(nullptr, std::memory_order_release);
}

ISurfacePresentationHost::RenderBackend KWinSurfacePresentation::renderBackend() const
{
    RenderBackend backend;
    // The app's cache directory (~/.cache/phosphor-works/plasmazones).
    backend.pipelineCachePath = PhosphorPaths::appCacheDir() + QStringLiteral("/pipeline.cache");
#if QT_CONFIG(vulkan)
    // main() made the instance (createAndRegisterVulkanInstance) and parked it
    // on the application object.
    if (qApp) {
        backend.vulkanInstance = qApp->property(PVulkanInstanceProperty).value<QVulkanInstance*>();
    }
#endif
    backend.vulkanApiVersion = PVulkanApiVersion;
    // Zero-copy dma-buf thumbnail import needs these device extensions enabled
    // on the overlay windows' QRhi Vulkan device; without them
    // vkGetMemoryFdPropertiesKHR is unavailable and the import fails. Skipped
    // when the gate is off, so a session pinned to raw pixels keeps Qt's stock
    // device. Qt enables only the physically-supported subset.
    if (PhosphorProtocol::Service::snapAssistDmabufThumbnailsEnabled()) {
        backend.vulkanDeviceExtensions = {
            QByteArrayLiteral("VK_KHR_external_memory_fd"), QByteArrayLiteral("VK_EXT_external_memory_dma_buf"),
            QByteArrayLiteral("VK_EXT_image_drm_format_modifier"), QByteArrayLiteral("VK_KHR_image_format_list")};
    }
    return backend;
}

void KWinSurfacePresentation::configureQmlEngine(QQmlEngine& engine)
{
    // Re-entrancy: if a second engine is ever configured, the first one owns
    // the first provider, so mint a fresh one rather than leave the second
    // engine without the image scheme.
    if (!m_providerOwned) {
        m_providerOwned = std::make_unique<DmabufTextureProvider>();
        m_provider.store(m_providerOwned.get(), std::memory_order_release);
    }
    engine.addImageProvider(QString::fromLatin1(DmabufTextureProvider::ProviderId), m_providerOwned.release());

    // Compare-exchange against the provider minted for THIS engine, so the
    // first engine's late destroyed signal cannot null a second engine's.
    auto* const providerForThisEngine = m_provider.load(std::memory_order_acquire);
    QObject::disconnect(m_engineDestroyConnection);
    m_engineDestroyConnection = connect(&engine, &QObject::destroyed, this, [this, providerForThisEngine]() {
        auto* expected = providerForThisEngine;
        m_provider.compare_exchange_strong(expected, nullptr, std::memory_order_acq_rel);
    });
}

bool KWinSurfacePresentation::importThumbnail(const QString& compositorHandle, const DmabufThumbnailDesc& desc,
                                              RevealThumbnail reveal)
{
    // Zero-copy GPU thumbnail path, the default transport. The env gate is a
    // kill switch rather than an opt-in: the import is refused here when
    // PHOSPHOR_DMABUF_THUMBNAILS=0, and returning false makes the effect
    // fall back to the raw-pixel setSnapAssistThumbnail path. That fallback is
    // also what covers every driver, backend and format the gate cannot
    // predict, so a preview always appears regardless of dma-buf support.
    static const bool dmabufEnabled = PhosphorProtocol::Service::snapAssistDmabufThumbnailsEnabled();
    if (!dmabufEnabled) {
        return false;
    }
    auto* provider = m_provider.load(std::memory_order_acquire);
    if (!provider || desc.fd < 0) {
        return false;
    }
    // Re-validate shape here, mirroring the raw-pixel sibling: the adaptor's
    // checks only cover the D-Bus path. Beyond dimensions, bound stride and
    // offset: the Vulkan importer feeds them into VkSubresourceLayout
    // unchecked, and a stride below the row's real byte count can only be a
    // corrupt descriptor.
    static constexpr int MaxDimension = PhosphorProtocol::Service::SnapAssistThumbnailMaxDimension;
    if (desc.width <= 0 || desc.height <= 0 || desc.width > MaxDimension || desc.height > MaxDimension
        || desc.stride < static_cast<uint32_t>(desc.width) * 4 || desc.stride > (1u << 30)
        || desc.offset > (1u << 30)) {
        qCDebug(lcOverlay) << "importThumbnail: invalid descriptor shape" << desc.width << "x" << desc.height
                           << "stride=" << desc.stride << "offset=" << desc.offset << "for" << compositorHandle;
        return false;
    }
    // Store the descriptor (the provider dups the borrowed fd) and resolve the
    // GPU image:// URL. The actual Vulkan import happens lazily on the render
    // thread when QML loads that URL through the texture provider.
    const QString providerUrl = provider->insert(compositorHandle, desc);
    if (providerUrl.isEmpty()) {
        return false;
    }
    qCDebug(lcOverlay) << "importThumbnail: stored dma-buf for" << compositorHandle << desc.width << "x" << desc.height
                       << "fourcc=0x" << QString::number(desc.fourcc, 16);

    // Reveal-gate on the render-completion fence: only hand the URL on once the
    // producer's GPU render has finished, so QML never loads (and the render
    // thread never imports) a half-rendered buffer. The wait is event-driven
    // (DmabufFenceWaiter / QSocketNotifier): no busy-poll, no blocked thread.
    //
    // No-fence branch: the D-Bus boundary requires a valid fence fd
    // (OverlayAdaptor rejects an invalid one), so fenceFd < 0 only arises for
    // a direct C++ caller; reveal immediately as a defensive fallback.
    // markRevealed also unlocks the provider's urlFor so the warm-cache pass
    // can re-serve this entry on a later show.
    if (desc.fenceFd < 0) {
        provider->markRevealed(compositorHandle, providerUrl);
        reveal(providerUrl);
        return true;
    }
    const int fenceDup = ::dup(desc.fenceFd);
    if (fenceDup < 0) {
        // Can't watch the fence: reveal now rather than drop the thumbnail.
        // (This is the one branch where a theoretically mid-render buffer can
        // reach QML; under fd exhaustion, showing a torn frame beats showing
        // nothing.)
        provider->markRevealed(compositorHandle, providerUrl);
        reveal(providerUrl);
        return true;
    }
    // 1 s bound: a thumbnail render completes in well under a frame; the cap
    // only guards against a producer that crashed mid-render. The waiter owns
    // fenceDup and self-deletes on signal or timeout.
    //
    // true is returned now (deferred-reveal contract: stored, revealed when the
    // fence signals). If the fence times out (only on a hung or crashed
    // producer) the reveal is dropped for THIS show; the entry stays
    // un-revealed, so the warm-cache pass will not serve it either, and
    // recovery happens when the producer re-posts after its own daemon-ready
    // or cache-trim dedup reset.
    auto* waiter = new DmabufFenceWaiter(fenceDup, /*timeoutMs=*/1000, this);
    connect(waiter, &DmabufFenceWaiter::ready, this,
            [this, compositorHandle, providerUrl, reveal = std::move(reveal)]() {
                // providerUrl names the generation THIS fence belongs to; the
                // provider applies the reveal only if that generation is still
                // the stored one, so a late fence from a superseded post cannot
                // reveal a newer, still-rendering buffer.
                if (auto* gpuProvider = m_provider.load(std::memory_order_acquire)) {
                    gpuProvider->markRevealed(compositorHandle, providerUrl);
                }
                // Matched by handle against snap assist's CURRENT candidates
                // when it fires: a dismissed or re-shown snap assist makes it
                // a harmless no-op.
                reveal(providerUrl);
            });
    return true;
}

QString KWinSurfacePresentation::thumbnailUrlFor(const QString& compositorHandle) const
{
    // urlFor only serves REVEALED entries (render fence signaled), so a warm
    // dma-buf URL is always safe to hand to QML.
    auto* provider = m_provider.load(std::memory_order_acquire);
    return provider ? provider->urlFor(compositorHandle) : QString();
}

void KWinSurfacePresentation::clearThumbnails()
{
    if (auto* provider = m_provider.load(std::memory_order_acquire)) {
        provider->clear();
    }
}

} // namespace PlasmaZones
