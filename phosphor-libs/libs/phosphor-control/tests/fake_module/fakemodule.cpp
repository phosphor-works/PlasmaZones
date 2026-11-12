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
//  17  every page and every domain, the headless one and each extension's,
//      is dirty from the start
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
//  30  also open the config file $FAKE_CONFIG_PATH and register a domain
//      that writes ["Fake"][<module id>] = "applied" there on apply(), and
//      reports a commit that failed as a failed apply()
//  31  open a config file by a relative path
//  32  register the first page with a controller that lives in another
//      thread
//  33  register the first page's controller as a headless domain before it
//      is registered as the page's
//  34  also register a headless domain that lives in another thread
//  35  the first extension has no QML source
//  36  delete the first page's controller once it is registered
//  37  also register a headless domain and delete it
//  38  register the first page with the object in the application property
//      fakeForeignController as its controller
//  39  also register the object in the application property
//      fakeForeignDomain as a headless domain, when the property holds one
//  40  like 30, with a factory that makes no backend
//  41  like 30, with an empty factory
//  42  like 30, with a factory that opens the same file through the context
//      itself before it returns a backend of its own. The backend that
//      nested call returned goes into the application property
//      fakeConfigNestedBackend, the one the outer call returned into
//      fakeConfigOuterBackend, and the domain writes through the nested one.
//  43  like 30, and also register a null domain
//  44  call a function no library defines, so the library cannot be loaded.
//      tests/CMakeLists.txt links this mode so that the call survives into
//      the library.
//  45  also register a search provider with an entity entry and a setting
//      entry on the first page, at its first anchor
//  46  also register a search provider with an entity entry on a page that
//      is not the module's, and the setting entry of 45
//  47  like 45, and also register a null domain
//  48  also register a search provider with a setting entry on the first
//      page at an anchor it does not list ("nope"), a section entry and a
//      setting entry on the first extension's target page, at "speed" and at
//      "gone", an action entry with no page, and an entity entry on the
//      first page with no anchor
//  49  also register a null search provider, and one search provider twice
//  50  every domain reports a failed discard() and deletes itself
//      (deleteLater) in it, and the headless domain has no objectName
//  52  deleteLater() the first page's controller and the first extension's
//      domain once registered, and the headless domain's apply() deletes the
//      last extension's domain
//
// The config modes (30 and 40 to 43) record every call of their factory: the
// file it was asked to open is appended to the application property
// fakeConfigFactoryPaths. Each backend they make adds one to the application
// property fakeConfigBackendsDestroyed when it is destroyed.
//
// Every page and domain has an invokable setDirty(bool), so a test can stage
// an edit without knowing these classes. A domain's resetToDefaults() clears
// it (a page's does nothing), so a test can see a page reset reach an
// extension's section. Every page and section shows qrc:/fake/Page.qml, which
// fake.qrc compiles into the module.

#include "fake_module_config.h"

#include <PhosphorControl/FacetContext.h>
#include <PhosphorControl/FacetModule.h>
#include <PhosphorControl/ISearchProvider.h>
#include <PhosphorControl/PageController.h>
#include <PhosphorControl/StagingDomain.h>

#include <PhosphorConfig/IBackend.h>
#include <PhosphorConfig/JsonBackend.h>

#include <QCoreApplication>
#include <QFile>
#include <QObject>
#include <QPointer>
#include <QStringList>
#include <QThread>
#include <QTimer>
#include <QUrl>
#include <QVariant>

#include <memory>
#include <utility>

static_assert((FAKE_MODE >= 0 && FAKE_MODE <= 10) || (FAKE_MODE >= 12 && FAKE_MODE <= 14)
                  || (FAKE_MODE >= 16 && FAKE_MODE <= 50) || FAKE_MODE == 52,
              "FAKE_MODE must be one of the modes listed above");

#if FAKE_MODE == 44
// Defined by no library. The variant is built with -fplt and links with
// -z undefs and -z lazy, so the reference survives into the module as a lazily
// bound call and only an eager bind at load time reports it.
extern "C" Q_DECL_IMPORT int phosphor_control_test_missing_function();
#endif

namespace {

// A thread that is never started, for an object that has to live in another
// thread than the one registering it. Only the affinity matters, and such an
// object is never deleted, since the check it is made for refuses it.
QThread* otherThread()
{
    static QThread thread;
    return &thread;
}

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
    if (FAKE_MODE == 50) {
        Q_EMIT domain->discardResult(false, QStringLiteral("locked"));
        domain->deleteLater();
        return;
    }
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
    bool m_dirty;
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
            deleteVictims();
        }
        return m_dirty;
    }
    /// Delete @p victim on this domain's first isDirty() in mode 29, and on
    /// its first apply() in mode 52.
    void addVictim(QObject* victim)
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
        if (FAKE_MODE == 52) {
            deleteVictims();
        }
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
    // Once, and only what it was handed.
    void deleteVictims() const
    {
        for (const QPointer<QObject>& victim : std::exchange(m_victims, {})) {
            delete victim.data();
        }
    }

    bool m_dirty = false;
    mutable QList<QPointer<QObject>> m_victims;
};

// Writes its module's key into a shared config file on apply().
class ConfigDomain : public FakeDomain
{
public:
    ConfigDomain(PhosphorConfig::IBackend* backend, QString key, QObject* parent)
        : FakeDomain(parent)
        , m_backend(backend)
        , m_key(std::move(key))
    {
    }
    void apply() override
    {
        m_backend->group(QStringLiteral("Fake"))->writeString(m_key, QStringLiteral("applied"));
        if (!m_backend->commit()) {
            // Nothing was saved, so the edit stays staged.
            Q_EMIT applyResult(false, QStringLiteral("the config file could not be written"));
            return;
        }
        FakeDomain::apply();
    }

private:
    PhosphorConfig::IBackend* m_backend;
    QString m_key;
};

class FakeProvider : public PhosphorControl::ISearchProvider
{
public:
    explicit FakeProvider(QVector<PhosphorControl::SearchEntry> entries)
        : m_entries(std::move(entries))
    {
    }
    QVector<PhosphorControl::SearchEntry> searchEntries() const override
    {
        return m_entries;
    }

private:
    QVector<PhosphorControl::SearchEntry> m_entries;
};
// The backend the config modes make, which counts its destructions in the
// application property fakeConfigBackendsDestroyed, so a test sees when the
// host or the ApplicationController lets go of one.
class CountedBackend : public PhosphorConfig::JsonBackend
{
public:
    using JsonBackend::JsonBackend;
    ~CountedBackend() override
    {
        if (qApp) {
            qApp->setProperty("fakeConfigBackendsDestroyed", qApp->property("fakeConfigBackendsDestroyed").toInt() + 1);
        }
    }
};

// The factory the config modes hand to FacetContext::configBackend. It
// appends the file it was asked to open to the application property
// fakeConfigFactoryPaths, so a test can count the calls and read their
// argument.
std::unique_ptr<PhosphorConfig::IBackend> makeConfigBackend(const QString& file)
{
    QStringList files = qApp->property("fakeConfigFactoryPaths").toStringList();
    files.append(file);
    qApp->setProperty("fakeConfigFactoryPaths", files);
    if (FAKE_MODE == 40) {
        return nullptr;
    }
    return std::make_unique<CountedBackend>(file);
}

} // namespace

class FakeFacetModule : public QObject, public PhosphorControl::IFacetModule
{
    Q_OBJECT
    Q_PLUGIN_METADATA(IID PhosphorFacetModule_iid FILE "facet_metadata.json")
    Q_INTERFACES(PhosphorControl::IFacetModule)

public:
    void registerInto(PhosphorControl::FacetContext& context) override
    {
#if FAKE_MODE == 44
        // Never reached: the library this call is in does not load.
        if (phosphor_control_test_missing_function() != 0) {
            return;
        }
#endif
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
            if (FAKE_MODE == 32 && i == 0) {
                auto* const foreign = new FakePage(id, nullptr);
                foreign->moveToThread(otherThread());
                context.registerPage(local, foreign, source);
                continue;
            }
            if (FAKE_MODE == 38 && i == 0) {
                // A controller the registry already holds: the test hands
                // over this page's own placeholder, whose id matches.
                QObject* const foreign = qApp->property("fakeForeignController").value<QObject*>();
                context.registerPage(local, qobject_cast<PhosphorControl::PageController*>(foreign), source);
                continue;
            }
            auto* const page = new FakePage(id, this);
            if (FAKE_MODE == 33 && i == 0) {
                context.registerDomain(page);
            }
            context.registerPage(local, page, source);
            if (FAKE_MODE == 36 && i == 0) {
                delete page;
                continue;
            }
            if (FAKE_MODE == 29 && i == 0) {
                firstPage = page;
            }
            if (FAKE_MODE == 52 && i == 0) {
                page->deleteLater();
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
        if (FAKE_MODE != 50) {
            domain->setObjectName(QStringLiteral("FakeDomain"));
        }
        if (firstPage) {
            domain->addVictim(firstPage);
        }
        context.registerDomain(domain);
        if (FAKE_MODE == 7) {
            context.registerDomain(domain);
        }
        if (FAKE_MODE == 8) {
            context.registerDomain(nullptr);
        }
        if (FAKE_MODE == 34) {
            auto* const foreign = new FakeDomain(nullptr);
            foreign->moveToThread(otherThread());
            context.registerDomain(foreign);
        }
        if (FAKE_MODE == 37) {
            auto* const doomed = new FakeDomain(this);
            context.registerDomain(doomed);
            delete doomed;
        }
        if (FAKE_MODE == 39) {
            // A domain the application tracks already, when the test handed
            // one over. Without one the module registers like mode 0.
            QObject* const foreign = qApp->property("fakeForeignDomain").value<QObject*>();
            if (auto* const tracked = qobject_cast<PhosphorControl::StagingDomain*>(foreign)) {
                context.registerDomain(tracked);
            }
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
            } else if (i == 0 && FAKE_MODE == 35) {
                source = QUrl();
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
                auto* const foreign = new FakeDomain(nullptr);
                foreign->moveToThread(otherThread());
                context.registerExtension(local, foreign, source);
                continue;
            }
            auto* const section = new FakeDomain(this);
            section->setObjectName(local);
            context.registerExtension(local, section, source);
            if (FAKE_MODE == 29 && i == 0) {
                domain->addVictim(section);
            }
            if (FAKE_MODE == 52 && i == 0) {
                section->deleteLater();
            }
            if (FAKE_MODE == 52 && i > 0 && i == extensions.size() - 1) {
                domain->addVictim(section);
            }
        }
        if (FAKE_MODE == 30 || FAKE_MODE == 31 || (FAKE_MODE >= 40 && FAKE_MODE <= 43)) {
            const QString path =
                FAKE_MODE == 31 ? QStringLiteral("relative.json") : qEnvironmentVariable("FAKE_CONFIG_PATH");
            PhosphorConfig::IBackend* nested = nullptr;
            PhosphorControl::FacetContext::ConfigBackendFactory create = [&context, &path,
                                                                          &nested](const QString& file) {
                if (FAKE_MODE == 42) {
                    nested = context.configBackend(path, &makeConfigBackend);
                }
                return makeConfigBackend(file);
            };
            if (FAKE_MODE == 41) {
                create = nullptr;
            }
            PhosphorConfig::IBackend* backend = context.configBackend(path, create);
            if (FAKE_MODE == 42) {
                qApp->setProperty("fakeConfigNestedBackend",
                                  QVariant::fromValue<quintptr>(reinterpret_cast<quintptr>(nested)));
                qApp->setProperty("fakeConfigOuterBackend",
                                  QVariant::fromValue<quintptr>(reinterpret_cast<quintptr>(backend)));
                // The backend the nested call was given is in use from that
                // moment, so it is the one that has to go on working.
                backend = nested;
            }
            if (backend) {
                auto* const config = new ConfigDomain(backend, context.moduleId(), this);
                config->setObjectName(QStringLiteral("config"));
                context.registerDomain(config);
            }
            if (FAKE_MODE == 43) {
                context.registerDomain(nullptr);
            }
        }
        if (FAKE_MODE == 45 || FAKE_MODE == 46 || FAKE_MODE == 47) {
            using PhosphorControl::SearchEntry;
            QVector<SearchEntry> entries;
            const auto& first = context.metadata().pages.first();
            SearchEntry entity;
            entity.kind = SearchEntry::Kind::Entity;
            entity.pageId = FAKE_MODE == 46 ? QStringLiteral("elsewhere:page") : context.pageId(first.id);
            entity.anchor = QStringLiteral("rule:42");
            entity.title = QStringLiteral("Dynamic entity");
            entries.append(entity);
            SearchEntry setting;
            setting.kind = SearchEntry::Kind::Setting;
            setting.pageId = context.pageId(first.id);
            setting.anchor = first.anchors.value(0);
            setting.title = QStringLiteral("Dynamic setting");
            entries.append(setting);
            m_provider = std::make_unique<FakeProvider>(entries);
            context.registerSearchProvider(m_provider.get());
            if (FAKE_MODE == 47) {
                context.registerDomain(nullptr);
            }
        }
        if (FAKE_MODE == 48) {
            using PhosphorControl::SearchEntry;
            const auto& first = context.metadata().pages.first();
            const QString extended = extensions.first().target;
            QVector<SearchEntry> entries;
            SearchEntry unlisted;
            unlisted.kind = SearchEntry::Kind::Setting;
            unlisted.pageId = context.pageId(first.id);
            unlisted.anchor = QStringLiteral("nope");
            unlisted.title = QStringLiteral("Unlisted anchor");
            entries.append(unlisted);
            SearchEntry section;
            section.kind = SearchEntry::Kind::Section;
            section.pageId = extended;
            section.anchor = QStringLiteral("speed");
            section.title = QStringLiteral("Extended section");
            entries.append(section);
            SearchEntry gone;
            gone.kind = SearchEntry::Kind::Setting;
            gone.pageId = extended;
            gone.anchor = QStringLiteral("gone");
            gone.title = QStringLiteral("Extended setting");
            entries.append(gone);
            SearchEntry action;
            action.kind = SearchEntry::Kind::Action;
            action.actionId = QStringLiteral("open-wizard");
            action.title = QStringLiteral("Open the wizard");
            entries.append(action);
            SearchEntry entity;
            entity.kind = SearchEntry::Kind::Entity;
            entity.pageId = context.pageId(first.id);
            entity.title = QStringLiteral("Unanchored entity");
            entries.append(entity);
            if (!m_provider) {
                m_provider = std::make_unique<FakeProvider>(entries);
            }
            context.registerSearchProvider(m_provider.get());
        }
        if (FAKE_MODE == 49) {
            if (!m_provider) {
                m_provider = std::make_unique<FakeProvider>(QVector<PhosphorControl::SearchEntry>());
            }
            context.registerSearchProvider(nullptr);
            context.registerSearchProvider(m_provider.get());
            context.registerSearchProvider(m_provider.get());
        }
        if (FAKE_MODE == 23 && !extensions.isEmpty()) {
            context.registerExtension(QStringLiteral("undeclared"), new FakeDomain(this), sectionSource);
            context.registerExtension(extensions.first().id, new FakeDomain(this), sectionSource);
        }
    }

private:
    std::unique_ptr<FakeProvider> m_provider;
};

#include "fakemodule.moc"
