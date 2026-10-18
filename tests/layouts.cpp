// SPDX-License-Identifier: GPL-2.0-or-later
#include "layouts.hpp"
#include <QApplication>
#include <QDir>
#include <QDockWidget>
#include <QEvent>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QMainWindow>
#include <QTemporaryDir>
#include <cstdlib>
#include <iostream>
#ifdef _WIN32
#include <windows.h>
#include <psapi.h>
#else
#include <mach/mach.h>
#endif

static size_t residentMemory()
{
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS info{};
    if (!GetProcessMemoryInfo(GetCurrentProcess(), &info, static_cast<DWORD>(sizeof(info))))
        return 0;
    return info.WorkingSetSize;
#else
    mach_task_basic_info info{};
    mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
    if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO, reinterpret_cast<task_info_t>(&info), &count) != KERN_SUCCESS)
        return 0;
    return static_cast<size_t>(info.resident_size);
#endif
}

static void check(bool condition, const char *message)
{
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

static void checkSnapshotLimit(const docksnap::Snapshot &sample)
{
    QTemporaryDir temp;
    check(temp.isValid(), "snapshot limit temporary directory");
    const auto fileName = [](int index) {
        return QString("snapshot-%1.json").arg(index, 4, 10, QLatin1Char('0'));
    };
    const auto save = [&](int index) {
        auto snapshot = sample;
        snapshot.name = fileName(index).chopped(5);
        check(docksnap::write(temp.filePath(fileName(index)), snapshot) == docksnap::FileError::None,
              "save limit fixture");
    };
    for (int i = 0; i < docksnap::maxSnapshots - 1; ++i)
        save(i);
    QFile bad(temp.filePath("broken.json"));
    check(bad.open(QIODevice::WriteOnly), "create damaged limit fixture");
    bad.write("invalid");
    bad.close();
    int unreadable = 0, overflow = -1;
    check(docksnap::listSnapshots(temp.path(), {}, unreadable, &overflow).size() == docksnap::maxSnapshots - 1 &&
              unreadable == 1 && overflow == 0,
          "999 valid snapshots leave room; damaged files do not count");
    save(docksnap::maxSnapshots - 1);
    check(docksnap::listSnapshots(temp.path(), {}, unreadable, &overflow).size() == docksnap::maxSnapshots &&
              overflow == 0,
          "exactly 1000 snapshots are retained");
    save(docksnap::maxSnapshots);
    const QStringList order{fileName(docksnap::maxSnapshots), fileName(500)};
    auto entries = docksnap::listSnapshots(temp.path(), order, unreadable, &overflow);
    check(entries.size() == docksnap::maxSnapshots && overflow == 1 && unreadable == 1 &&
              entries[0].fileName == order[0] && entries[1].fileName == order[1],
          "1001 snapshots are capped using the saved order");
    const auto hidden = fileName(docksnap::maxSnapshots - 1);
    check(entries.back().fileName != hidden && QFile::exists(temp.filePath(hidden)) &&
              QDir(temp.path()).entryList({"*.json"}, QDir::Files).size() == docksnap::maxSnapshots + 2,
          "over-limit files remain on disk");
    check(QFile::remove(temp.filePath(fileName(500))), "delete a managed snapshot at the limit");
    entries = docksnap::listSnapshots(temp.path(), order, unreadable, &overflow);
    check(entries.size() == docksnap::maxSnapshots && overflow == 0 && entries.back().fileName == hidden,
          "refresh reveals an excess snapshot after a listed snapshot is deleted");
    check(QFile::remove(temp.filePath(fileName(0))), "free one snapshot slot");
    check(docksnap::listSnapshots(temp.path(), order, unreadable).size() < docksnap::maxSnapshots,
          "deleting below the limit allows another snapshot");
}

static void checkSnapshotOrder(const docksnap::Snapshot &sample)
{
    QTemporaryDir temp;
    check(temp.isValid(), "snapshot order temporary directory");
    int unreadable = 0;
    check(docksnap::listSnapshots(temp.path(), {"missing.json"}, unreadable).isEmpty() && unreadable == 0,
          "empty directory ignores stale order");
    const auto save = [&](const QString &fileName, const QString &name) {
        auto snapshot = sample;
        snapshot.name = name;
        check(docksnap::write(temp.filePath(fileName), snapshot) == docksnap::FileError::None,
              "save ordering fixture");
    };
    const auto files = [&](const QStringList &order) {
        QStringList result;
        for (const auto &entry : docksnap::listSnapshots(temp.path(), order, unreadable))
            result.append(entry.fileName);
        return result;
    };
    save("a-file.json", "C");
    check(files({}) == QStringList{"a-file.json"}, "single snapshot order");
    const auto initial = docksnap::listSnapshots(temp.path(), {}, unreadable);
    check(initial.front().modified.isValid() &&
              initial.front().modified == QFileInfo(temp.filePath("a-file.json")).lastModified(),
          "listing retains the actual file modification time");
    save("b-file.json", "B");
    save("c-file.json", "A");
    QFile bad(temp.filePath("broken.json"));
    check(bad.open(QIODevice::WriteOnly), "create damaged ordering fixture");
    bad.write("invalid");
    bad.close();
    check(files({}) == QStringList{"c-file.json", "b-file.json", "a-file.json"} && unreadable == 1,
          "first use sorts by display name and skips damaged files");
    const QStringList custom{"a-file.json", "c-file.json", "a-file.json", "missing.json", "broken.json"};
    auto order = files(custom);
    check(order == QStringList{"a-file.json", "c-file.json", "b-file.json"},
          "custom order removes duplicates and missing files, then appends omitted snapshots");
    const auto bytes = QJsonDocument(QJsonObject{{"snapshotOrder", QJsonArray::fromStringList(order)}}).toJson();
    order.clear();
    for (const auto &value : QJsonDocument::fromJson(bytes).object().value("snapshotOrder").toArray())
        order.append(value.toString());
    check(files(order) == order, "custom order survives settings JSON round trip");
    save("new.json", "0");
    auto expected = order;
    expected.append("new.json");
    check(files(order) == expected, "new snapshot appends despite its alphabetically earlier name");
    save("a-file.json", "Z");
    check(files(expected) == expected, "overwriting metadata preserves position");
    check(QFile::remove(temp.filePath("c-file.json")), "delete ordered snapshot");
    expected.removeAll("c-file.json");
    check(files(order) == expected, "deletion preserves the order of remaining snapshots");
}

int main(int argc, char **argv)
{
    std::cout << std::unitbuf;
    std::cout << "Starting Qt layout tests\n";
    QApplication app(argc, argv);
    std::cout << "Qt application ready (" << qVersion() << ")\n";
    const bool stress = app.arguments().contains("--stress");
    QTemporaryDir temp;
    check(temp.isValid(), "temporary directory");
    QMainWindow window;
    window.setAnimated(false);
    window.setCentralWidget(new QLabel("preview", &window));
    const auto dock = [&](const char *name, Qt::DockWidgetArea area) {
        auto *widget = new QDockWidget(name, &window);
        widget->setObjectName(name);
        widget->setWidget(new QLabel(name, widget));
        window.addDockWidget(area, widget);
        return widget;
    };
    auto *scenes = dock("scenes", Qt::LeftDockWidgetArea);
    auto *sources = dock("sources", Qt::RightDockWidgetArea);
    auto *mixer = dock("mixer", Qt::BottomDockWidgetArea);
    window.resize(960, 640);
    window.show();
    app.processEvents();
    const auto original = docksnap::capture(window, QString::fromUtf8("直播 / 佈局 🎥"));
    const auto path = temp.filePath(QString::fromUtf8("中文快照.json"));
    check(docksnap::write(path, original) == docksnap::FileError::None, "save Unicode name and path");
    docksnap::Snapshot saved;
    check(docksnap::read(path, saved) == docksnap::FileError::None, "read snapshot");
    check(saved.name == original.name && saved.state == original.state && saved.docks == original.docks,
          "snapshot round trip");
    check(docksnap::checkName(temp.path(), QString::fromUtf8("直播布局 🎥")) == docksnap::NameError::None,
          "portable Unicode filename accepted");
    for (const auto &name : QStringList{"", "../outside", "a/b", "a\\b", "a:b", "CON.txt", "LPT1", "ending."})
        check(docksnap::checkName(temp.path(), name) == docksnap::NameError::Invalid, "unsafe filename rejected");
    check(docksnap::checkName(temp.path(), QString(81, 'a')) == docksnap::NameError::Invalid,
          "oversized filename rejected");
    check(docksnap::checkName(temp.path(), QString::fromUtf8("中文快照")) == docksnap::NameError::Exists,
          "existing filename cannot be replaced by a new snapshot");
    const auto namedPath = temp.filePath("stored-id.json");
    auto named = original;
    named.name = "Studio";
    check(docksnap::write(namedPath, named) == docksnap::FileError::None, "create named snapshot");
    for (const auto &name : QStringList{"Studio", "studio", "STUDIO"}) {
        check(docksnap::checkName(temp.path(), name) == docksnap::NameError::Exists,
              "stored names are unique regardless of case or filename");
        check(docksnap::checkName(temp.path(), name, namedPath) == docksnap::NameError::None,
              "rename and overwrite exclude only the current file");
    }
    const auto uppercasePath = temp.filePath("MixedCase.JSON");
    QFile uppercase(uppercasePath);
    check(uppercase.open(QIODevice::WriteOnly), "create unreadable uppercase-extension snapshot");
    uppercase.write("invalid");
    uppercase.close();
    for (const auto &name : QStringList{"MixedCase", "mixedcase", "MIXEDCASE"})
        check(docksnap::checkName(temp.path(), name) == docksnap::NameError::Exists,
              "uppercase extension and unreadable contents cannot bypass duplicate detection");
    check(QDir(temp.path()).mkdir("Folder.JSON"), "create directory occupying a snapshot path");
    check(docksnap::checkName(temp.path(), "folder") == docksnap::NameError::Exists,
          "directory at a case-equivalent snapshot path cannot be replaced");
    check(docksnap::checkName(temp.path(), "STUDIO", uppercasePath) == docksnap::NameError::Exists,
          "excluding another file does not bypass an existing stored name");
    named.name = QString::fromUtf8("Café");
    check(docksnap::write(namedPath, named) == docksnap::FileError::None, "create composed Unicode name");
    check(docksnap::checkName(temp.path(), QStringLiteral("Cafe\u0301")) == docksnap::NameError::Exists,
          "equivalent Unicode names cannot create duplicates");
    checkSnapshotOrder(original);
    checkSnapshotLimit(original);
    std::cout << "Snapshot storage and ordering passed\n";
    window.addDockWidget(Qt::TopDockWidgetArea, scenes);
    mixer->hide();
    const auto changed = docksnap::capture(window, "changed");
    check(docksnap::restore(window, saved), "settle saved layout on the current screen");
    const auto savedRect = window.geometry();
    window.resize(800, 500);
    check(docksnap::restore(window, saved), "restore layout and main window");
    check(window.geometry() == savedRect, "main window geometry restored");
    check(window.dockWidgetArea(scenes) == Qt::LeftDockWidgetArea && !mixer->isHidden(),
          "dock areas and visibility restored");
    check(docksnap::restore(window, changed), "restore alternate layout");
    check(window.dockWidgetArea(scenes) == Qt::TopDockWidgetArea && mixer->isHidden(), "alternate dock state restored");
    auto invalid = saved;
    invalid.state = "broken Qt state";
    const auto before = window.saveState();
    const auto beforeRect = window.geometry();
    check(!docksnap::restore(window, invalid), "invalid Qt state rejected");
    if (window.saveState() != before)
        std::cerr << "Rollback state: " << before.toHex().constData() << " -> "
                  << window.saveState().toHex().constData() << '\n';
    check(window.saveState() == before, "failed restore rolls back changes");
    check(window.geometry() == beforeRect, "failed restore preserves the existing window rectangle");
    invalid = saved;
    invalid.geometry = "broken geometry";
    check(!docksnap::restore(window, invalid), "invalid geometry rejected");
    check(window.saveState() == before, "invalid geometry keeps dock state");
    invalid = saved;
    invalid.docks.append("uninstalled-plugin");
    check(docksnap::missingDocks(window, invalid) == QStringList{"uninstalled-plugin"}, "missing dock detection");
    invalid = saved;
    invalid.state = QByteArray(2 * 1024 * 1024, 'x');
    check(docksnap::write(path, invalid) == docksnap::FileError::TooLarge, "oversized write rejected");
    check(docksnap::read(path, saved) == docksnap::FileError::None && saved.state == original.state,
          "failed write preserves previous snapshot");
    check(docksnap::write(temp.path(), original) == docksnap::FileError::Open, "unwritable destination reported");
    const auto badPath = temp.filePath("broken.json");
    QFile bad(badPath);
    check(bad.open(QIODevice::WriteOnly), "create malformed snapshot");
    bad.write("{invalid}");
    bad.close();
    check(docksnap::checkName(temp.path(), "BROKEN") == docksnap::NameError::Exists,
          "unreadable snapshot filename is still reserved");
    check(docksnap::read(badPath, saved) == docksnap::FileError::Invalid, "malformed JSON rejected");
    check(saved.state == original.state, "failed read does not modify output");
    check(bad.open(QIODevice::WriteOnly | QIODevice::Truncate), "create oversized input");
    bad.write(QByteArray(2 * 1024 * 1024 + 1, 'x'));
    bad.close();
    check(docksnap::read(badPath, saved) == docksnap::FileError::TooLarge, "oversized input rejected");
    // Deliver deferred deletions between batches, as in a normal event loop.
    app.processEvents();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    const auto baseline = residentMemory();
    auto peak = baseline;
    auto midpoint = baseline;
    const int rounds = stress ? 5000 : 500;
    std::cout << "Starting " << rounds * 2 << " restore operations\n";
    for (int i = 0; i < rounds; ++i) {
        check(docksnap::restore(window, original), "stress restore A");
        check(window.dockWidgetArea(scenes) == Qt::LeftDockWidgetArea && !mixer->isHidden(), "stress A state");
        check(docksnap::restore(window, changed), "stress restore B");
        check(window.dockWidgetArea(scenes) == Qt::TopDockWidgetArea && mixer->isHidden(), "stress B state");
        if (stress && i % 10 == 0) {
            check(docksnap::write(path, changed) == docksnap::FileError::None, "stress atomic save");
            docksnap::Snapshot loaded;
            check(docksnap::read(path, loaded) == docksnap::FileError::None && loaded.state == changed.state,
                  "stress saved layout integrity");
        }
        if (i % 100 == 0) {
            peak = qMax(peak, residentMemory());
            app.processEvents();
            QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
            peak = qMax(peak, residentMemory());
        }
        if (i == rounds / 2)
            midpoint = residentMemory();
        if (stress && i % 500 == 0)
            std::cout << "Stress progress: " << i * 2 << " restores\n";
    }
    app.processEvents();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    const auto retained = residentMemory();
    sources->setFloating(true);
    app.processEvents();
    const auto floating = docksnap::capture(window, "floating");
    sources->setFloating(false);
    check(docksnap::restore(window, floating) && sources->isFloating(), "floating state restored");
    check(docksnap::restore(window, original) && !sources->isFloating(), "floating dock returned to main window");
    check(baseline > 0 && peak - baseline < 64 * 1024 * 1024, "memory growth below 64 MiB");
    check(!stress || retained <= midpoint + 8 * 1024 * 1024, "second-half memory growth below 8 MiB");
    std::cout << "PASS: snapshots, atomic failure, Unicode, geometry, missing docks, floating docks; "
              << rounds * 2 << " restores; peak memory growth " << (peak - baseline)
              << " bytes; retained growth " << (retained > baseline ? retained - baseline : 0)
              << " bytes; second-half growth " << (retained > midpoint ? retained - midpoint : 0) << " bytes\n";
    return 0;
}
