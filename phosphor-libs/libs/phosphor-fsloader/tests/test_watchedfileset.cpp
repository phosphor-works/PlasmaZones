// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Coverage for WatchedFileSet against a real QFileSystemWatcher. Every row is a
// way a watched file reaches its next state (created late, created under a
// directory that is itself created late, replaced by rename, edited in place,
// deleted, reached through a symlink that is retargeted, under a parent or
// grandparent directory that is replaced by rename, named by a dangling link
// whose target appears) plus the negatives the class exists for: an unrelated
// write beside a watched file or in one of its ancestor directories reports
// nothing, and setPaths never accumulates watches.

#include <PhosphorFsLoader/WatchedFileSet.h>

#include <QDir>
#include <QFile>
#include <QSaveFile>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>

#include <algorithm>
#include <memory>

#include <cstdio>
#include <unistd.h>

using PhosphorFsLoader::WatchedFileSet;

namespace {

/// Short enough to keep the suite fast, long enough that a write and its
/// follow-up events land in one window.
constexpr int kDebounceMs = 20;
/// How long a positive row waits for the signal.
constexpr int kSignalTimeoutMs = 2000;
/// How long a negative row waits to be sure nothing arrives: many debounce
/// windows, so a stray report would have had every chance to fire.
constexpr int kQuietMs = 300;

[[nodiscard]] bool writeInPlace(const QString& path, const QByteArray& content)
{
    QFile f(path);
    return f.open(QIODevice::WriteOnly | QIODevice::Truncate) && f.write(content) == content.size();
}

/// Atomic replacement through a temporary file and rename(), the way editors
/// and QSaveFile save. The path ends up on a NEW inode.
[[nodiscard]] bool replaceByRename(const QString& path, const QByteArray& content)
{
    QSaveFile f(path);
    return f.open(QIODevice::WriteOnly) && f.write(content) == content.size() && f.commit();
}

/// A symlink at @p linkPath holding @p target verbatim (relative or absolute).
[[nodiscard]] bool makeSymlink(const QString& target, const QString& linkPath)
{
    return ::symlink(QFile::encodeName(target).constData(), QFile::encodeName(linkPath).constData()) == 0;
}

/// rename(2): replaces @p to when it is a file or a symlink, the way a dotfile
/// manager switches a link and the way a directory is swapped for a staged copy.
[[nodiscard]] bool renamePath(const QString& from, const QString& to)
{
    return ::rename(QFile::encodeName(from).constData(), QFile::encodeName(to).constData()) == 0;
}

/// Point the symlink at @p linkPath to @p target atomically: a new link beside
/// it, renamed over it. Home Manager switches a generation this way.
[[nodiscard]] bool retargetSymlink(const QString& linkPath, const QString& target)
{
    const QString staged = linkPath + QStringLiteral(".new");
    return makeSymlink(target, staged) && renamePath(staged, linkPath);
}

/// The physical path of the existing @p path and every directory above it, up
/// to `/`, sorted the way `watchedPathsForTest` returns them.
[[nodiscard]] QStringList withAncestors(const QString& path)
{
    QStringList chain;
    QString current = QFileInfo(path).canonicalFilePath();
    while (!current.isEmpty()) {
        chain.append(current);
        if (current == QLatin1String("/")) {
            break;
        }
        current = QFileInfo(current).absolutePath();
    }
    chain.sort();
    return chain;
}

/// The sorted union of @p lists, without duplicates.
[[nodiscard]] QStringList sortedUnion(const QList<QStringList>& lists)
{
    QStringList all;
    for (const QStringList& list : lists) {
        all.append(list);
    }
    all.sort();
    all.erase(std::unique(all.begin(), all.end()), all.end());
    return all;
}

} // namespace

class TestWatchedFileSet : public QObject
{
    Q_OBJECT

private:
    std::unique_ptr<QTemporaryDir> m_tmp;

    [[nodiscard]] QString path(const QString& relative) const
    {
        return QDir::cleanPath(m_tmp->filePath(relative));
    }

    [[nodiscard]] std::unique_ptr<WatchedFileSet> makeSet(const QStringList& paths) const
    {
        auto set = std::make_unique<WatchedFileSet>();
        set->setDebounceInterval(kDebounceMs);
        set->setPaths(paths);
        return set;
    }

private Q_SLOTS:

    void init()
    {
        m_tmp = std::make_unique<QTemporaryDir>();
        QVERIFY(m_tmp->isValid());
    }

    void cleanup()
    {
        m_tmp.reset();
    }

    /// setPaths normalises to clean absolute form, drops empties and
    /// duplicates, keeps caller order, and does not itself report anything.
    void setPaths_normalisesAndDoesNotReport()
    {
        const QString a = path(QStringLiteral("a.json"));
        const QString b = path(QStringLiteral("b.json"));
        QVERIFY(writeInPlace(a, "a"));

        WatchedFileSet set;
        QSignalSpy spy(&set, &WatchedFileSet::filesChanged);
        set.setPaths({b, QString(), m_tmp->path() + QStringLiteral("/./a.json"), a, b});

        QCOMPARE(set.paths(), (QStringList{b, a}));
        QCOMPARE(set.debounceInterval(), WatchedFileSet::DefaultDebounceMs);
        QTest::qWait(WatchedFileSet::DefaultDebounceMs + kQuietMs);
        QCOMPARE(spy.count(), 0);
    }

    /// A path that does not exist yet reports when it is created.
    void missingFile_reportsOnCreation()
    {
        const QString target = path(QStringLiteral("config.json"));
        auto set = makeSet({target});
        QSignalSpy spy(set.get(), &WatchedFileSet::filesChanged);

        QVERIFY(writeInPlace(target, "{}"));
        QVERIFY(spy.wait(kSignalTimeoutMs));
        QCOMPARE(spy.count(), 1);
        QCOMPARE(spy.at(0).at(0).toStringList(), QStringList{target});
    }

    /// The file's parent directory does not exist either. Creating the
    /// directory is not a change of the file and reports nothing, but it must
    /// extend the watches down to it, so the file created inside it afterwards
    /// is still noticed. Every ancestor stays watched: a later replacement of
    /// any of them must be seen too.
    void missingParentDirectory_reportsOnceFileAppears()
    {
        const QString directory = path(QStringLiteral("sub/deeper"));
        const QString target = directory + QStringLiteral("/theme.json");
        auto set = makeSet({target});
        QCOMPARE(set->watchedPathsForTest(), withAncestors(m_tmp->path()));
        QSignalSpy spy(set.get(), &WatchedFileSet::filesChanged);

        QVERIFY(QDir().mkpath(directory));
        // Creating "sub" raised the event on the temporary root, and the
        // re-resolution after it added the directories that now exist, down
        // to the one right above the file, keeping the root and its ancestors.
        QTRY_COMPARE_WITH_TIMEOUT(set->watchedPathsForTest(), withAncestors(directory), kSignalTimeoutMs);
        QTest::qWait(kQuietMs);
        QCOMPARE(spy.count(), 0);

        QVERIFY(writeInPlace(target, "{}"));
        QVERIFY(spy.wait(kSignalTimeoutMs));
        QCOMPARE(spy.at(0).at(0).toStringList(), QStringList{target});
    }

    /// Atomic replacement by rename lands a new inode under the same name.
    /// It reports, and the file watch is re-armed onto the new inode so a
    /// later in-place edit (which only the file's own watch can see) reports
    /// too.
    void atomicReplace_reportsAndRearms()
    {
        const QString target = path(QStringLiteral("config.json"));
        QVERIFY(writeInPlace(target, "one"));
        auto set = makeSet({target});
        QSignalSpy spy(set.get(), &WatchedFileSet::filesChanged);

        QVERIFY(replaceByRename(target, "two!"));
        QVERIFY(spy.wait(kSignalTimeoutMs));
        QCOMPARE(spy.at(0).at(0).toStringList(), QStringList{target});

        spy.clear();
        QVERIFY(writeInPlace(target, "three!!"));
        QVERIFY(spy.wait(kSignalTimeoutMs));
        QCOMPARE(spy.at(0).at(0).toStringList(), QStringList{target});
    }

    /// An in-place edit (same inode) reports.
    void inPlaceEdit_reports()
    {
        const QString target = path(QStringLiteral("config.json"));
        QVERIFY(writeInPlace(target, "one"));
        auto set = makeSet({target});
        QSignalSpy spy(set.get(), &WatchedFileSet::filesChanged);

        QVERIFY(writeInPlace(target, "a longer second body"));
        QVERIFY(spy.wait(kSignalTimeoutMs));
        QCOMPARE(spy.count(), 1);
        QCOMPARE(spy.at(0).at(0).toStringList(), QStringList{target});
    }

    /// Deletion reports, and so does recreating the file afterwards.
    void deletion_reportsThenRecreationReports()
    {
        const QString target = path(QStringLiteral("config.json"));
        QVERIFY(writeInPlace(target, "one"));
        auto set = makeSet({target});
        QSignalSpy spy(set.get(), &WatchedFileSet::filesChanged);

        QVERIFY(QFile::remove(target));
        QVERIFY(spy.wait(kSignalTimeoutMs));
        QCOMPARE(spy.at(0).at(0).toStringList(), QStringList{target});

        spy.clear();
        QVERIFY(writeInPlace(target, "back"));
        QVERIFY(spy.wait(kSignalTimeoutMs));
        QCOMPARE(spy.at(0).at(0).toStringList(), QStringList{target});
    }

    /// Writes beside a watched file wake the directory watch but change no
    /// watched fingerprint, so nothing is reported. Covers both a present and
    /// a missing watched file, since the missing one relies on the directory
    /// watch alone.
    void unrelatedSiblingWrite_reportsNothing()
    {
        const QString present = path(QStringLiteral("config.json"));
        const QString missing = path(QStringLiteral("theme.json"));
        QVERIFY(writeInPlace(present, "one"));
        auto set = makeSet({present, missing});
        QSignalSpy spy(set.get(), &WatchedFileSet::filesChanged);

        const QString sibling = path(QStringLiteral("unrelated.txt"));
        QVERIFY(writeInPlace(sibling, "noise"));
        QVERIFY(replaceByRename(sibling, "more noise"));
        QVERIFY(QFile::remove(sibling));
        QTest::qWait(kQuietMs);
        QCOMPARE(spy.count(), 0);
    }

    /// Changes to several watched files inside one debounce window arrive as
    /// one report listing exactly the files that changed, in paths() order.
    void severalChanges_coalesceIntoOneReport()
    {
        const QString a = path(QStringLiteral("a.json"));
        const QString b = path(QStringLiteral("b.json"));
        const QString c = path(QStringLiteral("c.json"));
        QVERIFY(writeInPlace(a, "a"));
        QVERIFY(writeInPlace(c, "c"));
        auto set = std::make_unique<WatchedFileSet>();
        set->setDebounceInterval(200);
        set->setPaths({a, b, c});
        QSignalSpy spy(set.get(), &WatchedFileSet::filesChanged);

        QVERIFY(writeInPlace(c, "changed c"));
        QVERIFY(writeInPlace(a, "changed a"));
        QVERIFY(spy.wait(kSignalTimeoutMs));
        QCOMPARE(spy.count(), 1);
        QCOMPARE(spy.at(0).at(0).toStringList(), (QStringList{a, c}));
    }

    /// A directory on the way to the file is a symlink, and it is retargeted
    /// the way Home Manager, stow and chezmoi switch a configuration
    /// directory. The rename lands in the directory that holds the link. It
    /// reports, the watch follows the new target, and the old target is no
    /// longer watched.
    void retargetedDirectorySymlink_reportsAndFollows()
    {
        QVERIFY(QDir(m_tmp->path()).mkpath(QStringLiteral("store-a")));
        QVERIFY(QDir(m_tmp->path()).mkpath(QStringLiteral("store-b")));
        const QString oldFile = path(QStringLiteral("store-a/config.json"));
        const QString newFile = path(QStringLiteral("store-b/config.json"));
        QVERIFY(writeInPlace(oldFile, "a"));
        QVERIFY(writeInPlace(newFile, "bb"));
        const QString link = path(QStringLiteral("config"));
        QVERIFY(makeSymlink(path(QStringLiteral("store-a")), link));
        const QString target = link + QStringLiteral("/config.json");

        auto set = makeSet({target});
        QSignalSpy spy(set.get(), &WatchedFileSet::filesChanged);

        QVERIFY(retargetSymlink(link, path(QStringLiteral("store-b"))));
        QVERIFY(spy.wait(kSignalTimeoutMs));
        QCOMPARE(spy.count(), 1);
        QCOMPARE(spy.at(0).at(0).toStringList(), QStringList{target});

        spy.clear();
        QVERIFY(writeInPlace(newFile, "edited through the new target"));
        QVERIFY(spy.wait(kSignalTimeoutMs));
        QCOMPARE(spy.at(0).at(0).toStringList(), QStringList{target});

        spy.clear();
        QVERIFY(writeInPlace(oldFile, "edited in the old target"));
        QTest::qWait(kQuietMs);
        QCOMPARE(spy.count(), 0);
    }

    /// The file's parent directory is replaced by a staged copy through two
    /// renames. A directory watch does not see its own directory being
    /// renamed, so only the watch on the grandparent notices. It reports, and
    /// the watches move to the new directory and the new file.
    void parentDirectoryReplacedByRename_reportsAndFollows()
    {
        QVERIFY(QDir(m_tmp->path()).mkpath(QStringLiteral("conf")));
        QVERIFY(QDir(m_tmp->path()).mkpath(QStringLiteral("conf.new")));
        const QString target = path(QStringLiteral("conf/config.json"));
        QVERIFY(writeInPlace(target, "one"));
        QVERIFY(writeInPlace(path(QStringLiteral("conf.new/config.json")), "two!"));

        auto set = makeSet({target});
        QSignalSpy spy(set.get(), &WatchedFileSet::filesChanged);

        QVERIFY(renamePath(path(QStringLiteral("conf")), path(QStringLiteral("conf.old"))));
        QVERIFY(renamePath(path(QStringLiteral("conf.new")), path(QStringLiteral("conf"))));
        QVERIFY(spy.wait(kSignalTimeoutMs));
        QCOMPARE(spy.count(), 1);
        QCOMPARE(spy.at(0).at(0).toStringList(), QStringList{target});

        spy.clear();
        QVERIFY(writeInPlace(target, "three, in place"));
        QVERIFY(spy.wait(kSignalTimeoutMs));
        QCOMPARE(spy.at(0).at(0).toStringList(), QStringList{target});

        spy.clear();
        QVERIFY(writeInPlace(path(QStringLiteral("conf.old/config.json")), "edited in the retired directory"));
        QTest::qWait(kQuietMs);
        QCOMPARE(spy.count(), 0);
    }

    /// The same with the grandparent: a whole tree above the file's directory
    /// is replaced by rename.
    void grandparentDirectoryReplacedByRename_reportsAndFollows()
    {
        QVERIFY(QDir(m_tmp->path()).mkpath(QStringLiteral("top/conf")));
        QVERIFY(QDir(m_tmp->path()).mkpath(QStringLiteral("top.new/conf")));
        const QString target = path(QStringLiteral("top/conf/config.json"));
        QVERIFY(writeInPlace(target, "one"));
        QVERIFY(writeInPlace(path(QStringLiteral("top.new/conf/config.json")), "two!"));

        auto set = makeSet({target});
        QSignalSpy spy(set.get(), &WatchedFileSet::filesChanged);

        QVERIFY(renamePath(path(QStringLiteral("top")), path(QStringLiteral("top.old"))));
        QVERIFY(renamePath(path(QStringLiteral("top.new")), path(QStringLiteral("top"))));
        QVERIFY(spy.wait(kSignalTimeoutMs));
        QCOMPARE(spy.count(), 1);
        QCOMPARE(spy.at(0).at(0).toStringList(), QStringList{target});

        spy.clear();
        QVERIFY(writeInPlace(target, "three, in place"));
        QVERIFY(spy.wait(kSignalTimeoutMs));
        QCOMPARE(spy.at(0).at(0).toStringList(), QStringList{target});

        spy.clear();
        QVERIFY(writeInPlace(path(QStringLiteral("top.old/conf/config.json")), "edited in the retired tree"));
        QTest::qWait(kQuietMs);
        QCOMPARE(spy.count(), 0);
    }

    /// The watched path is a symlink whose target, in another directory that
    /// does not exist yet, appears later. The watches follow the link, so
    /// creating the target directory reports nothing and creating the target
    /// file reports the link's path.
    void danglingLink_reportsWhenTargetAppears()
    {
        QVERIFY(QDir(m_tmp->path()).mkpath(QStringLiteral("links")));
        const QString target = path(QStringLiteral("links/config.json"));
        QVERIFY(makeSymlink(QStringLiteral("../store/config.json"), target));
        QVERIFY(!QFileInfo::exists(target));

        auto set = makeSet({target});
        QSignalSpy spy(set.get(), &WatchedFileSet::filesChanged);

        QVERIFY(QDir(m_tmp->path()).mkpath(QStringLiteral("store")));
        QTest::qWait(kQuietMs);
        QCOMPARE(spy.count(), 0);

        QVERIFY(writeInPlace(path(QStringLiteral("store/config.json")), "{}"));
        QVERIFY(spy.wait(kSignalTimeoutMs));
        QCOMPARE(spy.count(), 1);
        QCOMPARE(spy.at(0).at(0).toStringList(), QStringList{target});

        spy.clear();
        QVERIFY(writeInPlace(path(QStringLiteral("store/config.json")), "a longer second body"));
        QVERIFY(spy.wait(kSignalTimeoutMs));
        QCOMPARE(spy.at(0).at(0).toStringList(), QStringList{target});
    }

    /// Every ancestor directory is watched, and unrelated files and
    /// directories created, renamed and deleted in them wake the set without
    /// a report: the chain still names the same inodes. The watch set is the
    /// same afterwards.
    void ancestorChurn_reportsNothing()
    {
        QVERIFY(QDir(m_tmp->path()).mkpath(QStringLiteral("a/b")));
        const QString target = path(QStringLiteral("a/b/config.json"));
        QVERIFY(writeInPlace(target, "one"));
        auto set = makeSet({target});
        const QStringList watched = set->watchedPathsForTest();
        QCOMPARE(watched, withAncestors(target));
        QSignalSpy spy(set.get(), &WatchedFileSet::filesChanged);

        for (const QString& ancestor : QStringList{path(QStringLiteral(".")), path(QStringLiteral("a"))}) {
            const QString file = ancestor + QStringLiteral("/noise.txt");
            QVERIFY(writeInPlace(file, "noise"));
            QVERIFY(renamePath(file, file + QStringLiteral(".moved")));
            QVERIFY(QFile::remove(file + QStringLiteral(".moved")));
            const QString directory = ancestor + QStringLiteral("/noise.d");
            QVERIFY(QDir().mkpath(directory));
            QVERIFY(renamePath(directory, directory + QStringLiteral(".moved")));
            QVERIFY(QDir().rmdir(directory + QStringLiteral(".moved")));
            const QString link = ancestor + QStringLiteral("/noise.link");
            QVERIFY(makeSymlink(target, link));
            QVERIFY(QFile::remove(link));
        }
        QTest::qWait(kQuietMs);
        QCOMPARE(spy.count(), 0);
        QCOMPARE(set->watchedPathsForTest(), watched);
    }

    /// Steady churn in an ancestor directory, faster than the debounce, must
    /// not postpone the report of an edit. The churn opens identity windows,
    /// and it never restarts the debounce of a change to the file. A busy
    /// `/tmp` under parallel tests postponed reports this way.
    void ancestorChurn_doesNotPostponeReport()
    {
        QVERIFY(QDir(m_tmp->path()).mkpath(QStringLiteral("a/b")));
        const QString target = path(QStringLiteral("a/b/config.json"));
        QVERIFY(writeInPlace(target, "one"));
        auto set = std::make_unique<WatchedFileSet>();
        set->setDebounceInterval(100);
        set->setPaths({target});
        QSignalSpy spy(set.get(), &WatchedFileSet::filesChanged);

        const QString noise = path(QStringLiteral("noise.txt"));
        QTimer churn;
        churn.setInterval(5);
        connect(&churn, &QTimer::timeout, &churn, [noise] {
            if (QFile file(noise); !file.exists()) {
                if (file.open(QIODevice::WriteOnly)) {
                    file.close();
                }
            } else {
                QFile::remove(noise);
            }
        });
        churn.start();

        QVERIFY(writeInPlace(target, "edited while the ancestor churns"));
        QVERIFY(spy.wait(kSignalTimeoutMs));
        churn.stop();
        QCOMPARE(spy.at(0).at(0).toStringList(), QStringList{target});
    }

    /// setPaths reconciles: watches for a dropped path (its file and the
    /// directories only it needed) are removed, the ancestors two paths share
    /// are watched once, nothing accumulates across calls, and a dropped path
    /// no longer reports.
    void setPaths_reconcilesWatches()
    {
        QVERIFY(QDir(m_tmp->path()).mkpath(QStringLiteral("one")));
        QVERIFY(QDir(m_tmp->path()).mkpath(QStringLiteral("two")));
        const QString dirOne = path(QStringLiteral("one"));
        const QString dirTwo = path(QStringLiteral("two"));
        const QString a = dirOne + QStringLiteral("/a.json");
        const QString b = dirTwo + QStringLiteral("/b.json");
        QVERIFY(writeInPlace(a, "a"));
        QVERIFY(writeInPlace(b, "b"));

        auto set = makeSet({a, b});
        QCOMPARE(set->watchedPathsForTest(), sortedUnion({withAncestors(a), withAncestors(b)}));

        set->setPaths({b});
        QCOMPARE(set->watchedPathsForTest(), withAncestors(b));
        set->setPaths({b});
        QCOMPARE(set->watchedPathsForTest(), withAncestors(b));
        QCOMPARE(set->paths(), QStringList{b});

        QSignalSpy spy(set.get(), &WatchedFileSet::filesChanged);
        QVERIFY(writeInPlace(a, "dropped path edited"));
        QTest::qWait(kQuietMs);
        QCOMPARE(spy.count(), 0);

        QVERIFY(writeInPlace(b, "kept path edited"));
        QVERIFY(spy.wait(kSignalTimeoutMs));
        QCOMPARE(spy.at(0).at(0).toStringList(), QStringList{b});

        set->setPaths({});
        QCOMPARE(set->watchedPathsForTest(), QStringList{});
    }
};

QTEST_MAIN(TestWatchedFileSet)
#include "test_watchedfileset.moc"
