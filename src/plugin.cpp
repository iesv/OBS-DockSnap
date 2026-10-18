// SPDX-License-Identifier: GPL-2.0-or-later
#include "layouts.hpp"
#include <obs-module.h>
#include <obs-frontend-api.h>
#include <QAbstractItemModel>
#include <QAbstractItemView>
#include <QAction>
#include <QApplication>
#include <QDesktopServices>
#include <QDialog>
#include <QDir>
#include <QEvent>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFontMetrics>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeyEvent>
#include <QKeySequence>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMainWindow>
#include <QMap>
#include <QMessageBox>
#include <QPainter>
#include <QPointer>
#include <QPushButton>
#include <QSaveFile>
#include <QScopedValueRollback>
#include <QScrollBar>
#include <QSet>
#include <QShortcut>
#include <QSizePolicy>
#include <QStyle>
#include <QStyledItemDelegate>
#include <QStyleOptionViewItem>
#include <QThread>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("obs-docksnap", "en-US")
OBS_MODULE_AUTHOR("haliluya.net")
MODULE_EXPORT const char *obs_module_name(void) { return "OBS-DockSnap"; }
MODULE_EXPORT const char *obs_module_description(void) { return "Save and restore OBS dock layout snapshots."; }

namespace {
QString text(const char *key) { return QString::fromUtf8(obs_module_text(key)); }
constexpr int modifiedRole = Qt::UserRole + 1;

class SnapshotDelegate final : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;

    void paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const override
    {
        auto item = option;
        initStyleOption(&item, index);
        const QFontMetrics fontMetrics(item.font);
        const auto *style = item.widget ? item.widget->style() : QApplication::style();
        auto textRect = style->subElementRect(QStyle::SE_ItemViewItemText, &item, item.widget);
        if (const auto *view = qobject_cast<const QAbstractItemView *>(item.widget))
            textRect.setRight(qMin(textRect.right(), view->viewport()->rect().right() - 2));
        const auto number = QStringLiteral("%1.").arg(index.row() + 1);
        auto numberFont = item.font;
        numberFont.setBold(true);
        const QFontMetrics numberMetrics(numberFont);
        const int numberWidth = numberMetrics.horizontalAdvance(QStringLiteral("%1.").arg(docksnap::maxSnapshots));
        const auto modified = index.data(modifiedRole).toString();
        const int dateWidth = qMax(0, qMin(fontMetrics.horizontalAdvance(modified) + 2, textRect.width() / 2));
        const int dateGap = modified.isEmpty() ? 0 : 12;
        const auto nameRect = textRect.adjusted(numberWidth + numberMetrics.horizontalAdvance(' '), 0,
                                               -dateWidth - dateGap, 0);
        const auto name = fontMetrics.elidedText(item.text, Qt::ElideRight, qMax(0, nameRect.width()));
        item.text.clear();
        style->drawControl(QStyle::CE_ItemViewItem, &item, painter, item.widget);
        painter->save();
        painter->setClipRect(textRect, Qt::IntersectClip);
        const bool selected = item.state.testFlag(QStyle::State_Selected);
        const auto group = !item.state.testFlag(QStyle::State_Enabled) ? QPalette::Disabled :
                           item.state.testFlag(QStyle::State_Active) ? QPalette::Active : QPalette::Inactive;
        const auto detailColor = item.palette.color(group, selected ? QPalette::HighlightedText : QPalette::PlaceholderText);
        painter->setPen(detailColor);
        painter->setFont(numberFont);
        auto numberRect = textRect;
        numberRect.setWidth(numberWidth);
        painter->drawText(numberRect, Qt::AlignRight | Qt::AlignVCenter, number);
        painter->setFont(item.font);
        painter->setPen(item.palette.color(group, selected ? QPalette::HighlightedText : QPalette::Text));
        painter->drawText(nameRect, Qt::AlignLeft | Qt::AlignVCenter, name);
        painter->setPen(detailColor);
        auto dateRect = textRect;
        dateRect.setLeft(dateRect.right() - dateWidth + 1);
        painter->drawText(dateRect, Qt::AlignRight | Qt::AlignVCenter,
                          fontMetrics.elidedText(modified, Qt::ElideRight, dateWidth));
        painter->restore();
    }
};

QJsonArray saveBindings(obs_hotkey_id id)
{
    obs_data_t *data = obs_data_create();
    obs_data_array_t *bindings = obs_hotkey_save(id);
    obs_data_set_array(data, "bindings", bindings);
    const auto result = QJsonDocument::fromJson(obs_data_get_json(data)).object().value("bindings").toArray();
    obs_data_array_release(bindings);
    obs_data_release(data);
    return result;
}

void loadBindings(obs_hotkey_id id, const QJsonArray &bindings)
{
    const auto json = QJsonDocument(QJsonObject{{"bindings", bindings}}).toJson(QJsonDocument::Compact);
    obs_data_t *data = obs_data_create_from_json(json.constData());
    obs_data_array_t *array = obs_data_get_array(data, "bindings");
    if (array)
        obs_hotkey_load(id, array);
    obs_data_array_release(array);
    obs_data_release(data);
}

class DockSnap final : public QObject {
public:
    explicit DockSnap(QMainWindow *mainWindow) : QObject(mainWindow), window(mainWindow)
    {
        char *path = obs_module_config_path("snapshots");
        directory = QString::fromUtf8(path ? path : "");
        bfree(path);
        path = obs_module_config_path("settings.json");
        settingsPath = QString::fromUtf8(path ? path : "");
        bfree(path);
        QFile file(settingsPath);
        if (file.open(QIODevice::ReadOnly) && file.size() <= 4 * 1024 * 1024)
            settings = QJsonDocument::fromJson(file.readAll()).object();
        last = settings.value("last").toString();
        for (const auto &value : settings.value("snapshotOrder").toArray()) {
            if (value.isString())
                snapshotOrder.append(value.toString());
        }
        cycleHotkey = obs_hotkey_register_frontend("obs-docksnap.cycle", obs_module_text("CycleHotkey"),
                                                  hotkeyCallback, this);
        previousHotkey = obs_hotkey_register_frontend("obs-docksnap.previous", obs_module_text("PreviousHotkey"),
                                                     hotkeyCallback, this);
        openHotkey = obs_hotkey_register_frontend("obs-docksnap.open", obs_module_text("OpenHotkey"),
                                                 hotkeyCallback, this);
        loadBindings(cycleHotkey, settings.value("cycleHotkey").toArray());
        loadBindings(previousHotkey, settings.value("previousHotkey").toArray());
        loadBindings(openHotkey, settings.value("openHotkey").toArray());
        settingsSaveTimer.setSingleShot(true);
        connect(&settingsSaveTimer, &QTimer::timeout, this, [this] {
            if (ready && !stopping)
                saveSettings();
        });
        obs_frontend_add_event_callback(frontendCallback, this);
    }

    ~DockSnap() override { shutdown(); }

    void shutdown()
    {
        if (stopping)
            return;
        stopping = true;
        settingsSaveTimer.stop();
        if (ready) {
            signal_handler_disconnect(obs_get_signal_handler(), "hotkey_bindings_changed", bindingsChangedCallback, this);
            saveSettings();
        }
        obs_hotkey_unregister(cycleHotkey);
        obs_hotkey_unregister(previousHotkey);
        obs_hotkey_unregister(openHotkey);
        for (const auto id : snapshotHotkeys)
            obs_hotkey_unregister(id);
        snapshotHotkeys.clear();
        obs_frontend_remove_event_callback(frontendCallback, this);
        delete action;
        action = nullptr;
        delete dialog;
        dialog = nullptr;
    }

private:
    QPointer<QMainWindow> window;
    QPointer<QAction> action;
    QPointer<QDialog> dialog;
    QPointer<QListWidget> list;
    QPointer<QLineEdit> searchInput;
    QPointer<QLabel> searchStatus;
    QString directory, settingsPath, last;
    QStringList snapshotOrder;
    QJsonObject settings;
    obs_hotkey_id cycleHotkey = OBS_INVALID_HOTKEY_ID;
    obs_hotkey_id previousHotkey = OBS_INVALID_HOTKEY_ID;
    obs_hotkey_id openHotkey = OBS_INVALID_HOTKEY_ID;
    QMap<QString, obs_hotkey_id> snapshotHotkeys;
    QTimer settingsSaveTimer;
    bool ready = false, stopping = false, loading = false;
    QList<QPointer<QPushButton>> selectionButtons;

    static void frontendCallback(obs_frontend_event event, void *data)
    {
        auto *self = static_cast<DockSnap *>(data);
        if (self->stopping)
            return;
        if (event == OBS_FRONTEND_EVENT_FINISHED_LOADING && !self->ready) {
            if (!self->ensureDirectory())
                return;
            self->ready = true;
            self->action = static_cast<QAction *>(obs_frontend_add_tools_menu_qaction(obs_module_text("Title")));
            if (self->action)
                connect(self->action, &QAction::triggered, self, [self] { self->show(); });
            self->refresh();
            self->saveSettings();
            signal_handler_connect(obs_get_signal_handler(), "hotkey_bindings_changed", bindingsChangedCallback, self);
        } else if (event == OBS_FRONTEND_EVENT_EXIT) {
            self->shutdown();
        }
    }

    static void hotkeyCallback(void *data, obs_hotkey_id id, obs_hotkey_t *, bool pressed)
    {
        if (!pressed)
            return;
        auto *self = static_cast<DockSnap *>(data);
        // Queue hotkeys on the GUI thread; destruction cancels pending calls.
        QMetaObject::invokeMethod(self, [self, id] {
            if (!self->ready || self->stopping)
                return;
            if (id == self->openHotkey)
                self->show();
            else if (id == self->cycleHotkey)
                self->cycle(1);
            else if (id == self->previousHotkey)
                self->cycle(-1);
            else {
                const auto path = self->snapshotHotkeys.key(id);
                if (!path.isEmpty())
                    self->load(path);
            }
        }, Qt::QueuedConnection);
    }

    static void bindingsChangedCallback(void *data, calldata_t *params)
    {
        auto *key = static_cast<obs_hotkey_t *>(calldata_ptr(params, "key"));
        if (!key)
            return;
        auto *self = static_cast<DockSnap *>(data);
        const auto id = obs_hotkey_get_id(key);
        QMetaObject::invokeMethod(self, [self, id] {
            if (self->ready && !self->stopping &&
                (id == self->cycleHotkey || id == self->previousHotkey || id == self->openHotkey ||
                 !self->snapshotHotkeys.key(id).isEmpty()))
                self->settingsSaveTimer.start(100);
        }, Qt::QueuedConnection);
    }

    bool ensureDirectory()
    {
        if (!directory.isEmpty() && !settingsPath.isEmpty() && QDir().mkpath(directory))
            return true;
        error("DirectoryFailed");
        return false;
    }

    void saveSettings()
    {
        if (!ensureDirectory())
            return;
        QJsonObject object{{"cycleHotkey", saveBindings(cycleHotkey)},
                           {"previousHotkey", saveBindings(previousHotkey)},
                           {"openHotkey", saveBindings(openHotkey)}};
        QJsonObject snapshots;
        for (auto it = snapshotHotkeys.cbegin(); it != snapshotHotkeys.cend(); ++it)
            snapshots.insert(QFileInfo(it.key()).fileName(), saveBindings(it.value()));
        object.insert("snapshotHotkeys", snapshots);
        object.insert("last", last);
        object.insert("snapshotOrder", QJsonArray::fromStringList(snapshotOrder));
        settings = object;
        QSaveFile file(settingsPath);
        const auto bytes = QJsonDocument(object).toJson();
        if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit())
            blog(LOG_WARNING, "[obs-docksnap] Could not save settings: %s", file.errorString().toUtf8().constData());
    }

    void error(const char *key, const QString &detail = {})
    {
        blog(LOG_WARNING, "[obs-docksnap] %s: %s", key, detail.toUtf8().constData());
        if (!stopping && window) {
            QPointer<QMessageBox> box = new QMessageBox(QMessageBox::Warning, text("Title"),
                text(key) + (detail.isEmpty() ? QString() : "\n\n" + detail), QMessageBox::Ok,
                dialog ? static_cast<QWidget *>(dialog.data()) : window.data());
            box->setTextFormat(Qt::PlainText);
            box->exec();
            delete box;
        }
    }

    bool confirm(const QString &message, bool defaultYes = false)
    {
        QPointer<QMessageBox> box = new QMessageBox(QMessageBox::Question, text("Title"), message,
                                                   QMessageBox::NoButton, dialog);
        box->setTextFormat(Qt::PlainText);
        auto *yes = box->addButton(text("Continue"), QMessageBox::AcceptRole);
        auto *no = box->addButton(text("Cancel"), QMessageBox::RejectRole);
        box->setDefaultButton(defaultYes ? yes : no);
        box->exec();
        const bool accepted = box && !stopping && box->clickedButton() == yes;
        delete box;
        return accepted;
    }

    QString selected() const
    {
        return list && list->currentItem() ? list->currentItem()->data(Qt::UserRole).toString() : QString();
    }

    QString nameFor(const QString &initial, const QString &excludePath = {})
    {
        QPointer<QInputDialog> input = new QInputDialog(dialog);
        input->setWindowTitle(text("Title"));
        input->setLabelText(text("NamePrompt"));
        input->setTextValue(initial);
        input->setOkButtonText(text("Save"));
        input->setCancelButtonText(text("Cancel"));
        while (input && input->exec() == QDialog::Accepted) {
            if (!input || stopping || !window)
                break;
            const auto name = input->textValue().trimmed().normalized(QString::NormalizationForm_C);
            const auto result = docksnap::checkName(directory, name, excludePath);
            if (result == docksnap::NameError::None) {
                delete input;
                return name;
            }
            input->setTextValue(name);
            error(result == docksnap::NameError::Exists ? "DuplicateName" : "InvalidName");
        }
        delete input;
        return {};
    }

    void removeSnapshotSettings(const QString &path)
    {
        const auto found = snapshotHotkeys.find(path);
        if (found != snapshotHotkeys.end()) {
            obs_hotkey_unregister(found.value());
            snapshotHotkeys.erase(found);
        }
        auto saved = settings.value("snapshotHotkeys").toObject();
        saved.remove(QFileInfo(path).fileName());
        settings.insert("snapshotHotkeys", saved);
        snapshotOrder.removeAll(QFileInfo(path).fileName());
    }

    void moveSnapshotSettings(const QString &oldPath, const QString &path)
    {
        const auto found = snapshotHotkeys.find(oldPath);
        if (found != snapshotHotkeys.end()) {
            const auto id = found.value();
            snapshotHotkeys.erase(found);
            snapshotHotkeys.insert(path, id);
        }
        auto saved = settings.value("snapshotHotkeys").toObject();
        const auto bindings = saved.take(QFileInfo(oldPath).fileName());
        saved.insert(QFileInfo(path).fileName(), bindings);
        settings.insert("snapshotHotkeys", saved);
        const auto index = snapshotOrder.indexOf(QFileInfo(oldPath).fileName());
        if (index >= 0)
            snapshotOrder[index] = QFileInfo(path).fileName();
        if (last == oldPath)
            last = path;
    }

    void syncSnapshotHotkey(const QString &path, const QString &name)
    {
        struct Label {
            obs_hotkey_id id;
            QByteArray key, description;
        } label{OBS_INVALID_HOTKEY_ID, ("obs-docksnap.load." + QFileInfo(path).fileName()).toUtf8(),
                text("LoadHotkey").arg(name).toUtf8()};
        const auto found = snapshotHotkeys.constFind(path);
        if (found == snapshotHotkeys.cend()) {
            label.id = obs_hotkey_register_frontend(label.key.constData(), label.description.constData(),
                                                    hotkeyCallback, this);
            if (label.id == OBS_INVALID_HOTKEY_ID) {
                blog(LOG_WARNING, "[obs-docksnap] Could not register snapshot hotkey: %s", label.key.constData());
                return;
            }
            snapshotHotkeys.insert(path, label.id);
            loadBindings(label.id, settings.value("snapshotHotkeys").toObject()
                                      .value(QFileInfo(path).fileName()).toArray());
        } else {
            label.id = found.value();
            // The OBS setters require the hotkey lock while changing labels.
            obs_hotkey_update_atomic([](void *data) {
                const auto &value = *static_cast<Label *>(data);
                obs_hotkey_set_name(value.id, value.key.constData());
                obs_hotkey_set_description(value.id, value.description.constData());
            }, &label);
        }
    }

    void refresh(const QString &select = {})
    {
        const QString target = select.isEmpty() ? selected() : select;
        const bool keepPosition = list && select.isEmpty();
        const int selectedRow = list ? list->currentRow() : -1;
        const int scroll = list ? list->verticalScrollBar()->value() : 0;
        const auto *top = list ? list->itemAt(QPoint(1, 1)) : nullptr;
        const auto topPath = top ? top->data(Qt::UserRole).toString() : QString();
        const int topOffset = top ? list->visualItemRect(top).top() : 0;
        QListWidgetItem *anchor = nullptr;
        if (list)
            list->clear();
        int invalid = 0, overflow = 0;
        const auto entries = docksnap::listSnapshots(directory, snapshotOrder, invalid, &overflow);
        snapshotOrder.clear();
        QSet<QString> present;
        for (const auto &entry : entries) {
            snapshotOrder.append(entry.fileName);
            const auto path = QDir(directory).absoluteFilePath(entry.fileName);
            present.insert(path);
            syncSnapshotHotkey(path, entry.name);
            if (!list)
                continue;
            auto *item = new QListWidgetItem(entry.name, list.data());
            item->setData(Qt::UserRole, path);
            item->setData(modifiedRole, entry.modified.toLocalTime().toString("yyyy-MM-dd HH:mm"));
            item->setToolTip(entry.name + "\n" +
                             text("Modified").arg(entry.modified.toLocalTime().toString("yyyy-MM-dd HH:mm:ss")) +
                             "\n" + path);
            if (path == topPath)
                anchor = item;
            if (path == target)
                list->setCurrentItem(item);
        }
        for (auto it = snapshotHotkeys.begin(); it != snapshotHotkeys.end();) {
            if (!present.contains(it.key())) {
                obs_hotkey_unregister(it.value());
                it = snapshotHotkeys.erase(it);
            } else {
                ++it;
            }
        }
        if (invalid)
            blog(LOG_WARNING, "[obs-docksnap] Skipped %d unreadable snapshot file(s)", invalid);
        if (overflow)
            blog(LOG_WARNING, "[obs-docksnap] %d snapshot file(s) exceed the %d-snapshot limit; files remain on disk",
                 overflow, docksnap::maxSnapshots);
        if (!list)
            return;
        if (list->currentRow() < 0 && list->count())
            list->setCurrentRow(qBound(0, selectedRow, list->count() - 1));
        if (keepPosition) {
            list->doItemsLayout();
            if (anchor) {
                list->scrollToItem(anchor, QAbstractItemView::PositionAtTop);
                if (list->verticalScrollMode() == QAbstractItemView::ScrollPerPixel)
                    list->verticalScrollBar()->setValue(list->verticalScrollBar()->value() - topOffset);
            } else {
                list->verticalScrollBar()->setValue(scroll);
            }
        } else if (list->currentItem()) {
            list->scrollToItem(list->currentItem());
        }
        if (searchInput && searchStatus) {
            const auto query = searchInput->text().trimmed().normalized(QString::NormalizationForm_C);
            searchStatus->setVisible(!query.isEmpty() && !searchMatch(query));
        }
        updateButtons();
    }

    QListWidgetItem *searchMatch(const QString &query) const
    {
        if (!list || query.isEmpty())
            return nullptr;
        if (query.startsWith('#')) {
            bool valid = false;
            const int number = query.mid(1).toInt(&valid);
            return valid && number > 0 && number <= list->count() ? list->item(number - 1) : nullptr;
        }
        for (int row = 0; row < list->count(); ++row) {
            auto *item = list->item(row);
            if (item->text().contains(query, Qt::CaseInsensitive))
                return item;
        }
        return nullptr;
    }

    void findSnapshot()
    {
        if (!list || !searchInput || !searchStatus || stopping || loading || !window)
            return;
        const auto query = searchInput->text().trimmed().normalized(QString::NormalizationForm_C);
        auto *match = searchMatch(query);
        searchStatus->setVisible(!query.isEmpty() && !match);
        if (!match)
            return;
        list->setCurrentItem(match);
        list->scrollToItem(match);
    }

    bool eventFilter(QObject *watched, QEvent *event) override
    {
        if (watched == searchInput && event->type() == QEvent::KeyPress) {
            const auto *key = static_cast<QKeyEvent *>(event);
            if (key->key() == Qt::Key_Return || key->key() == Qt::Key_Enter) {
                if (!key->isAutoRepeat() && list && !stopping && !loading) {
                    const auto query = searchInput->text().trimmed().normalized(QString::NormalizationForm_C);
                    auto *match = list->currentItem();
                    if (query.startsWith('#') || !match || !match->text().contains(query, Qt::CaseInsensitive))
                        match = searchMatch(query);
                    if (match)
                        load(match->data(Qt::UserRole).toString());
                }
                return true;
            }
        }
        return QObject::eventFilter(watched, event);
    }

    void updateButtons()
    {
        for (const auto &button : selectionButtons) {
            if (button)
                button->setEnabled(!selected().isEmpty());
        }
    }

    bool read(const QString &path, docksnap::Snapshot &snapshot)
    {
        if (docksnap::read(path, snapshot) == docksnap::FileError::None)
            return true;
        error("ReadFailed", path);
        return false;
    }

    bool write(const QString &path, const docksnap::Snapshot &snapshot)
    {
        if (docksnap::write(path, snapshot) != docksnap::FileError::None) {
            error("WriteFailed", path);
            return false;
        }
        return true;
    }

    QString saveSnapshot(const docksnap::Snapshot &snapshot, const QString &oldPath = {})
    {
        if (!ensureDirectory())
            return {};
        // Recheck after input/file dialogs, since files may have changed outside OBS.
        if ((oldPath.isEmpty() || !QFileInfo::exists(oldPath)) && !canAddSnapshot())
            return {};
        const auto result = docksnap::checkName(directory, snapshot.name, oldPath);
        if (result != docksnap::NameError::None) {
            error(result == docksnap::NameError::Exists ? "DuplicateName" : "InvalidName");
            return {};
        }
        const auto path = QDir(directory).filePath(snapshot.name + ".json");
        const bool moved = !oldPath.isEmpty() && oldPath != path;
        if (moved && !QFile::rename(oldPath, path)) {
            error("RenameFailed", oldPath);
            return {};
        }
        if (!write(path, snapshot)) {
            if (moved && !QFile::rename(path, oldPath)) {
                error("RenameFailed", path);
                moveSnapshotSettings(oldPath, path);
                refresh(path);
                saveSettings();
            }
            return {};
        }
        if (oldPath.isEmpty()) {
            // A new or imported snapshot always starts without a binding.
            removeSnapshotSettings(path);
        } else if (moved) {
            moveSnapshotSettings(oldPath, path);
        }
        refresh(path);
        return path;
    }

    bool canAddSnapshot()
    {
        int invalid = 0;
        if (docksnap::listSnapshots(directory, snapshotOrder, invalid).size() < docksnap::maxSnapshots)
            return true;
        error("SnapshotLimit");
        return false;
    }

    void saveNew()
    {
        if (stopping || loading || !window)
            return;
        QScopedValueRollback<bool> guard(loading, true);
        if (!canAddSnapshot())
            return;
        const auto name = nameFor({});
        if (name.isEmpty())
            return;
        const auto path = saveSnapshot(docksnap::capture(*window, name));
        if (!path.isEmpty()) {
            last = path;
            saveSettings();
        }
    }

    void overwrite()
    {
        if (stopping || loading || !window)
            return;
        QScopedValueRollback<bool> guard(loading, true);
        const auto path = selected();
        docksnap::Snapshot snapshot;
        if (path.isEmpty() || !read(path, snapshot) || !confirm(text("ConfirmOverwrite").arg(snapshot.name)))
            return;
        const auto savedPath = saveSnapshot(docksnap::capture(*window, snapshot.name), path);
        if (!savedPath.isEmpty()) {
            last = savedPath;
            saveSettings();
        }
    }

    void load(const QString &path)
    {
        if (path.isEmpty() || !window || stopping || loading)
            return;
        QScopedValueRollback<bool> guard(loading, true);
        docksnap::Snapshot snapshot;
        if (!read(path, snapshot))
            return;
        const auto missing = docksnap::missingDocks(*window, snapshot);
        if (!missing.isEmpty() && !confirm(text("MissingDocks").arg(missing.join(", "))))
            return;
        if (!docksnap::restore(*window, snapshot)) {
            error("RestoreFailed");
            return;
        }
        last = path;
        settingsSaveTimer.start(100);
        if (list) {
            for (int row = 0; row < list->count(); ++row) {
                if (list->item(row)->data(Qt::UserRole).toString() == path) {
                    list->setCurrentRow(row);
                    break;
                }
            }
        }
    }

    void cycle(int direction)
    {
        if (stopping || loading || !window || snapshotOrder.isEmpty())
            return;
        const auto current = snapshotOrder.indexOf(QFileInfo(last).fileName());
        const auto next = current < 0 ? (direction < 0 ? snapshotOrder.size() - 1 : 0)
                                      : (current + direction + snapshotOrder.size()) % snapshotOrder.size();
        load(QDir(directory).absoluteFilePath(snapshotOrder[next]));
    }

    void rename()
    {
        if (stopping || loading || !window)
            return;
        QScopedValueRollback<bool> guard(loading, true);
        const auto path = selected();
        docksnap::Snapshot snapshot;
        if (path.isEmpty() || !read(path, snapshot))
            return;
        const auto name = nameFor(snapshot.name, path);
        if (!name.isEmpty()) {
            snapshot.name = name;
            if (!saveSnapshot(snapshot, path).isEmpty())
                saveSettings();
        }
    }

    void remove()
    {
        if (stopping || loading || !window)
            return;
        QScopedValueRollback<bool> guard(loading, true);
        const auto path = selected();
        if (path.isEmpty() || !confirm(text("ConfirmDelete").arg(list->currentItem()->text()), true))
            return;
        if (!QFile::remove(path)) {
            error("DeleteFailed", path);
            return;
        }
        removeSnapshotSettings(path);
        if (last == path)
            last.clear();
        refresh();
        saveSettings();
    }

    void importFile()
    {
        if (stopping || loading || !window)
            return;
        QScopedValueRollback<bool> guard(loading, true);
        if (!canAddSnapshot())
            return;
        const auto source = filePath(QFileDialog::AcceptOpen);
        docksnap::Snapshot snapshot;
        if (source.isEmpty() || !read(source, snapshot))
            return;
        if (!saveSnapshot(snapshot).isEmpty())
            saveSettings();
    }

    void exportFile()
    {
        if (stopping || loading || !window)
            return;
        QScopedValueRollback<bool> guard(loading, true);
        docksnap::Snapshot snapshot;
        const auto source = selected();
        if (source.isEmpty() || !read(source, snapshot))
            return;
        const auto path = filePath(QFileDialog::AcceptSave, snapshot.name + ".json");
        if (!path.isEmpty())
            write(path, snapshot);
    }

    QString filePath(QFileDialog::AcceptMode mode, const QString &name = {})
    {
        QPointer<QFileDialog> file = new QFileDialog(dialog, text(mode == QFileDialog::AcceptSave ? "Export" : "Import"),
                                                     {}, text("FileFilter"));
        file->setAcceptMode(mode);
        file->setFileMode(mode == QFileDialog::AcceptSave ? QFileDialog::AnyFile : QFileDialog::ExistingFile);
        file->setDefaultSuffix("json");
        if (!name.isEmpty())
            file->selectFile(name);
        QString path;
        if (file->exec() == QDialog::Accepted && file && !stopping && window)
            path = file->selectedFiles().value(0);
        delete file;
        return path;
    }

    void show()
    {
        if (!ready || stopping || loading || !window || !ensureDirectory())
            return;
        if (window->isMinimized())
            window->setWindowState(window->windowState() & ~Qt::WindowMinimized);
        if (!dialog) {
            selectionButtons.clear();
            dialog = new QDialog(window);
            dialog->setWindowTitle(text("Title"));
            dialog->resize(630, 400);
            auto *outer = new QVBoxLayout(dialog);
            auto *help = new QLabel(text("Help"), dialog);
            help->setWordWrap(true);
            outer->addWidget(help);
            auto *row = new QGridLayout;
            row->setSpacing(outer->spacing());
            row->setColumnStretch(0, 1);
            auto *searchRow = new QHBoxLayout;
            searchInput = new QLineEdit(dialog);
            searchInput->setAttribute(Qt::WA_LayoutUsesWidgetRect);
            searchInput->setPlaceholderText(text("SearchPrompt"));
            searchInput->setAccessibleName(text("Search"));
            searchInput->setClearButtonEnabled(true);
            searchInput->installEventFilter(this);
            searchStatus = new QLabel(text("SearchNotFound"), dialog);
            searchStatus->hide();
            searchRow->addWidget(searchInput, 1);
            searchRow->addWidget(searchStatus);
            row->addLayout(searchRow, 0, 0);
            list = new QListWidget(dialog);
            list->setAttribute(Qt::WA_LayoutUsesWidgetRect);
            list->setItemDelegate(new SnapshotDelegate(list));
            list->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Ignored);
            list->setUniformItemSizes(true);
            list->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
            list->setDragDropMode(QAbstractItemView::InternalMove);
            list->setDefaultDropAction(Qt::MoveAction);
            list->setContextMenuPolicy(Qt::ActionsContextMenu);
            auto *search = new QAction(text("Search"), dialog);
            search->setShortcut(QKeySequence(QKeySequence::Find));
            search->setShortcutContext(Qt::WindowShortcut);
            search->setAutoRepeat(false);
            connect(search, &QAction::triggered, this, [this] {
                if (!stopping && !loading && searchInput) {
                    searchInput->setFocus(Qt::ShortcutFocusReason);
                    searchInput->selectAll();
                }
            });
            dialog->addAction(search);
            connect(searchInput, &QLineEdit::textChanged, this, [this] { findSnapshot(); });
            auto *openFolder = new QAction(text("OpenFolder"), list);
            connect(openFolder, &QAction::triggered, this, [this] {
                if (ensureDirectory() && !QDesktopServices::openUrl(QUrl::fromLocalFile(directory)))
                    error("OpenFolderFailed", directory);
            });
            list->addAction(openFolder);
            const auto shortcut = [&](const QList<QKeySequence> &keys, auto callback) {
                auto *key = new QShortcut(list);
                key->setKeys(keys);
                key->setContext(Qt::WidgetShortcut);
                key->setAutoRepeat(false);
                connect(key, &QShortcut::activated, this, callback);
            };
            shortcut({QKeySequence(Qt::Key_Return), QKeySequence(Qt::Key_Enter)}, [this] { load(selected()); });
            shortcut({QKeySequence(Qt::Key_F2)}, [this] { rename(); });
            shortcut({QKeySequence(Qt::Key_Delete)}, [this] { remove(); });
            int buttonRow = 0;
            const auto add = [&](const char *key, auto callback, bool selection = false) {
                auto *button = new QPushButton(text(key), dialog);
                button->setAttribute(Qt::WA_LayoutUsesWidgetRect);
                connect(button, &QPushButton::clicked, this, callback);
                row->addWidget(button, buttonRow++, 1);
                if (selection)
                    selectionButtons.append(button);
                return button;
            };
            auto *saveNewButton = add("SaveNew", [this] { saveNew(); });
            add("Overwrite", [this] { overwrite(); }, true);
            add("Load", [this] { load(selected()); }, true);
            add("Rename", [this] { rename(); }, true);
            add("Delete", [this] { remove(); }, true);
            add("Import", [this] { importFile(); });
            add("Export", [this] { exportFile(); }, true);
            add("Refresh", [this] { refresh(); saveSettings(); });
            const int controlHeight = qMax(searchInput->sizeHint().height(), saveNewButton->sizeHint().height());
            searchInput->setFixedHeight(controlHeight);
            for (int index = 0; index < buttonRow; ++index)
                row->itemAtPosition(index, 1)->widget()->setFixedHeight(controlHeight);
            row->addWidget(list, 1, 0, buttonRow - 1, 1);
            outer->addLayout(row);
            outer->addStretch();
            auto *info = new QLabel(
                QStringLiteral("OBS-DockSnap %1 · %2 <a href=\"https://haliluya.net/\">haliluya.net</a>")
                    .arg(QString::fromLatin1(PLUGIN_VERSION), text("Author").toHtmlEscaped()), dialog);
            info->setTextFormat(Qt::RichText);
            info->setTextInteractionFlags(Qt::TextBrowserInteraction);
            info->setOpenExternalLinks(true);
            info->setWordWrap(true);
            outer->addWidget(info);
            connect(list, &QListWidget::itemSelectionChanged, this, [this] { updateButtons(); });
            connect(list, &QListWidget::itemDoubleClicked, this, [this] { load(selected()); });
            connect(list->model(), &QAbstractItemModel::rowsMoved, this, [this] {
                snapshotOrder.clear();
                for (int row = 0; row < list->count(); ++row)
                    snapshotOrder.append(QFileInfo(list->item(row)->data(Qt::UserRole).toString()).fileName());
                saveSettings();
            });
        }
        refresh(last);
        searchInput->clear();
        dialog->show();
        dialog->raise();
        dialog->activateWindow();
    }
};
QPointer<DockSnap> instance;
} // namespace

bool obs_module_load(void)
{
    auto *window = static_cast<QMainWindow *>(obs_frontend_get_main_window());
    if (instance || !window || QThread::currentThread() != window->thread())
        return false;
    instance = new DockSnap(window);
    blog(LOG_INFO, "[obs-docksnap] Loaded version %s (OBS 32.0.4)", PLUGIN_VERSION);
    return true;
}

void obs_module_unload(void)
{
    delete instance;
    instance = nullptr;
}
