#pragma once

#include "output.hpp"
#include <obs-frontend-api.h>
#include <QDialog>

class QComboBox;
class QSpinBox;
class QSlider;
class QCheckBox;
class QPushButton;
class QLabel;
class QTimer;
class QListWidget;
class QStackedWidget;

namespace dvc {

class DevicePanel final : public QWidget {
	QComboBox *type;
	QComboBox *selection;
	QComboBox *test_card;
	QSpinBox *numerator;
	QSpinBox *denominator;
	QSpinBox *slots;
	QSpinBox *target_frames;
	QSlider *phase;
	QLabel *phase_value;
	QComboBox *pixel_format;
	QCheckBox *follow_obs_fps;
	QCheckBox *use_obs_interval;
	QPushButton *toggle;
	QLabel *statistics;
	QLabel *status;
	QTimer *timer;
	obs_data_t *settings = nullptr;
	obs_output_t *output = nullptr;
	obs_view_t *view = nullptr;
	obs_scene_t *source_scene = nullptr;
	std::wstring device_id;
	QString device_name;
	std::wstring session;
	Handle owner_mutex;
	Handle watchdog_stop;
	Handle watchdog;
	Handle stopped;
	uint64_t statistics_time = 0;
	uint64_t previous_delivered = 0;
	uint64_t previous_repeated = 0;
	uint64_t previous_published = 0;

	void load_settings(bool migrate);
	void save_settings();
	void refresh_sources();
	Rate effective_rate() const;
	void update_phase_range();
	void update_live_settings();
	bool start();
	void update_route();
	void refresh_statistics();
	bool launch_watchdog();
	void clear_route();
	static void frontend_event(enum obs_frontend_event event, void *data);

public:
	DevicePanel(QWidget *parent, const std::wstring &id, const QString &name, bool migrate = false);
	~DevicePanel() override;
	void stop();
	bool has_receivers() const;
	bool running() const { return output != nullptr; }
	const std::wstring &id() const { return device_id; }
	const QString &name() const { return device_name; }
	void rename(const QString &name)
	{
		device_name = name;
		setWindowTitle(name);
	}
};

class Dialog final : public QDialog {
	QListWidget *devices;
	QStackedWidget *panels;

	void add_device(const std::wstring &id, const QString &name, bool migrate = false);
	void save_catalog();
	void load_catalog();

public:
	explicit Dialog(QWidget *parent);
	~Dialog() override;
};

}
