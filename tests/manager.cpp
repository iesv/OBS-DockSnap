// SPDX-License-Identifier: GPL-2.0-or-later
#include <obs-module.h>
#include <obs-frontend-api.h>
#include <QApplication>
#include <QDockWidget>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QScrollBar>
#include <QTemporaryDir>
#include <cstdlib>
#include <iostream>
#include <thread>
#include <QAction>
#include <QDir>
#include <QMainWindow>

// Supply the frontend window and event hooks; libobs and Qt run unchanged.
namespace host {
QMainWindow *window = nullptr;
QAction *action = nullptr;
obs_frontend_event_cb callback = nullptr;
void *callbackData = nullptr;
QString config;
void *mainWindow() { return window; }
void *toolsAction(const char *name) { return action = new QAction(QString::fromUtf8(name), window); }
void addEvent(obs_frontend_event_cb function, void *data) { callback = function; callbackData = data; }
void removeEvent(obs_frontend_event_cb, void *) { callback = nullptr; callbackData = nullptr; }
char *configPath(obs_module_t *, const char *file) { return bstrdup(QDir(config).filePath(file).toUtf8().constData()); }
}
#define obs_frontend_get_main_window host::mainWindow
#define obs_frontend_add_tools_menu_qaction host::toolsAction
#define obs_frontend_add_event_callback host::addEvent
#define obs_frontend_remove_event_callback host::removeEvent
#define obs_module_get_config_path host::configPath
#include "../src/plugin.cpp"

static void check(bool condition, const char *message)
{
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

static void pump(int milliseconds = 0)
{
    if (milliseconds) {
        QEventLoop loop;
        QTimer::singleShot(milliseconds, &loop, &QEventLoop::quit);
        loop.exec();
    }
    QCoreApplication::processEvents();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
}

static QMap<QString, obs_hotkey_id> hotkeys()
{
    QMap<QString, obs_hotkey_id> result;
    obs_enum_hotkeys([](void *data, obs_hotkey_id id, obs_hotkey_t *key) {
        static_cast<QMap<QString, obs_hotkey_id> *>(data)->insert(QString::fromUtf8(obs_hotkey_get_name(key)), id);
        return true;
    }, &result);
    return result;
}

static QJsonObject settings()
{
    QFile file(QDir(host::config).filePath("settings.json"));
    check(file.open(QIODevice::ReadOnly), "read manager settings");
    return QJsonDocument::fromJson(file.readAll()).object();
}

static QPushButton *button(QDialog *dialog, const char *key)
{
    for (auto *widget : dialog->findChildren<QPushButton *>()) {
        if (widget->text() == text(key))
            return widget;
    }
    check(false, "manager button exists");
    return nullptr;
}

static QShortcut *localShortcut(QListWidget *list, Qt::Key key)
{
    for (auto *shortcut : list->findChildren<QShortcut *>()) {
        if (shortcut->keys().contains(QKeySequence(key))) {
            check(shortcut->context() == Qt::WidgetShortcut && !shortcut->autoRepeat(),
                  "list shortcut requires focus and does not repeat");
            return shortcut;
        }
    }
    check(false, "local shortcut exists");
    return nullptr;
}

static void acceptConfirmation()
{
    auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
    check(box != nullptr, "confirmation opens");
    for (auto *choice : box->buttons()) {
        if (box->buttonRole(choice) == QMessageBox::AcceptRole) {
            choice->click();
            return;
        }
    }
    check(false, "confirmation has accept button");
}

static void chooseFile(const QString &path)
{
    auto *file = qobject_cast<QFileDialog *>(QApplication::activeModalWidget());
    check(file != nullptr, "file selector opens");
    // Type the path; selectFile() preserves a focused filename field.
    auto *input = file->findChild<QLineEdit *>(QStringLiteral("fileNameEdit"));
    check(input != nullptr, "file selector has a filename field");
    input->setText(QDir::toNativeSeparators(path));
    static_cast<QDialog *>(file)->accept();
}

static bool sameSnapshot(const docksnap::Snapshot &a, const docksnap::Snapshot &b)
{
    return a.name == b.name && a.state == b.state && a.geometry == b.geometry && a.docks == b.docks;
}

int main(int argc, char **argv)
{
    std::cout << std::unitbuf;
    QCoreApplication::setAttribute(Qt::AA_DontUseNativeDialogs);
    QApplication app(argc, argv);
    check(QFontMetrics(app.font()).inFont(QChar('0')), "test platform provides readable numeric glyphs");
    QTemporaryDir temp;
    check(temp.isValid(), "manager temporary directory");
    host::config = temp.path();
    check(!obs_module_load(), "null frontend window is rejected");
    check(obs_startup("en-US", temp.path().toUtf8().constData(), nullptr), "initialize official libobs");
    obs_hotkey_enable_callback_rerouting(true);
    QMainWindow window;
    host::window = &window;
    window.resize(960, 640);
    auto *dock = new QDockWidget("Test", &window);
    dock->setObjectName("test-dock");
    dock->setWidget(new QLabel("Test", dock));
    window.addDockWidget(Qt::LeftDockWidgetArea, dock);
    window.show();
    pump();
    bool wrongThreadLoaded = true;
    std::thread worker([&] { wrongThreadLoaded = obs_module_load(); });
    worker.join();
    check(!wrongThreadLoaded, "non-GUI-thread load is rejected");
    const QDir folder(QDir(temp.path()).filePath("snapshots"));
    check(QDir().mkpath(folder.path()), "manager snapshot directory");
    for (int i = 0; i < docksnap::maxSnapshots; ++i) {
        const auto name = QString("Snapshot %1").arg(i, 4, 10, QLatin1Char('0'));
        check(docksnap::write(folder.filePath(name + ".json"), docksnap::capture(window, name)) ==
                  docksnap::FileError::None, "create 1000 manager snapshots");
    }
    const auto next = [&] {
        obs_hotkey_trigger_routed_callback(hotkeys().value("obs-docksnap.cycle", OBS_INVALID_HOTKEY_ID), true);
        pump();
    };
    for (const auto *locale : {"en-US", "zh-CN", "zh-TW"}) {
        obs_module_lookup = text_lookup_create((QString(TEST_SOURCE_DIR) + "/data/locale/" + locale + ".ini").toUtf8().constData());
        check(obs_module_lookup != nullptr, "load manager locale");
        check(obs_module_load() && host::callback, "load manager with frontend hooks");
        check(!obs_module_load(), "duplicate module load is rejected");
        QElapsedTimer timer;
        timer.start();
        host::callback(OBS_FRONTEND_EVENT_FINISHED_LOADING, host::callbackData);
        const auto startupMs = timer.elapsed();
        auto keys = hotkeys();
        check(keys.size() == docksnap::maxSnapshots + 3, "1000 snapshot hotkeys plus three global hotkeys");
        for (auto id : keys)
            check(saveBindings(id).isEmpty(), "hotkeys start unassigned");
        host::action->trigger();
        pump();
        QPointer<QDialog> dialog = window.findChild<QDialog *>();
        check(dialog && dialog->windowTitle() == text("Title"), "localized manager opens");
        const auto expectedTitle = QByteArray(locale) == "en-US" ? QStringLiteral("DockSnap") :
            QByteArray(locale) == "zh-CN" ? QString::fromUtf8("布局快照") : QString::fromUtf8("佈局快照");
        check(host::action->text() == expectedTitle && dialog->windowTitle() == expectedTitle,
              "menu and window use the specified locale name");
        auto *list = dialog->findChild<QListWidget *>();
        check(list && list->count() == docksnap::maxSnapshots && list->verticalScrollBar()->maximum() > 0,
              "1000-item list scrolls");
        check(dialog->findChildren<QPushButton *>().size() == 8, "eight manager buttons");
        auto *search = dialog->findChild<QLineEdit *>();
        check(search != nullptr && search->isClearButtonEnabled(), "top search field has a clear button");
        check(search->placeholderText() == text("SearchPrompt") && search->placeholderText().contains("#12"),
              "localized placeholder explains numbered search");
        auto *first = button(dialog, "SaveNew");
        auto *second = button(dialog, "Overwrite");
        auto *last = button(dialog, "Refresh");
        check(search->height() == first->height() &&
                  search->mapTo(dialog, QPoint()).y() == first->mapTo(dialog, QPoint()).y() &&
                  list->mapTo(dialog, QPoint()).y() == second->mapTo(dialog, QPoint()).y() &&
                  list->mapTo(dialog, QPoint(0, list->height())).y() ==
                      last->mapTo(dialog, QPoint(0, last->height())).y(), "search, list and buttons align on the grid");
        const int controlSpacing = second->y() - first->y() - first->height();
        check(list->y() - search->y() - search->height() == controlSpacing,
              "search-to-list spacing matches button spacing");
        check(first->x() - list->x() - list->width() == controlSpacing, "horizontal and vertical gaps match");
        int controlRow = 0;
        for (const auto *key : {"SaveNew", "Overwrite", "Load", "Rename", "Delete", "Import", "Export", "Refresh"}) {
            const auto *control = button(dialog, key);
            check(control->height() == first->height() &&
                      control->y() == first->y() + controlRow++ * (first->height() + controlSpacing),
                  "all buttons have uniform height and spacing");
        }
        const auto firstPath = list->item(0)->data(Qt::UserRole).toString();
        check(list->item(0)->data(modifiedRole).toString() ==
                  QFileInfo(firstPath).lastModified().toLocalTime().toString("yyyy-MM-dd HH:mm"),
              "rows cache the file modification time in local time");
        check(list->actions().size() == 1 && list->actions().front()->text() == text("OpenFolder"),
              "list context menu only opens the snapshot folder");
        QAction *findAction = nullptr;
        for (auto *action : dialog->actions()) {
            if (action->shortcut() == QKeySequence(QKeySequence::Find))
                findAction = action;
        }
        check(findAction && findAction->shortcutContext() == Qt::WindowShortcut,
              "standard Find shortcut belongs to the manager");
        const auto orderBeforeSearch = settings().value("snapshotOrder");
        search->setText("sNaPsHoT 0900");
        check(list->currentRow() == 900 && list->count() == docksnap::maxSnapshots,
              "search locates partial names without case sensitivity or filtering");
        search->setText("12");
        check(list->currentRow() == 12, "plain digits search names rather than row numbers");
        search->setText(" #12 ");
        check(list->currentRow() == 11, "numbered search uses one-based positions");
        search->setText("#1000");
        check(list->currentRow() == 999 && settings().value("last").toString().isEmpty(),
              "numbered search locates the final row without loading");
        findAction->trigger();
        check(search->selectedText() == search->text(), "Find selects the current search text");
        search->setText(" sNaPsHoT 099 ");
        check(list->currentRow() == 990, "typing finds the first matching row");
        QKeyEvent searchEnter(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
        QApplication::sendEvent(search, &searchEnter);
        pump(150);
        const auto partialPath = list->item(990)->data(Qt::UserRole).toString();
        check(list->currentRow() == 990 && settings().value("last").toString() == partialPath,
              "Enter loads a partial name match without moving to the next result");
        list->setCurrentRow(991);
        QKeyEvent repeatEnter(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier, {}, true);
        QApplication::sendEvent(search, &repeatEnter);
        pump(150);
        check(settings().value("last").toString() == partialPath, "held search Enter does not repeat loading");
        search->setText("#12");
        list->setCurrentRow(0);
        QApplication::sendEvent(search, &searchEnter);
        pump(150);
        check(list->currentRow() == 11 &&
                  settings().value("last").toString() == list->item(11)->data(Qt::UserRole).toString(),
              "Enter resolves the numbered position rather than a stale selection");
        search->setText("snapshot 099");
        list->setCurrentRow(999);
        QKeyEvent keypadEnter(QEvent::KeyPress, Qt::Key_Enter, Qt::NoModifier);
        QApplication::sendEvent(search, &keypadEnter);
        pump(150);
        const auto loadedPath = list->item(999)->data(Qt::UserRole).toString();
        check(list->currentRow() == 999 && settings().value("last").toString() == loadedPath,
              "keypad Enter loads the selected matching result");
        search->setText("no matching snapshot");
        check(list->currentRow() == 999 && list->count() == docksnap::maxSnapshots,
              "an unmatched query preserves the list and selection");
        list->setCurrentRow(123);
        for (const auto *query : {"no matching snapshot", "#0", "#1001", "#-1", "#99999999999999999999", "#"}) {
            search->setText(query);
            QApplication::sendEvent(search, &searchEnter);
        }
        pump(150);
        check(list->currentRow() == 123 && settings().value("last").toString() == loadedPath,
              "unmatched names and invalid positions never load a stale selection");
        search->setText("snapshot 099");
        search->selectAll();
        QKeyEvent editDelete(QEvent::KeyPress, Qt::Key_Delete, Qt::NoModifier);
        QApplication::sendEvent(search, &editDelete);
        check(search->text().isEmpty() && list->count() == docksnap::maxSnapshots &&
                  settings().value("snapshotOrder") == orderBeforeSearch,
              "Delete in the search field edits text and preserves snapshots and order");
        timer.restart();
        for (int row = 0; row < docksnap::maxSnapshots; ++row) {
            search->setText(QString("snapshot %1").arg(row, 4, 10, QLatin1Char('0')));
            check(list->currentRow() == row, "repeated search selects its target");
            if (row % 50 == 0)
                pump();
        }
        const auto searchMs = timer.elapsed();
        check(searchMs < 10000, "1000 searches stay within ten seconds");
        timer.restart();
        for (int row = 0; row < docksnap::maxSnapshots; ++row) {
            search->setText(QStringLiteral("#%1").arg(row + 1));
            check(list->currentRow() == row, "repeated numbered search selects its current position");
            if (row % 50 == 0)
                pump();
        }
        const auto numberSearchMs = timer.elapsed();
        check(numberSearchMs < 10000, "1000 numbered searches stay within ten seconds");
        pump();
        check(dialog->grab().save(QString("manager-1000-%1.png").arg(locale)), "save the final numbered row");
        search->clear();
        auto *loadKey = localShortcut(list, Qt::Key_Return);
        check(loadKey->keys().contains(QKeySequence(Qt::Key_Enter)), "keypad Enter also loads");
        check(QMetaObject::invokeMethod(loadKey, "activated", Qt::DirectConnection), "invoke local load action");
        pump(150);
        check(settings().value("last").toString() == list->currentItem()->data(Qt::UserRole).toString(),
              "local Enter loads the selected snapshot");
        list->setCurrentRow(0);
        list->scrollToItem(list->item(500), QAbstractItemView::PositionAtTop);
        pump();
        const auto *top = list->itemAt(QPoint(1, 1));
        check(top != nullptr, "scrolled list has a first visible row");
        const auto topPath = top->data(Qt::UserRole).toString();
        const int scroll = list->verticalScrollBar()->value();
        button(dialog, "Refresh")->click();
        pump();
        check(list->itemAt(QPoint(1, 1)) && list->itemAt(QPoint(1, 1))->data(Qt::UserRole).toString() == topPath &&
                  list->verticalScrollBar()->value() == scroll && list->currentRow() == 0,
              "refresh preserves viewport and selection independently");
        list->setCurrentRow(500);
        const auto deletedPath = list->currentItem()->data(Qt::UserRole).toString();
        docksnap::Snapshot deleted;
        check(docksnap::read(deletedPath, deleted) == docksnap::FileError::None, "read deletion fixture");
        const auto exportPath = temp.filePath("exported.json");
        QFile::remove(exportPath);
        QTimer::singleShot(0, [&] { chooseFile(temp.filePath("exported")); });
        button(dialog, "Export")->click();
        docksnap::Snapshot exported;
        check(docksnap::read(exportPath, exported) == docksnap::FileError::None && sameSnapshot(exported, deleted),
              "export adds the JSON suffix and preserves every snapshot field");
        auto *deleteKey = localShortcut(list, Qt::Key_Delete);
        QTimer::singleShot(0, [] {
            auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
            check(box != nullptr, "Delete requires confirmation");
            QKeyEvent escape(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
            QApplication::sendEvent(box, &escape);
        });
        QMetaObject::invokeMethod(deleteKey, "activated", Qt::DirectConnection);
        check(QFile::exists(deletedPath) && list->count() == docksnap::maxSnapshots,
              "cancelling Delete preserves the file");
        QTimer::singleShot(0, [] {
            auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
            check(box && box->defaultButton() && box->buttonRole(box->defaultButton()) == QMessageBox::AcceptRole,
                  "Delete defaults to confirmation");
            QKeyEvent enter(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
            QApplication::sendEvent(box, &enter);
        });
        QMetaObject::invokeMethod(deleteKey, "activated", Qt::DirectConnection);
        check(!QFile::exists(deletedPath) && list->count() == docksnap::maxSnapshots - 1 &&
                  list->currentRow() == 500 &&
                  !hotkeys().contains("obs-docksnap.load." + QFileInfo(deletedPath).fileName()),
              "confirmed Delete removes the file and binding, then selects a nearby row");
        const auto importPath = temp.filePath("import.json");
        check(docksnap::write(importPath, deleted) == docksnap::FileError::None, "prepare import fixture");
        QTimer::singleShot(0, [&] { chooseFile(importPath); });
        button(dialog, "Import")->click();
        check(QFile::exists(deletedPath) && list->count() == docksnap::maxSnapshots,
              "import restores the deleted snapshot without exceeding capacity");
        docksnap::Snapshot imported;
        check(docksnap::read(deletedPath, imported) == docksnap::FileError::None && sameSnapshot(imported, deleted),
              "import preserves every snapshot field");
        const auto transferPath = temp.filePath("round trip.json");
        const auto transferSource = list->item(list->count() - 1)->data(Qt::UserRole).toString();
        docksnap::Snapshot transferSnapshot;
        check(docksnap::read(transferSource, transferSnapshot) == docksnap::FileError::None, "read transfer fixture");
        const auto transferOrder = settings().value("snapshotOrder");
        constexpr int transferRounds = 25;
        timer.restart();
        for (int round = 0; round < transferRounds; ++round) {
            list->setCurrentRow(list->count() - 1);
            QFile::remove(transferPath);
            if (round == 0) {
                QTimer::singleShot(0, [] {
                    auto *file = qobject_cast<QFileDialog *>(QApplication::activeModalWidget());
                    check(file != nullptr, "export cancellation opens a file dialog");
                    file->reject();
                });
                button(dialog, "Export")->click();
                check(!QFile::exists(transferPath), "cancelled export creates no file");
            }
            QTimer::singleShot(0, [&] { chooseFile(transferPath); });
            button(dialog, "Export")->click();
            docksnap::Snapshot transferred;
            check(docksnap::read(transferPath, transferred) == docksnap::FileError::None &&
                      sameSnapshot(transferred, transferSnapshot), "repeated export preserves every snapshot field");
            check(QFile::remove(transferSource), "remove transfer fixture before import");
            button(dialog, "Refresh")->click();
            check(list->count() == docksnap::maxSnapshots - 1, "transfer fixture leaves exactly one slot");
            if (round == 0) {
                QTimer::singleShot(0, [] {
                    auto *file = qobject_cast<QFileDialog *>(QApplication::activeModalWidget());
                    check(file != nullptr, "import cancellation opens a file dialog");
                    file->reject();
                });
                button(dialog, "Import")->click();
                check(!QFile::exists(transferSource) && list->count() == docksnap::maxSnapshots - 1,
                      "cancelled import leaves snapshots unchanged");
            }
            QTimer::singleShot(0, [&] { chooseFile(transferPath); });
            button(dialog, "Import")->click();
            check(docksnap::read(transferSource, transferred) == docksnap::FileError::None &&
                      sameSnapshot(transferred, transferSnapshot), "repeated import preserves every snapshot field");
            check(list->count() == docksnap::maxSnapshots && settings().value("snapshotOrder") == transferOrder &&
                      hotkeys().size() == docksnap::maxSnapshots + 3 && dialog->findChildren<QFileDialog *>().isEmpty(),
                  "round trips keep capacity, order and hotkey count stable without retaining file dialogs");
            pump();
        }
        const auto transferMs = timer.elapsed();
        list->setCurrentRow(0);
        check(list->model()->moveRow({}, 2, {}, 0), "drag model moves a row");
        check(settings().value("snapshotOrder").toArray()[0].toString() == "Snapshot 0002.json",
              "drag order persists immediately");
        search->setText("#1");
        check(list->currentRow() == 0 && list->currentItem()->text() == "Snapshot 0002",
              "numbered search follows the current drag order");
        search->clear();
        const auto snapshotKey = QStringLiteral("obs-docksnap.load.Snapshot 0002.json");
        const auto id = keys.value(snapshotKey, OBS_INVALID_HOTKEY_ID);
        QJsonArray binding{QJsonObject{{"key", "OBS_KEY_F12"}, {"control", true}}};
        loadBindings(id, binding);
        pump(150);
        check(!settings().value("snapshotHotkeys").toObject().value("Snapshot 0002.json").toArray().isEmpty(),
              "native binding signal persists settings");
        obs_hotkey_trigger_routed_callback(id, true);
        pump();
        check(list->currentRow() == 0, "specific snapshot hotkey selects the target");
        next();
        check(list->currentRow() == 1, "next hotkey follows custom order");
        for (const auto *key : {"SaveNew", "Import"}) {
            QTimer::singleShot(0, [] {
                auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
                check(box && box->text() == text("SnapshotLimit"), "capacity prompt precedes input dialog");
                box->accept();
            });
            button(dialog, key)->click();
        }
        list->setCurrentRow(0);
        QTimer::singleShot(0, [] {
            auto *input = qobject_cast<QInputDialog *>(QApplication::activeModalWidget());
            check(input != nullptr, "rename input opens");
            input->setTextValue("Renamed");
            input->accept();
        });
        QMetaObject::invokeMethod(localShortcut(list, Qt::Key_F2), "activated", Qt::DirectConnection);
        check(list->currentRow() == 0 && list->currentItem()->text() == "Renamed" &&
                  hotkeys().value("obs-docksnap.load.Renamed.json", OBS_INVALID_HOTKEY_ID) == id &&
                  !saveBindings(id).isEmpty(), "rename preserves position and native binding");
        QTimer::singleShot(0, [] {
            auto *input = qobject_cast<QInputDialog *>(QApplication::activeModalWidget());
            check(input != nullptr, "case-only rename input opens");
            input->setTextValue("RENAMED");
            input->accept();
        });
        button(dialog, "Rename")->click();
        check(list->currentRow() == 0 && list->currentItem()->text() == "RENAMED" &&
                  hotkeys().value("obs-docksnap.load.RENAMED.json", OBS_INVALID_HOTKEY_ID) == id &&
                  !saveBindings(id).isEmpty(), "case-only rename preserves order and binding");
        QTimer::singleShot(0, acceptConfirmation);
        button(dialog, "Overwrite")->click();
        check(!saveBindings(id).isEmpty(), "overwrite preserves binding at capacity");
        const auto copy = folder.filePath("Extra.json");
        check(docksnap::write(copy, docksnap::capture(window, "Extra")) == docksnap::FileError::None,
              "external file exceeds capacity");
        button(dialog, "Refresh")->click();
        check(list->count() == docksnap::maxSnapshots && hotkeys().size() == docksnap::maxSnapshots + 3 &&
                  QFile::exists(copy), "refresh caps entries and hotkeys without deleting excess files");
        QFile::remove(copy);
        timer.restart();
        const auto cycleId = hotkeys().value("obs-docksnap.cycle", OBS_INVALID_HOTKEY_ID);
        for (int i = 0; i < 1000; ++i) {
            obs_hotkey_trigger_routed_callback(cycleId, true);
            pump();
        }
        const auto cycleMs = timer.elapsed();
        check(cycleMs < 60000 && hotkeys().size() == docksnap::maxSnapshots + 3,
              "1000 switches stay within 60 seconds with stable hotkey count");
        check(dialog->grab().save(QString("manager-%1.png").arg(locale)), "save native manager screenshot");
        // A removed file must not make unrelated snapshot hotkeys rescan the directory.
        check(QFile::remove(folder.filePath("Snapshot 0999.json")), "remove an unrelated file externally");
        obs_hotkey_trigger_routed_callback(id, true);
        pump();
        check(list->count() == docksnap::maxSnapshots, "loading only reads the target snapshot");
        button(dialog, "Refresh")->click();
        check(list->count() == docksnap::maxSnapshots - 1, "manual refresh detects file removal");
        // Destroy the view with a pending hotkey, then reopen and unload twice.
        obs_hotkey_trigger_routed_callback(id, true);
        delete dialog;
        pump();
        host::action->trigger();
        pump();
        check(window.findChild<QDialog *>() != nullptr, "manager recreates its destroyed view");
        obs_hotkey_trigger_routed_callback(cycleId, true);
        obs_module_unload();
        pump();
        obs_module_unload();
        check(hotkeys().isEmpty() && !host::callback, "unload clears native hotkeys and frontend callback");
        check(obs_module_load() && host::callback, "reload manager for exit test");
        host::callback(OBS_FRONTEND_EVENT_FINISHED_LOADING, host::callbackData);
        host::action->trigger();
        pump();
        auto *exitDialog = window.findChild<QDialog *>();
        check(exitDialog != nullptr, "exit test manager opens");
        QTimer::singleShot(0, [] {
            check(qobject_cast<QInputDialog *>(QApplication::activeModalWidget()) != nullptr,
                  "exit occurs during name input");
            host::callback(OBS_FRONTEND_EVENT_EXIT, host::callbackData);
        });
        button(exitDialog, "Rename")->click();
        check(hotkeys().isEmpty() && !host::callback, "exit closes modal input and clears callbacks");
        obs_module_unload();
        for (const auto *key : {"Import", "Export"}) {
            check(obs_module_load() && host::callback, "reload manager for file dialog exit test");
            host::callback(OBS_FRONTEND_EVENT_FINISHED_LOADING, host::callbackData);
            host::action->trigger();
            pump();
            exitDialog = window.findChild<QDialog *>();
            check(exitDialog != nullptr, "file dialog exit manager opens");
            if (QByteArray(key) == "Import") {
                check(QFile::remove(folder.filePath("Snapshot 0998.json")), "make room for import exit test");
                button(exitDialog, "Refresh")->click();
            }
            QTimer::singleShot(0, [] {
                check(qobject_cast<QFileDialog *>(QApplication::activeModalWidget()) != nullptr,
                      "exit occurs during file selection");
                host::callback(OBS_FRONTEND_EVENT_EXIT, host::callbackData);
            });
            button(exitDialog, key)->click();
            check(hotkeys().isEmpty() && !host::callback, "exit closes file dialog and clears callbacks");
            obs_module_unload();
        }
        obs_module_free_locale();
        // Restore fixture names for the next locale.
        auto restored = docksnap::capture(window, "Snapshot 0002");
        QFile::remove(folder.filePath("RENAMED.json"));
        check(docksnap::write(folder.filePath("Snapshot 0002.json"), restored) == docksnap::FileError::None,
              "restore renamed fixture");
        check(docksnap::write(folder.filePath("Snapshot 0999.json"),
                             docksnap::capture(window, "Snapshot 0999")) == docksnap::FileError::None,
              "restore removed fixture");
        check(docksnap::write(folder.filePath("Snapshot 0998.json"),
                             docksnap::capture(window, "Snapshot 0998")) == docksnap::FileError::None,
              "restore import capacity fixture");
        QFile::remove(QDir(temp.path()).filePath("settings.json"));
        std::cout << "PASS: manager " << locale << "; 1000 snapshots; startup " << startupMs
                  << " ms; 1000 name searches " << searchMs << " ms; 1000 numbered searches " << numberSearchMs
                  << " ms; " << transferRounds << " import/export round trips " << transferMs
                  << " ms; 1000 native hotkey switches " << cycleMs << " ms; file dialogs, lifecycle and bindings\n";
    }
    obs_shutdown();
    return 0;
}
