// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#include <PhosphorWorkspaces/WorkspaceMap.h>

#include <algorithm>
#include <iterator>
#include <utility>

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLoggingCategory>
#include <QSet>

Q_LOGGING_CATEGORY(lcWorkspaceMap, "org.phosphor.workspaces.map", QtInfoMsg)

namespace PhosphorWorkspaces {

namespace {
constexpr int WireVersion = 1;
}

QStringList WorkspaceMap::screenOrder() const
{
    return m_screenOrder;
}

void WorkspaceMap::setScreenOrder(const QStringList& order)
{
    // De-duplicate as we build: the order is the slice-concatenation order, so
    // a screen listed twice would have its slice counted and serialized twice
    // (allDesktopIds, toJson, globalPositionForInsert all walk it).
    QStringList next;
    next.reserve(order.size());
    for (const QString& screenId : order) {
        if (!screenId.isEmpty() && !next.contains(screenId)) {
            next.append(screenId);
        }
    }
    // Keep slices reachable: any slice-holding screen missing from the new
    // order is appended in its previous relative position.
    for (const QString& screenId : m_screenOrder) {
        if (!next.contains(screenId) && m_slices.contains(screenId)) {
            next.append(screenId);
        }
    }
    for (auto it = m_slices.constBegin(); it != m_slices.constEnd(); ++it) {
        if (!next.contains(it.key())) {
            next.append(it.key());
        }
    }
    m_screenOrder = next;
}

bool WorkspaceMap::hasScreen(const QString& screenId) const
{
    return m_slices.contains(screenId);
}

bool WorkspaceMap::knowsScreen(const QString& screenId) const
{
    return m_slices.contains(screenId) || m_screenOrder.contains(screenId);
}

QList<WorkspaceEntry> WorkspaceMap::slice(const QString& screenId) const
{
    return m_slices.value(screenId);
}

int WorkspaceMap::sliceSize(const QString& screenId) const
{
    return m_slices.value(screenId).size();
}

QString WorkspaceMap::ownerOf(const QString& desktopId) const
{
    return m_ownerOf.value(desktopId);
}

int WorkspaceMap::sliceIndexOf(const QString& desktopId) const
{
    const QString owner = m_ownerOf.value(desktopId);
    if (owner.isEmpty()) {
        return -1;
    }
    // constFind, not operator[]: QHash::operator[] on a const hash returns by
    // VALUE, so binding it to a reference here deep-copied the whole slice on
    // every lookup, and this runs on the verb hot path.
    const auto slot = m_slices.constFind(owner);
    if (slot == m_slices.constEnd()) {
        return -1;
    }
    const QList<WorkspaceEntry>& entries = slot.value();
    for (int i = 0; i < entries.size(); ++i) {
        if (entries.at(i).desktopId == desktopId) {
            return i;
        }
    }
    return -1;
}

QStringList WorkspaceMap::allDesktopIds() const
{
    QStringList ids;
    for (const QString& screenId : m_screenOrder) {
        const auto entries = m_slices.value(screenId);
        for (const auto& entry : entries) {
            ids.append(entry.desktopId);
        }
    }
    return ids;
}

WorkspaceEntry WorkspaceMap::entryFor(const QString& desktopId) const
{
    const QString owner = m_ownerOf.value(desktopId);
    const auto slot = m_slices.constFind(owner);
    if (slot == m_slices.constEnd()) {
        return {};
    }
    for (const WorkspaceEntry& entry : slot.value()) {
        if (entry.desktopId == desktopId) {
            return entry;
        }
    }
    return {};
}

void WorkspaceMap::insert(const QString& screenId, int sliceIndex, const WorkspaceEntry& entry)
{
    if (entry.desktopId.isEmpty() || screenId.isEmpty()) {
        return;
    }
    if (m_ownerOf.contains(entry.desktopId)) {
        qCWarning(lcWorkspaceMap) << "insert of already-owned desktop" << entry.desktopId << "— repairing by removal";
        remove(entry.desktopId);
    }
    auto& entries = m_slices[screenId];
    const int idx = qBound(0, sliceIndex, static_cast<int>(entries.size()));
    entries.insert(idx, entry);
    m_ownerOf.insert(entry.desktopId, screenId);
    if (!m_screenOrder.contains(screenId)) {
        m_screenOrder.append(screenId);
    }
}

bool WorkspaceMap::remove(const QString& desktopId)
{
    // Look the owner up first and only commit the index removal once the entry
    // is actually found: taking it up front would strip a desktop from the
    // inverse index while leaving it in its slice, and every later lookup for
    // it (ownerOf, entryFor, sliceIndexOf) would then answer "unowned" for a
    // desktop the map still holds.
    const QString owner = m_ownerOf.value(desktopId);
    if (owner.isEmpty()) {
        return false;
    }
    // find(), not operator[]: a stale owner row naming a screen with no slice
    // would otherwise MATERIALIZE an empty one on the way to the repair below,
    // which is exactly the empty-slice state this class does not keep.
    const auto slot = m_slices.find(owner);
    if (slot == m_slices.end()) {
        qCWarning(lcWorkspaceMap) << "owner index named" << owner << "for desktop" << desktopId
                                  << "which holds no slice at all — dropping the stale row";
        m_ownerOf.remove(desktopId);
        return false;
    }
    QList<WorkspaceEntry>& entries = slot.value();
    for (int i = 0; i < entries.size(); ++i) {
        if (entries.at(i).desktopId == desktopId) {
            entries.removeAt(i);
            m_ownerOf.remove(desktopId);
            // An emptied slice leaves the hash entirely. hasScreen() answers
            // "holds a slice", and a screen emptied by a removal or a transfer
            // holds none — keeping a zero-length QList made that answer wrong
            // and also made a map compare unequal to its own JSON round trip,
            // since fromJson never materializes an empty slice.
            if (entries.isEmpty()) {
                m_slices.erase(slot);
            }
            return true;
        }
    }
    // The row named a slice that does not hold the id: garbage by definition,
    // and leaving it costs more than dropping it. insert()'s duplicate repair
    // calls this, sees "not found", and then overwrites the row anyway — so a
    // surviving row would have the desktop counted under the OLD owner by
    // consistentWith and takeSlice while the new slice really holds it.
    //
    // Still false, and the repair is deliberately silent: no slice lost an
    // entry, so the serialized map is byte-identical and there is nothing for
    // a generation bump to announce. The return value answers "did the
    // published map change", not "did this call touch anything" — see the
    // header.
    qCWarning(lcWorkspaceMap) << "owner index named" << owner << "for desktop" << desktopId
                              << "which that slice does not hold — dropping the stale row";
    m_ownerOf.remove(desktopId);
    return false;
}

bool WorkspaceMap::reorderWithinSlice(const QString& desktopId, int newSliceIndex)
{
    const QString owner = m_ownerOf.value(desktopId);
    if (owner.isEmpty()) {
        return false;
    }
    const auto slot = m_slices.find(owner);
    if (slot == m_slices.end()) {
        return false;
    }
    QList<WorkspaceEntry>& entries = slot.value();
    for (int i = 0; i < entries.size(); ++i) {
        if (entries.at(i).desktopId == desktopId) {
            const WorkspaceEntry entry = entries.takeAt(i);
            entries.insert(qBound(0, newSliceIndex, static_cast<int>(entries.size())), entry);
            return true;
        }
    }
    return false;
}

bool WorkspaceMap::reorderSlice(const QString& screenId, const QStringList& orderedIds)
{
    const auto slot = m_slices.find(screenId);
    if (slot == m_slices.end()) {
        return false;
    }
    QList<WorkspaceEntry>& entries = slot.value();
    if (orderedIds.size() != entries.size()) {
        return false;
    }
    QList<WorkspaceEntry> next;
    next.reserve(entries.size());
    for (const QString& desktopId : orderedIds) {
        const auto found = std::find_if(entries.cbegin(), entries.cend(), [&desktopId](const WorkspaceEntry& entry) {
            return entry.desktopId == desktopId;
        });
        if (found == entries.cend()) {
            return false;
        }
        next.append(*found);
    }
    // A repeated id would have matched the same entry twice and still filled
    // the list to size, so the permutation check finishes on the id set.
    const QSet<QString> unique(orderedIds.cbegin(), orderedIds.cend());
    if (unique.size() != orderedIds.size()) {
        return false;
    }
    if (next == entries) {
        return false;
    }
    entries = next;
    return true;
}

bool WorkspaceMap::transfer(const QString& desktopId, const QString& toScreenId, int sliceIndex)
{
    if (toScreenId.isEmpty()) {
        return false;
    }
    const WorkspaceEntry entry = entryFor(desktopId);
    if (entry.desktopId.isEmpty()) {
        return false;
    }
    remove(desktopId);
    insert(toScreenId, sliceIndex, entry);
    return true;
}

void WorkspaceMap::setName(const QString& desktopId, const QString& name)
{
    const QString owner = m_ownerOf.value(desktopId);
    if (owner.isEmpty()) {
        return;
    }
    const auto slot = m_slices.find(owner);
    if (slot == m_slices.end()) {
        return;
    }
    for (WorkspaceEntry& entry : slot.value()) {
        if (entry.desktopId == desktopId) {
            entry.name = name;
            return;
        }
    }
}

void WorkspaceMap::setNamedId(const QString& desktopId, const QString& namedId)
{
    const auto slot = m_slices.find(m_ownerOf.value(desktopId));
    if (slot == m_slices.end()) {
        return;
    }
    for (WorkspaceEntry& entry : slot.value()) {
        if (entry.desktopId == desktopId) {
            entry.namedId = namedId;
            return;
        }
    }
}

bool WorkspaceMap::renameScreen(const QString& fromScreenId, const QString& toScreenId)
{
    if (fromScreenId.isEmpty() || toScreenId.isEmpty() || fromScreenId == toScreenId) {
        return false;
    }
    if (!knowsScreen(fromScreenId) || m_slices.contains(toScreenId)) {
        return false;
    }
    // The order slot is kept, so a rename never moves the slice in the
    // concatenation. A stale order row already naming the new id goes first,
    // or the screen would be listed twice.
    m_screenOrder.removeAll(toScreenId);
    const int at = m_screenOrder.indexOf(fromScreenId);
    if (at >= 0) {
        m_screenOrder[at] = toScreenId;
    } else {
        m_screenOrder.append(toScreenId);
    }
    const QList<WorkspaceEntry> entries = m_slices.take(fromScreenId);
    if (!entries.isEmpty()) {
        for (const WorkspaceEntry& entry : entries) {
            m_ownerOf.insert(entry.desktopId, toScreenId);
        }
        m_slices.insert(toScreenId, entries);
    }
    for (auto it = m_slices.begin(); it != m_slices.end(); ++it) {
        for (WorkspaceEntry& entry : it.value()) {
            if (entry.homeScreenId == fromScreenId) {
                entry.homeScreenId = toScreenId;
            }
        }
    }
    return true;
}

void WorkspaceMap::setHomeScreen(const QString& desktopId, const QString& homeScreenId)
{
    const QString owner = m_ownerOf.value(desktopId);
    if (owner.isEmpty()) {
        return;
    }
    const auto slot = m_slices.find(owner);
    if (slot == m_slices.end()) {
        return;
    }
    for (WorkspaceEntry& entry : slot.value()) {
        if (entry.desktopId == desktopId) {
            entry.homeScreenId = homeScreenId;
            return;
        }
    }
}

QList<WorkspaceEntry> WorkspaceMap::takeSlice(const QString& screenId)
{
    const QList<WorkspaceEntry> entries = m_slices.take(screenId);
    for (const auto& entry : entries) {
        m_ownerOf.remove(entry.desktopId);
    }
    m_screenOrder.removeAll(screenId);
    return entries;
}

void WorkspaceMap::clear()
{
    m_screenOrder.clear();
    m_slices.clear();
    m_ownerOf.clear();
}

uint WorkspaceMap::globalPositionForInsert(const QString& screenId, int sliceIndex) const
{
    uint position = 0;
    for (const QString& id : m_screenOrder) {
        if (id == screenId) {
            break;
        }
        position += static_cast<uint>(m_slices.value(id).size());
    }
    const int size = m_slices.value(screenId).size();
    position += static_cast<uint>(qBound(0, sliceIndex, size));
    return position;
}

bool WorkspaceMap::consistentWith(const QStringList& kwinIds) const
{
    if (m_ownerOf.size() != kwinIds.size()) {
        return false;
    }
    for (const QString& id : kwinIds) {
        if (!m_ownerOf.contains(id)) {
            return false;
        }
    }
    // Owner index must be the exact inverse of the slices.
    int total = 0;
    for (auto it = m_slices.constBegin(); it != m_slices.constEnd(); ++it) {
        total += it.value().size();
        for (const auto& entry : it.value()) {
            if (m_ownerOf.value(entry.desktopId) != it.key()) {
                return false;
            }
        }
    }
    return total == m_ownerOf.size();
}

QStringList WorkspaceMap::repairAgainst(const QStringList& kwinIds)
{
    // Drop entries whose desktop vanished. A slice left with nothing is erased
    // rather than kept as an empty list, the same no-empty-slices invariant
    // remove() and takeSlice() hold. Membership through a set built once: a
    // list lookup per entry is quadratic in the desktop count.
    const QSet<QString> live(kwinIds.cbegin(), kwinIds.cend());
    for (auto it = m_slices.begin(); it != m_slices.end();) {
        auto& entries = it.value();
        for (int i = entries.size() - 1; i >= 0; --i) {
            if (!live.contains(entries.at(i).desktopId)) {
                m_ownerOf.remove(entries.at(i).desktopId);
                entries.removeAt(i);
            }
        }
        it = entries.isEmpty() ? m_slices.erase(it) : std::next(it);
    }
    // Report ids no slice owns, in KWin order, for adoption by the caller.
    QStringList unowned;
    for (const QString& id : kwinIds) {
        if (!m_ownerOf.contains(id)) {
            unowned.append(id);
        }
    }
    return unowned;
}

QString WorkspaceMap::toJson(quint64 generation, const QHash<QString, int>& currentByScreen,
                             const std::function<int(const QString&)>& indexOf, bool includeState) const
{
    QJsonObject root;
    root[QLatin1String("v")] = WireVersion;
    root[QLatin1String("generation")] = static_cast<qint64>(generation);
    root[QLatin1String("screenOrder")] = QJsonArray::fromStringList(m_screenOrder);

    QJsonObject slices;
    for (const QString& screenId : m_screenOrder) {
        QJsonArray sliceArray;
        const auto entries = m_slices.value(screenId);
        const int currentIndex = currentByScreen.value(screenId, 0);
        for (const auto& entry : entries) {
            QJsonObject obj;
            obj[QLatin1String("id")] = entry.desktopId;
            const int index = indexOf ? indexOf(entry.desktopId) : 0;
            if (index > 0) {
                obj[QLatin1String("index")] = index;
                if (index == currentIndex) {
                    obj[QLatin1String("current")] = true;
                }
            }
            if (!entry.name.isEmpty()) {
                obj[QLatin1String("name")] = entry.name;
            }
            if (!entry.namedId.isEmpty()) {
                obj[QLatin1String("namedId")] = entry.namedId;
            }
            if (includeState && !entry.homeScreenId.isEmpty()) {
                obj[QLatin1String("homeScreen")] = entry.homeScreenId;
            }
            sliceArray.append(obj);
        }
        slices[screenId] = sliceArray;
    }
    root[QLatin1String("slices")] = slices;

    return QString::fromUtf8(QJsonDocument(root).toJson(QJsonDocument::Compact));
}

bool WorkspaceMap::fromJson(const QString& json)
{
    QJsonParseError error{};
    const QJsonDocument doc = QJsonDocument::fromJson(json.toUtf8(), &error);
    if (error.error != QJsonParseError::NoError || !doc.isObject()) {
        return false;
    }
    const QJsonObject root = doc.object();
    if (root.value(QLatin1String("v")).toInt() != WireVersion) {
        return false;
    }
    const QJsonArray orderArray = root.value(QLatin1String("screenOrder")).toArray();
    const QJsonObject slices = root.value(QLatin1String("slices")).toObject();

    // screenOrder drives the walk, so anything it omits would be silently
    // dropped and anything it repeats would be walked twice. Build the ordered
    // screen list from the union of both halves, de-duplicated: the order
    // decides sequence, the slices object decides membership.
    QStringList screens;
    for (const QJsonValue& value : orderArray) {
        const QString screenId = value.toString();
        if (!screenId.isEmpty() && !screens.contains(screenId)) {
            screens.append(screenId);
        }
    }
    const QStringList sliceKeys = slices.keys();
    for (const QString& screenId : sliceKeys) {
        if (!screenId.isEmpty() && !screens.contains(screenId)) {
            qCWarning(lcWorkspaceMap) << "restoring slice for" << screenId << "which the stored screen order omits";
            screens.append(screenId);
        }
    }

    // Built in a scratch map and swapped in only once the parse is through,
    // so a caller holding a good map never loses it to a bad file.
    WorkspaceMap parsed;
    for (const QString& screenId : std::as_const(screens)) {
        const QJsonArray sliceArray = slices.value(screenId).toArray();
        QList<WorkspaceEntry> entries;
        for (const QJsonValue& entryValue : sliceArray) {
            const QJsonObject obj = entryValue.toObject();
            WorkspaceEntry entry;
            entry.desktopId = obj.value(QLatin1String("id")).toString();
            // A name is user text from the config, so a stored one can be all
            // whitespace. Trimmed to empty here, because a name that renders as
            // nothing would still make the desktop destroy-exempt and the user
            // would have no way to see why the workspace never goes away.
            entry.name = obj.value(QLatin1String("name")).toString().trimmed();
            entry.namedId = entry.name.isEmpty() ? QString() : obj.value(QLatin1String("namedId")).toString();
            entry.homeScreenId = obj.value(QLatin1String("homeScreen")).toString();
            // The state file is a boundary, but a desktop id is opaque: KWin
            // mints unbraced UUIDs and keeps whatever a kwinrc it loaded
            // carried, so only the empty and the absurd are refused here.
            // Whether a well-formed id still EXISTS is decided later by
            // repairAgainst against KWin's live list, the only authority.
            if (entry.desktopId.isEmpty() || entry.desktopId.size() > MaxDesktopIdLength) {
                qCWarning(lcWorkspaceMap) << "ignoring a stored workspace entry with an unusable desktop id of length"
                                          << entry.desktopId.size();
                continue;
            }
            if (parsed.m_ownerOf.contains(entry.desktopId)) {
                continue;
            }
            entries.append(entry);
            parsed.m_ownerOf.insert(entry.desktopId, screenId);
        }
        parsed.m_screenOrder.append(screenId);
        // Materialize the slice only when it holds something: an empty entry
        // would make hasScreen() true for a screen the restore knows nothing
        // real about, and the reconciler reads hasScreen as "this output is
        // part of the world".
        if (!entries.isEmpty()) {
            parsed.m_slices.insert(screenId, entries);
        }
    }
    *this = parsed;
    return true;
}

} // namespace PhosphorWorkspaces
