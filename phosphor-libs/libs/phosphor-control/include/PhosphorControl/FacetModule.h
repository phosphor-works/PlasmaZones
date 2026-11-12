// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once

#include <QtPlugin>

namespace PhosphorControl {

class FacetContext;

/// The `ApiVersion` a Facet module's metadata declares, and the version the
/// PhosphorFacetModule_iid below ends in. Both change together.
inline constexpr int FacetModuleApiVersion = 1;

/// The root object of a Facet module.
///
/// A Facet module is a Qt plugin in the `facet` seam: its root QObject
/// implements this interface and declares
/// `Q_PLUGIN_METADATA(IID PhosphorFacetModule_iid FILE "metadata.json")`.
/// The metadata describes the module and its pages (FacetModuleMetadata), so
/// Facet can put them in the sidebar before the module loads. The module
/// loads the first time one of its pages is opened, or a page it adds a
/// section to (its metadata's `Extensions`).
class IFacetModule
{
public:
    virtual ~IFacetModule() = default;

    /// Register this module's page controllers, headless staging domains and
    /// extension sections. Every page and every extension the metadata
    /// declares must be registered, and nothing it does not declare. Runs on
    /// the GUI thread when the module loads. Must not throw.
    ///
    /// Nothing registered here takes effect until registerInto returns and
    /// the whole registration checks out. When it does not, the module fails
    /// and none of it is kept, and a retry calls registerInto again on the
    /// same root object. So create the controllers and domains inside this
    /// call, parented to the root object, rather than once up front.
    ///
    /// FacetContext::configBackend is the exception. It shares the file's
    /// backend at once, and the backend stays shared when the registration
    /// is not kept, so a retry gets the same one back.
    virtual void registerInto(FacetContext& context) = 0;
};

} // namespace PhosphorControl

/// The interface id of a Facet module. Its last component is
/// PhosphorControl::FacetModuleApiVersion.
#define PhosphorFacetModule_iid "org.phosphor_works.Facet.Module/1"
Q_DECLARE_INTERFACE(PhosphorControl::IFacetModule, PhosphorFacetModule_iid)
