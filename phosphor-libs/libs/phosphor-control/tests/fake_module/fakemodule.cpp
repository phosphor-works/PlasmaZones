// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

// One source for every Facet test module. tests/CMakeLists.txt builds it once
// per variant with its own fake_module_config.h and metadata.json, so the
// variants differ only in their metadata and in FAKE_MODE. "Pages" below means
// context.metadata().pages, which is parents-first, not declaration order.
//   0  register every page and one headless domain
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
//  17  the headless domain is dirty from the start
//  18  every domain sends no applyResult at all
//  19  the first page points at a QML file the module does not contain
//  20  every domain sends its one applyResult from a timer, well after apply()
//  21  the first page has a relative QML source
//
// Every page and domain has an invokable setDirty(bool), so a test can stage
// an edit without knowing these classes. Every page shows qrc:/fake/Page.qml,
// which fake.qrc compiles into the module.

#include "fake_module_config.h"

#include <PhosphorControl/FacetContext.h>
#include <PhosphorControl/FacetModule.h>
#include <PhosphorControl/PageController.h>
#include <PhosphorControl/StagingDomain.h>

#include <QCoreApplication>
#include <QFile>
#include <QObject>
#include <QTimer>
#include <QUrl>

static_assert((FAKE_MODE >= 0 && FAKE_MODE <= 10) || (FAKE_MODE >= 12 && FAKE_MODE <= 14)
                  || (FAKE_MODE >= 16 && FAKE_MODE <= 21),
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
            context.registerPage(local, new FakePage(id, this), source);
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
        context.registerDomain(domain);
        if (FAKE_MODE == 7) {
            context.registerDomain(domain);
        }
        if (FAKE_MODE == 8) {
            context.registerDomain(nullptr);
        }
    }
};

#include "fakemodule.moc"
