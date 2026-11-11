// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

// One source for every Facet test module. tests/CMakeLists.txt builds it once
// per variant with its own fake_module_config.h and metadata.json, so the
// variants differ only in their metadata and in FAKE_MODE. "Pages" below means
// context.metadata().pages, which is parents-first, not declaration order.
//   0  register every page and extension, and one headless domain
//   1  skip the last page
//   2  also register a page the metadata does not declare
//   3  register the first page with a controller whose id is wrong
//   4  every domain emits applyResult twice per apply()
//   5  register the first page twice
//   6  register the first page with no controller
//   7  register the headless domain twice
//   8  also register a null domain
//   9  every domain emits discardResult twice per discard()
//  10  every domain deletes itself (deleteLater) in apply()
//  12  copy the load record's content, read inside registerInto, into the
//      application property facetLoadRecord. The record's path comes from
//      the FACET_TEST_LOAD_RECORD environment variable.
//  13  every domain emits a second applyResult from the event loop
//  14  every domain reports a failed apply()
//  16  register the first page with no QML source
//  17  every domain, the headless one and each extension's, is dirty from
//      the start
//  18  every domain sends no applyResult at all
//  19  the first page points at a QML file the module does not contain
//  20  every domain sends its one applyResult from a timer, well after apply()
//  21  the first page has a relative QML source
//  22  skip the last extension
//  23  also register an extension the metadata does not declare, and the
//      first extension a second time
//  24  register the first extension with no domain
//  25  register the first extension with the headless domain
//  26  the first extension has a relative QML source
//  27  the first extension points at a QML file the module does not contain
//  28  register the first extension with a domain that lives in another
//      thread
//  29  the headless domain's first isDirty() deletes the first extension's
//      domain and the first page's controller, as module code running while
//      the host registers what passed its checks
//
// Every page and domain has an invokable setDirty(bool), so a test can stage
// an edit without knowing these classes. A domain's resetToDefaults() clears
// it (a page's does nothing), so a test can see a page reset reach an
// extension's section. Every page and section shows qrc:/fake/Page.qml, which
// fake.qrc compiles into the module.

#include "fake_module_config.h"

#include <PhosphorControl/FacetContext.h>
#include <PhosphorControl/FacetModule.h>
#include <PhosphorControl/PageController.h>
#include <PhosphorControl/StagingDomain.h>

#include <QCoreApplication>
#include <QFile>
#include <QObject>
#include <QPointer>
#include <QThread>
#include <QTimer>
#include <QUrl>

#include <utility>

static_assert((FAKE_MODE >= 0 && FAKE_MODE <= 10) || (FAKE_MODE >= 12 && FAKE_MODE <= 14)
                  || (FAKE_MODE >= 16 && FAKE_MODE <= 29),
              "FAKE_MODE must be one of the modes listed above");

namespace {

void emitApply(PhosphorControl::StagingDomain* domain)
{
    if (FAKE_MODE == 18) {
        return;
    }
    if (FAKE_MODE == 20) {
        QTimer::singleShot(200, domain, [domain]() {
            Q_EMIT domain->applyResult(true, QString());
        });
        return;
    }
    if (FAKE_MODE == 14) {
        Q_EMIT domain->applyResult(false, QStringLiteral("disk full"));
        return;
    }
    Q_EMIT domain->applyResult(true, QString());
    if (FAKE_MODE == 4) {
        Q_EMIT domain->applyResult(true, QString());
    }
    if (FAKE_MODE == 13) {
        QTimer::singleShot(0, domain, [domain]() {
            Q_EMIT domain->applyResult(true, QString());
        });
    }
    if (FAKE_MODE == 10) {
        domain->deleteLater();
    }
}

void emitDiscard(PhosphorControl::StagingDomain* domain)
{
    Q_EMIT domain->discardResult(true, QString());
    if (FAKE_MODE == 9) {
        Q_EMIT domain->discardResult(true, QString());
    }
}

class FakePage : public PhosphorControl::PageController
{
    Q_OBJECT
public:
    FakePage(const QString& id, QObject* parent)
        : PageController(id, parent)
    {
    }
    bool isDirty() const override
    {
        return m_dirty;
    }
    Q_INVOKABLE void setDirty(bool dirty)
    {
        if (m_dirty != dirty) {
            m_dirty = dirty;
            Q_EMIT dirtyChanged();
        }
    }
    void apply() override
    {
        setDirty(false);
        emitApply(this);
    }
    void discard() override
    {
        setDirty(false);
        emitDiscard(this);
    }

private:
    bool m_dirty = false;
};

class FakeDomain : public PhosphorControl::StagingDomain
{
    Q_OBJECT
public:
    explicit FakeDomain(QObject* parent)
        : StagingDomain(parent)
        , m_dirty(FAKE_MODE == 17)
    {
    }
    bool isDirty() const override
    {
        if (FAKE_MODE == 29) {
            // Once, and only what it was handed.
            for (const QPointer<QObject>& victim : std::exchange(m_victims, {})) {
                delete victim.data();
            }
        }
        return m_dirty;
    }
    void deleteOnFirstDirtyCheck(QObject* victim)
    {
        m_victims.append(victim);
    }
    Q_INVOKABLE void setDirty(bool dirty)
    {
        if (m_dirty != dirty) {
            m_dirty = dirty;
            Q_EMIT dirtyChanged();
        }
    }
    void apply() override
    {
        setDirty(false);
        emitApply(this);
    }
    void discard() override
    {
        setDirty(false);
        emitDiscard(this);
    }
    void resetToDefaults() override
    {
        setDirty(false);
    }

private:
    bool m_dirty = false;
    mutable QList<QPointer<QObject>> m_victims;
};

} // namespace

class FakeFacetModule : public QObject, public PhosphorControl::IFacetModule
{
    Q_OBJECT
    Q_PLUGIN_METADATA(IID PhosphorFacetModule_iid FILE "metadata.json")
    Q_INTERFACES(PhosphorControl::IFacetModule)

public:
    void registerInto(PhosphorControl::FacetContext& context) override
    {
        if (FAKE_MODE == 12) {
            QFile record(qEnvironmentVariable("FACET_TEST_LOAD_RECORD"));
            const QByteArray content = record.open(QIODevice::ReadOnly) ? record.readAll() : QByteArray();
            qApp->setProperty("facetLoadRecord", QString::fromUtf8(content));
        }
        const auto& pages = context.metadata().pages;
        QObject* firstPage = nullptr;
        for (qsizetype i = 0; i < pages.size(); ++i) {
            if (FAKE_MODE == 1 && i == pages.size() - 1) {
                continue;
            }
            const QString local = pages.at(i).id;
            const QString id = (FAKE_MODE == 3 && i == 0) ? QStringLiteral("wrong:id") : context.pageId(local);
            QUrl source(QStringLiteral("qrc:/fake/Page.qml"));
            if (i == 0 && FAKE_MODE == 16) {
                source = QUrl();
            } else if (i == 0 && FAKE_MODE == 19) {
                source = QUrl(QStringLiteral("qrc:/fake/Missing.qml"));
            } else if (i == 0 && FAKE_MODE == 21) {
                source = QUrl(QStringLiteral("Page.qml"));
            }
            if (FAKE_MODE == 6 && i == 0) {
                context.registerPage(local, nullptr, source);
                continue;
            }
            auto* const page = new FakePage(id, this);
            context.registerPage(local, page, source);
            if (FAKE_MODE == 29 && i == 0) {
                firstPage = page;
            }
            if (FAKE_MODE == 5 && i == 0) {
                context.registerPage(local, new FakePage(id, this), source);
            }
        }
        if (FAKE_MODE == 2) {
            context.registerPage(QStringLiteral("undeclared"),
                                 new FakePage(context.pageId(QStringLiteral("undeclared")), this),
                                 QUrl(QStringLiteral("qrc:/fake/Page.qml")));
        }
        auto* const domain = new FakeDomain(this);
        domain->setObjectName(QStringLiteral("FakeDomain"));
        if (firstPage) {
            domain->deleteOnFirstDirtyCheck(firstPage);
        }
        context.registerDomain(domain);
        if (FAKE_MODE == 7) {
            context.registerDomain(domain);
        }
        if (FAKE_MODE == 8) {
            context.registerDomain(nullptr);
        }

        const auto& extensions = context.metadata().extensions;
        const QUrl sectionSource(QStringLiteral("qrc:/fake/Page.qml"));
        for (qsizetype i = 0; i < extensions.size(); ++i) {
            if (FAKE_MODE == 22 && i == extensions.size() - 1) {
                continue;
            }
            const QString local = extensions.at(i).id;
            QUrl source = sectionSource;
            if (i == 0 && FAKE_MODE == 26) {
                source = QUrl(QStringLiteral("Section.qml"));
            } else if (i == 0 && FAKE_MODE == 27) {
                source = QUrl(QStringLiteral("qrc:/fake/MissingSection.qml"));
            }
            if (i == 0 && FAKE_MODE == 24) {
                context.registerExtension(local, nullptr, source);
                continue;
            }
            if (i == 0 && FAKE_MODE == 25) {
                context.registerExtension(local, domain, source);
                continue;
            }
            if (i == 0 && FAKE_MODE == 28) {
                // Never started: only its affinity matters. The domain is
                // never deleted, since the check refuses it.
                static QThread elsewhere;
                auto* const foreign = new FakeDomain(nullptr);
                foreign->moveToThread(&elsewhere);
                context.registerExtension(local, foreign, source);
                continue;
            }
            auto* const section = new FakeDomain(this);
            section->setObjectName(local);
            context.registerExtension(local, section, source);
            if (FAKE_MODE == 29 && i == 0) {
                domain->deleteOnFirstDirtyCheck(section);
            }
        }
        if (FAKE_MODE == 23 && !extensions.isEmpty()) {
            context.registerExtension(QStringLiteral("undeclared"), new FakeDomain(this), sectionSource);
            context.registerExtension(extensions.first().id, new FakeDomain(this), sectionSource);
        }
    }
};

#include "fakemodule.moc"
