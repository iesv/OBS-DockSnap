// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <QByteArray>
#include <QDateTime>
#include <QList>
#include <QString>
#include <QStringList>

class QMainWindow;

namespace docksnap {
inline constexpr int maxSnapshots = 1000;

struct Snapshot {
    QString name;
    QByteArray state;
    QByteArray geometry;
    QStringList docks;
};
struct SnapshotEntry {
    QString fileName;
    QString name;
    QDateTime modified;
};
enum class FileError { None, Open, Invalid, TooLarge, Write };
enum class NameError { None, Invalid, Exists };
Snapshot capture(QMainWindow &window, const QString &name);
bool restore(QMainWindow &window, const Snapshot &snapshot);
QStringList missingDocks(const QMainWindow &window, const Snapshot &snapshot);
NameError checkName(const QString &directory, const QString &name, const QString &excludePath = {});
FileError read(const QString &path, Snapshot &snapshot);
FileError write(const QString &path, const Snapshot &snapshot);
QList<SnapshotEntry> listSnapshots(const QString &directory, const QStringList &order, int &unreadable,
                                  int *overflow = nullptr);
} // namespace docksnap
