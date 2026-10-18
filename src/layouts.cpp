// SPDX-License-Identifier: GPL-2.0-or-later
#include "layouts.hpp"
#include <QDir>
#include <QDockWidget>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLayout>
#include <QMainWindow>
#include <QMap>
#include <QSaveFile>
#include <algorithm>

namespace docksnap {
static constexpr qint64 maxFileSize = 2 * 1024 * 1024;

static QStringList dockNames(const QMainWindow &window)
{
    QStringList names;
    for (auto *dock : window.findChildren<QDockWidget *>()) {
        if (!dock->objectName().isEmpty())
            names.append(dock->objectName());
    }
    names.removeDuplicates();
    names.sort();
    return names;
}

Snapshot capture(QMainWindow &window, const QString &name)
{
    return {name, window.saveState(), window.saveGeometry(), dockNames(window)};
}

bool restore(QMainWindow &window, const Snapshot &snapshot)
{
    const auto previous = capture(window, {});
    const auto previousRect = window.geometry();
    if (snapshot.state.isEmpty() || snapshot.geometry.isEmpty() || !window.restoreGeometry(snapshot.geometry) ||
        !window.restoreState(snapshot.state)) {
        window.restoreGeometry(previous.geometry);
        window.restoreState(previous.state);
        window.layout()->activate();
        // Rollback preserves the existing rectangle, including off-screen edges.
        if (window.windowState() == Qt::WindowNoState)
            window.setGeometry(previousRect);
        return false;
    }
    window.layout()->activate();
    return true;
}

QStringList missingDocks(const QMainWindow &window, const Snapshot &snapshot)
{
    const auto present = dockNames(window);
    QStringList missing;
    for (const auto &name : snapshot.docks) {
        if (!present.contains(name))
            missing.append(name);
    }
    return missing;
}

NameError checkName(const QString &directory, const QString &name, const QString &excludePath)
{
    if (name.isEmpty() || name.size() > 80 || name != name.trimmed() || name.endsWith('.'))
        return NameError::Invalid;
    for (const auto character : name) {
        if (character.unicode() < 32 || QStringLiteral("<>:\"/\\|?*").contains(character))
            return NameError::Invalid;
    }
    const auto stem = name.section('.', 0, 0).trimmed().toUpper();
    if (QStringList{"CON", "PRN", "AUX", "NUL", "CONIN$", "CONOUT$"}.contains(stem) ||
        (stem.size() == 4 && (stem.startsWith("COM") || stem.startsWith("LPT")) &&
         QStringLiteral("123456789\u00b9\u00b2\u00b3").contains(stem.back())))
        return NameError::Invalid;
    const auto key = [](const QString &value) {
        return value.toCaseFolded().normalized(QString::NormalizationForm_C);
    };
    const auto nameKey = key(name);
    // Reserve both filenames and stored names, including unreadable files.
    for (const auto &file : QDir(directory).entryInfoList(
             QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot)) {
        if (!file.fileName().endsWith(".json", Qt::CaseInsensitive))
            continue;
        if (file.absoluteFilePath() == excludePath)
            continue;
        if (key(file.fileName()) == nameKey + ".json")
            return NameError::Exists;
        Snapshot snapshot;
        if (file.isFile() && read(file.absoluteFilePath(), snapshot) == FileError::None &&
            key(snapshot.name) == nameKey)
            return NameError::Exists;
    }
    return NameError::None;
}

QList<SnapshotEntry> listSnapshots(const QString &directory, const QStringList &order, int &unreadable, int *overflow)
{
    unreadable = 0;
    QMap<QString, SnapshotEntry> available;
    for (const auto &file : QDir(directory).entryInfoList({"*.json"}, QDir::Files | QDir::Hidden, QDir::Name)) {
        Snapshot snapshot;
        if (read(file.absoluteFilePath(), snapshot) == FileError::None)
            available.insert(file.fileName(), {file.fileName(), snapshot.name, file.lastModified()});
        else
            ++unreadable;
    }
    QList<SnapshotEntry> entries;
    for (const auto &fileName : order) {
        const auto found = available.find(fileName);
        if (found != available.end()) {
            entries.append(found.value());
            available.erase(found);
        }
    }
    // Existing settings keep their order; newly discovered files follow them.
    QList<SnapshotEntry> added;
    for (auto it = available.cbegin(); it != available.cend(); ++it)
        added.append(it.value());
    std::stable_sort(added.begin(), added.end(), [](const auto &a, const auto &b) {
        return QString::localeAwareCompare(a.name, b.name) < 0;
    });
    entries.append(added);
    const bool exceeded = entries.size() > maxSnapshots;
    if (overflow)
        *overflow = exceeded ? static_cast<int>(entries.size() - maxSnapshots) : 0;
    if (exceeded)
        entries.resize(maxSnapshots);
    return entries;
}

FileError read(const QString &path, Snapshot &snapshot)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return FileError::Open;
    if (file.size() > maxFileSize)
        return FileError::TooLarge;
    const auto bytes = file.read(maxFileSize + 1);
    QJsonParseError error;
    const auto document = QJsonDocument::fromJson(bytes, &error);
    if (error.error != QJsonParseError::NoError || !document.isObject())
        return FileError::Invalid;
    const auto object = document.object();
    if (object.value("format").toInt() != 1 || object.value("plugin").toString() != "obs-docksnap" ||
        !object.value("name").isString() || !object.value("state").isString() ||
        !object.value("geometry").isString() || !object.value("docks").isArray())
        return FileError::Invalid;
    const auto state = QByteArray::fromBase64Encoding(object.value("state").toString().toLatin1(),
                                                     QByteArray::AbortOnBase64DecodingErrors);
    const auto geometry = QByteArray::fromBase64Encoding(object.value("geometry").toString().toLatin1(),
                                                        QByteArray::AbortOnBase64DecodingErrors);
    Snapshot loaded{object.value("name").toString().trimmed().normalized(QString::NormalizationForm_C),
                    state.decoded, geometry.decoded, {}};
    if (!state || !geometry || loaded.name.isEmpty() || loaded.name.size() > 80 ||
        loaded.state.isEmpty() || loaded.geometry.isEmpty())
        return FileError::Invalid;
    const auto docks = object.value("docks").toArray();
    for (const auto &dock : docks) {
        if (!dock.isString())
            return FileError::Invalid;
        loaded.docks.append(dock.toString());
    }
    snapshot = loaded;
    return FileError::None;
}

FileError write(const QString &path, const Snapshot &snapshot)
{
    if (snapshot.name.trimmed().isEmpty() || snapshot.name.size() > 80 ||
        snapshot.state.isEmpty() || snapshot.geometry.isEmpty())
        return FileError::Invalid;
    const QJsonObject object{{"format", 1}, {"plugin", "obs-docksnap"}, {"obs", "32.0.4"},
                             {"name", snapshot.name}, {"state", QString::fromLatin1(snapshot.state.toBase64())},
                             {"geometry", QString::fromLatin1(snapshot.geometry.toBase64())},
                             {"docks", QJsonArray::fromStringList(snapshot.docks)}};
    const auto data = QJsonDocument(object).toJson();
    if (data.size() > maxFileSize)
        return FileError::TooLarge;
    QSaveFile file(path);
    // Atomic replacement preserves the existing file on write failure.
    if (!file.open(QIODevice::WriteOnly))
        return FileError::Open;
    if (file.write(data) != data.size() || !file.commit())
        return FileError::Write;
    return FileError::None;
}
} // namespace docksnap
