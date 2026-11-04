#include "ui.hpp"
#include "usage-guard.hpp"

#include <util/platform.h>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QSplitter>
#include <QStackedWidget>
#include <QStringList>
#include <QVBoxLayout>
#include <algorithm>

namespace dvc {

static std::wstring new_device_id()
{
	GUID id;
	wchar_t text[40];
	if (FAILED(CoCreateGuid(&id)) || !StringFromGUID2(id, text, 40)) {
		return {};
	}
	return text;
}

Dialog::Dialog(QWidget *parent) : QDialog(parent)
{
	setWindowTitle(obs_module_text("Title"));
	resize(1100, 640);
	auto *layout = new QVBoxLayout(this);
	auto *buttons = new QHBoxLayout;
	auto *add = new QPushButton(obs_module_text("AddDevice"), this);
	auto *rename = new QPushButton(obs_module_text("RenameDevice"), this);
	auto *remove = new QPushButton(obs_module_text("RemoveDevice"), this);
	buttons->addWidget(add);
	buttons->addWidget(rename);
	buttons->addWidget(remove);
	buttons->addStretch();
	layout->addLayout(buttons);
	auto *splitter = new QSplitter(this);
	devices = new QListWidget(splitter);
	panels = new QStackedWidget(splitter);
	splitter->addWidget(devices);
	splitter->addWidget(panels);
	splitter->setStretchFactor(0, 0);
	splitter->setStretchFactor(1, 1);
	splitter->setSizes({240, 800});
	layout->addWidget(splitter, 1);
	connect(devices, &QListWidget::currentRowChanged, panels, &QStackedWidget::setCurrentIndex);
	connect(add, &QPushButton::clicked, this, [this] {
		const std::wstring id = new_device_id();
		if (id.empty()) {
			QMessageBox::warning(this, windowTitle(), obs_module_text("OutputFailed"));
			return;
		}
		int suffix = devices->count() + 1;
		QString name;
		do {
			name = QString("OBS Dynamic Virtual Camera %1").arg(suffix++);
		} while (!devices->findItems(name, Qt::MatchExactly).isEmpty());
		add_device(id, name);
		save_catalog();
	});
	connect(rename, &QPushButton::clicked, this, [this] {
		const int row = devices->currentRow();
		if (row < 0) {
			return;
		}
		auto *panel = static_cast<DevicePanel *>(panels->widget(row));
		if (panel->running()) {
			QMessageBox::information(this, windowTitle(), obs_module_text("StopBeforeRename"));
			return;
		}
		bool accepted = false;
		const QString name = QInputDialog::getText(this, obs_module_text("RenameDevice"),
							   obs_module_text("DeviceName"), QLineEdit::Normal,
							   panel->name(), &accepted)
					     .trimmed();
		if (!accepted || name.isEmpty()) {
			return;
		}
		if (name.size() > 128 ||
		    (name != panel->name() && !devices->findItems(name, Qt::MatchExactly).isEmpty())) {
			QMessageBox::warning(this, windowTitle(), obs_module_text("InvalidDeviceName"));
			return;
		}
		panel->rename(name);
		devices->item(row)->setText(name);
		save_catalog();
	});
	connect(remove, &QPushButton::clicked, this, [this] {
		const int row = devices->currentRow();
		if (row < 0) {
			return;
		}
		auto *panel = static_cast<DevicePanel *>(panels->widget(row));
		const char *key = panel->has_receivers() ? "ConfirmRemoveInUseDevice" : "ConfirmRemoveDevice";
		if (!confirm_usage(this, windowTitle(), QString::fromUtf8(obs_module_text(key)).arg(panel->name()))) {
			return;
		}
		panel->stop();
		panels->removeWidget(panel);
		delete panel;
		delete devices->takeItem(row);
		if (devices->count()) {
			devices->setCurrentRow(std::min(row, devices->count() - 1));
		}
		save_catalog();
	});
	load_catalog();
	if (parent) {
		new CloseGuard(parent, this, [this, parent] {
			QStringList occupied;
			for (int index = 0; index < panels->count(); ++index) {
				auto *panel = static_cast<DevicePanel *>(panels->widget(index));
				if (panel->has_receivers()) {
					occupied.push_back(panel->name());
				}
			}
			return occupied.isEmpty() ||
			       confirm_usage(parent, windowTitle(),
					     QString::fromUtf8(obs_module_text("ConfirmExitInUseDevices"))
						     .arg(occupied.join("\n")));
		});
	}
}

Dialog::~Dialog()
{
	save_catalog();
	for (int index = 0; index < panels->count(); ++index) {
		static_cast<DevicePanel *>(panels->widget(index))->stop();
	}
}

void Dialog::add_device(const std::wstring &id, const QString &name, bool migrate)
{
	auto *panel = new DevicePanel(panels, id, name, migrate);
	panels->addWidget(panel);
	auto *item = new QListWidgetItem(name, devices);
	item->setToolTip(QString::fromStdWString(id));
	devices->setCurrentRow(devices->count() - 1);
}

void Dialog::load_catalog()
{
	char *path = obs_module_config_path("devices.json");
	obs_data_t *catalog = obs_data_create_from_json_file_safe(path, "bak");
	bfree(path);
	if (!catalog) {
		const std::wstring id = new_device_id();
		if (!id.empty()) {
			add_device(id, "OBS Dynamic Virtual Camera 1", true);
			save_catalog();
		}
		return;
	}
	obs_data_array_t *array = obs_data_get_array(catalog, "devices");
	for (size_t index = 0; index < obs_data_array_count(array); ++index) {
		obs_data_t *entry = obs_data_array_item(array, index);
		const std::wstring id = QString::fromUtf8(obs_data_get_string(entry, "id")).toStdWString();
		const QString name = QString::fromUtf8(obs_data_get_string(entry, "name")).trimmed();
		GUID parsed;
		wchar_t canonical[40];
		bool duplicate = false;
		for (int row = 0; row < panels->count(); ++row) {
			duplicate |= static_cast<DevicePanel *>(panels->widget(row))->id() == id;
		}
		if (!duplicate && id.size() == 38 && SUCCEEDED(CLSIDFromString(id.c_str(), &parsed)) &&
		    StringFromGUID2(parsed, canonical, 40) && _wcsicmp(id.c_str(), canonical) == 0 && !name.isEmpty() &&
		    name.size() <= 128 && devices->findItems(name, Qt::MatchExactly).isEmpty()) {
			add_device(canonical, name);
		}
		obs_data_release(entry);
	}
	obs_data_array_release(array);
	obs_data_release(catalog);
	if (devices->count()) {
		devices->setCurrentRow(0);
	}
}

void Dialog::save_catalog()
{
	obs_data_t *catalog = obs_data_create();
	obs_data_array_t *array = obs_data_array_create();
	for (int index = 0; index < panels->count(); ++index) {
		auto *panel = static_cast<DevicePanel *>(panels->widget(index));
		obs_data_t *entry = obs_data_create();
		obs_data_set_string(entry, "id", QString::fromStdWString(panel->id()).toUtf8().constData());
		obs_data_set_string(entry, "name", panel->name().toUtf8().constData());
		obs_data_array_push_back(array, entry);
		obs_data_release(entry);
	}
	obs_data_set_array(catalog, "devices", array);
	obs_data_array_release(array);
	char *directory = obs_module_config_path("");
	os_mkdirs(directory);
	bfree(directory);
	char *path = obs_module_config_path("devices.json");
	obs_data_save_json_safe(catalog, path, "tmp", "bak");
	bfree(path);
	obs_data_release(catalog);
}

}
