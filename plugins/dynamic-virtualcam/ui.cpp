#include <obs-frontend-api.h>
#include "ui.hpp"
#include "usage-guard.hpp"

#include <util/platform.h>
#include <QComboBox>
#include <QSlider>
#include <QCheckBox>
#include <QHBoxLayout>
#include <QFormLayout>
#include <QLabel>
#include <QMessageBox>
#include <QMetaObject>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QTimer>
#include <QVBoxLayout>
#include <algorithm>
#include <shellapi.h>

namespace dvc {

static std::wstring data_file(const char *name)
{
	char *path = obs_module_file(name);
	const std::wstring result = path ? QString::fromUtf8(path).toStdWString() : L"";
	bfree(path);
	return result;
}

DevicePanel::DevicePanel(QWidget *parent, const std::wstring &id, const QString &name, bool migrate)
	: QWidget(parent),
	  device_id(id),
	  device_name(name)
{
	setWindowTitle(name);
	auto *layout = new QVBoxLayout(this);
	auto *form = new QFormLayout;
	type = new QComboBox(this);
	type->addItems({obs_module_text("Program"), obs_module_text("Preview"), obs_module_text("Scene"),
			obs_module_text("Source")});
	selection = new QComboBox(this);
	pixel_format = new QComboBox(this);
	for (const auto &entry : {std::pair{"NV12", PixelFormat::NV12},
				  {"YUY2", PixelFormat::YUY2},
				  {"RGB24", PixelFormat::RGB24},
				  {"RGB32", PixelFormat::RGB32},
				  {"ARGB32", PixelFormat::ARGB32}}) {
		pixel_format->addItem(entry.first, int(entry.second));
	}
	pixel_format->setToolTip(obs_module_text("PixelFormatHelp"));
	follow_obs_fps = new QCheckBox(this);
	use_obs_interval = new QCheckBox(obs_module_text("UseObsInterval"), this);
	use_obs_interval->setToolTip(obs_module_text("UseObsIntervalHelp"));
	test_card = new QComboBox(this);
	test_card->addItems({obs_module_text("TestCardOff"), obs_module_text("TestCardMotion")});
	test_card->setToolTip(obs_module_text("TestCardHelp"));
	numerator = new QSpinBox(this);
	denominator = new QSpinBox(this);
	for (auto *spin : {numerator, denominator}) {
		spin->setRange(1, 1000000);
	}
	numerator->setValue(60);
	denominator->setValue(1);
	slots = new QSpinBox(this);
	slots->setRange(1, int(max_slots));
	slots->setValue(5);
	target_frames = new QSpinBox(this);
	target_frames->setRange(1, slots->value());
	target_frames->setValue(2);
	target_frames->setToolTip(obs_module_text("BufferHelp"));
	slots->setToolTip(obs_module_text("BufferHelp"));
	phase = new QSlider(Qt::Horizontal, this);
	phase->setToolTip(obs_module_text("PhaseDetails"));
	phase_value = new QLabel(this);
	phase_value->setToolTip(obs_module_text("PhaseDetails"));
	auto *phase_layout = new QHBoxLayout;
	phase_layout->addWidget(phase, 1);
	phase_layout->addWidget(phase_value);
	form->addRow(obs_module_text("OutputType"), type);
	form->addRow(obs_module_text("OutputSelection"), selection);
	form->addRow(obs_module_text("PixelFormat"), pixel_format);
	form->addRow(follow_obs_fps);
	form->addRow(use_obs_interval);
	form->addRow(obs_module_text("FpsNumerator"), numerator);
	form->addRow(obs_module_text("FpsDenominator"), denominator);
	form->addRow(obs_module_text("BufferTarget"), target_frames);
	form->addRow(obs_module_text("BufferMax"), slots);
	form->addRow(obs_module_text("Phase"), phase_layout);
	auto *phase_help = new QLabel(obs_module_text("PhaseHelp"), this);
	phase_help->setWordWrap(true);
	form->addRow(phase_help);
	form->addRow(obs_module_text("TestCard"), test_card);
	layout->addLayout(form);
	auto *note = new QLabel(obs_module_text("Notes"), this);
	note->setWordWrap(true);
	layout->addWidget(note);
	statistics = new QLabel(this);
	statistics->setWordWrap(true);
	layout->addWidget(statistics, 1);
	status = new QLabel(obs_module_text("Stopped"), this);
	layout->addWidget(status);
	toggle = new QPushButton(obs_module_text("Start"), this);
	layout->addWidget(toggle);
	stopped.reset(CreateEventW(nullptr, TRUE, FALSE, nullptr));
	load_settings(migrate);
	refresh_sources();
	connect(type, &QComboBox::currentIndexChanged, this, [this] {
		refresh_sources();
		save_settings();
	});
	connect(selection, &QComboBox::currentTextChanged, this, [this] { save_settings(); });
	connect(test_card, &QComboBox::currentIndexChanged, this, [this] {
		save_settings();
		update_live_settings();
	});
	connect(phase, &QSlider::valueChanged, this, [this] {
		update_phase_range();
		save_settings();
		update_live_settings();
	});
	for (auto *spin : {numerator, denominator}) {
		connect(spin, &QSpinBox::valueChanged, this, [this] {
			update_phase_range();
			save_settings();
		});
	}
	connect(slots, &QSpinBox::valueChanged, this, [this] {
		target_frames->setMaximum(slots->value());
		save_settings();
	});
	connect(target_frames, &QSpinBox::valueChanged, this, [this] {
		slots->setMinimum(target_frames->value());
		save_settings();
	});
	connect(pixel_format, &QComboBox::currentIndexChanged, this, [this] { save_settings(); });
	connect(follow_obs_fps, &QCheckBox::toggled, this, [this](bool follow) {
		numerator->setEnabled(!output && !follow);
		denominator->setEnabled(!output && !follow);
		update_phase_range();
		save_settings();
	});
	connect(use_obs_interval, &QCheckBox::toggled, this, [this](bool enabled) {
		if (enabled) {
			follow_obs_fps->setChecked(true);
		}
		follow_obs_fps->setEnabled(!output && !enabled);
		update_phase_range();
		save_settings();
	});
	connect(toggle, &QPushButton::clicked, this, [this] {
		if (output) {
			if (has_receivers() &&
			    !confirm_usage(
				    this, windowTitle(),
				    QString::fromUtf8(obs_module_text("ConfirmStopInUseDevice")).arg(device_name))) {
				return;
			}
			stop();
		} else if (!start()) {
			const QString reason = status->text();
			stop();
			status->setText(reason);
			QMessageBox::warning(this, windowTitle(), reason);
		}
	});
	timer = new QTimer(this);
	connect(timer, &QTimer::timeout, this, [this] { refresh_statistics(); });
	timer->start(500);
	obs_frontend_add_event_callback(frontend_event, this);
}

DevicePanel::~DevicePanel()
{
	obs_frontend_remove_event_callback(frontend_event, this);
	stop();
	save_settings();
	obs_data_release(settings);
}

void DevicePanel::load_settings(bool migrate)
{
	const QByteArray filename = ("device-" + QString::fromStdWString(device_id) + ".json").toUtf8();
	char *path = obs_module_config_path(filename.constData());
	settings = obs_data_create_from_json_file_safe(path, "bak");
	bfree(path);
	if (!settings && migrate) {
		path = obs_module_config_path("settings.json");
		settings = obs_data_create_from_json_file_safe(path, "bak");
		bfree(path);
	}
	if (!settings) {
		settings = obs_data_create();
	}
	obs_data_set_default_int(settings, "fps_num", 60);
	obs_data_set_default_int(settings, "fps_den", 1);
	obs_data_set_default_int(settings, "max_frames",
				 obs_data_has_user_value(settings, "slots") ? obs_data_get_int(settings, "slots") : 5);
	obs_data_set_default_int(settings, "target_frames", 2);
	obs_data_set_default_bool(settings, "follow_obs_fps", true);
	obs_data_set_default_bool(settings, "use_obs_interval", false);
	obs_data_set_default_int(settings, "pixel_format", int(PixelFormat::NV12));
	use_obs_interval->setChecked(obs_data_get_bool(settings, "use_obs_interval"));
	follow_obs_fps->setChecked(obs_data_get_bool(settings, "follow_obs_fps") || use_obs_interval->isChecked());
	follow_obs_fps->setEnabled(!use_obs_interval->isChecked());
	pixel_format->setCurrentIndex(
		std::max(0, pixel_format->findData(int(obs_data_get_int(settings, "pixel_format")))));
	type->setCurrentIndex(std::clamp(int(obs_data_get_int(settings, "type")), 0, 3));
	numerator->setValue(int(obs_data_get_int(settings, "fps_num")));
	denominator->setValue(int(obs_data_get_int(settings, "fps_den")));
	const int maximum = int(std::clamp<int64_t>(obs_data_get_int(settings, "max_frames"), 1, max_slots));
	target_frames->setMaximum(maximum);
	target_frames->setValue(int(obs_data_get_int(settings, "target_frames")));
	slots->setMinimum(target_frames->value());
	slots->setValue(maximum);
	test_card->setCurrentIndex(std::clamp(int(obs_data_get_int(settings, "test_card")), 0, 1));
	update_phase_range();
	phase->setValue(int(std::clamp<int64_t>(obs_data_get_int(settings, "phase_ns"), 0, phase->maximum())));
	numerator->setEnabled(!follow_obs_fps->isChecked());
	denominator->setEnabled(!follow_obs_fps->isChecked());
	update_phase_range();
}

void DevicePanel::save_settings()
{
	obs_data_set_int(settings, "type", type->currentIndex());
	obs_data_set_string(settings, "selection", selection->currentText().toUtf8().constData());
	obs_data_set_int(settings, "fps_num", numerator->value());
	obs_data_set_int(settings, "fps_den", denominator->value());
	obs_data_set_int(settings, "max_frames", slots->value());
	obs_data_set_int(settings, "target_frames", target_frames->value());
	obs_data_set_int(settings, "pixel_format", pixel_format->currentData().toInt());
	obs_data_set_bool(settings, "follow_obs_fps", follow_obs_fps->isChecked());
	obs_data_set_bool(settings, "use_obs_interval", use_obs_interval->isChecked());
	obs_data_set_int(settings, "test_card", test_card->currentIndex());
	obs_data_set_int(settings, "phase_ns", phase->value());
	char *directory = obs_module_config_path("");
	os_mkdirs(directory);
	bfree(directory);
	const QByteArray filename = ("device-" + QString::fromStdWString(device_id) + ".json").toUtf8();
	char *path = obs_module_config_path(filename.constData());
	obs_data_save_json_safe(settings, path, "tmp", "bak");
	bfree(path);
}

Rate DevicePanel::effective_rate() const
{
	if (output) {
		const auto *context = static_cast<Output *>(obs_obj_get_data(output));
		if (context && context->queue) {
			return context->rate;
		}
	}
	if (follow_obs_fps->isChecked() || use_obs_interval->isChecked()) {
		obs_video_info info = {};
		if (obs_get_video_info(&info)) {
			return {info.fps_num, info.fps_den, use_obs_interval->isChecked()};
		}
	}
	return {uint32_t(numerator->value()), uint32_t(denominator->value())};
}

void DevicePanel::update_phase_range()
{
	const Rate rate = effective_rate();
	const int maximum = rate.valid() ? int(rate.offset(1) - 1) : 0;
	phase->setRange(0, maximum);
	phase->setSingleStep(std::max(1, maximum / 10000));
	phase->setPageStep(std::max(1, maximum / 100));
	phase_value->setText(QString("%1 / %2 ms")
				     .arg(double(phase->value()) / 1000000.0, 0, 'f', 6)
				     .arg(double(maximum) / 1000000.0, 0, 'f', 6));
	obs_video_info info = {};
	if (obs_get_video_info(&info)) {
		follow_obs_fps->setText(
			QString::fromUtf8(obs_module_text("FollowObsFps")).arg(info.fps_num).arg(info.fps_den));
	}
}

void DevicePanel::update_live_settings()
{
	if (!output) {
		return;
	}
	obs_data_t *update = obs_data_create();
	obs_data_set_int(update, "test_card", test_card->currentIndex());
	obs_data_set_int(update, "phase_ns", phase->value());
	obs_output_update(output, update);
	obs_data_release(update);
	blog(LOG_INFO, "[dynamic-virtualcam] Device %s: phase=%.6f ms test_card=%d", device_name.toUtf8().constData(),
	     double(phase->value()) / 1000000.0, test_card->currentIndex());
}

void DevicePanel::refresh_sources()
{
	if (output) {
		return;
	}
	const QSignalBlocker blocker(selection);
	const QString previous = selection->currentText().isEmpty()
					 ? QString::fromUtf8(obs_data_get_string(settings, "selection"))
					 : selection->currentText();
	selection->clear();
	selection->setEnabled(!output && type->currentIndex() >= 2);
	if (type->currentIndex() == 2) {
		obs_frontend_source_list scenes = {};
		obs_frontend_get_scenes(&scenes);
		for (size_t index = 0; index < scenes.sources.num; ++index) {
			selection->addItem(QString::fromUtf8(obs_source_get_name(scenes.sources.array[index])));
		}
		obs_frontend_source_list_free(&scenes);
	} else if (type->currentIndex() == 3) {
		obs_enum_sources(
			[](void *data, obs_source_t *source) {
				if (obs_source_get_output_flags(source) & OBS_SOURCE_VIDEO) {
					static_cast<QComboBox *>(data)->addItem(
						QString::fromUtf8(obs_source_get_name(source)));
				}
				return true;
			},
			selection);
	}
	const int index = selection->findText(previous);
	if (index >= 0) {
		selection->setCurrentIndex(index);
	}
}

void DevicePanel::update_route()
{
	if (!view) {
		return;
	}
	obs_source_t *source = nullptr;
	if (type->currentIndex() == 1) {
		source = obs_frontend_preview_program_mode_active() ? obs_frontend_get_current_preview_scene()
								    : obs_frontend_get_current_scene();
	} else {
		source = obs_get_source_by_name(selection->currentText().toUtf8().constData());
	}
	if (type->currentIndex() == 3) {
		obs_view_set_source(view, 0, nullptr);
		if (source_scene) {
			obs_scene_release(source_scene);
		}
		source_scene = obs_scene_create_private("dynamic-virtualcam-source");
		if (source) {
			obs_sceneitem_t *item = obs_scene_add(source_scene, source);
			if (item) {
				obs_sceneitem_set_alignment(item, OBS_ALIGN_TOP | OBS_ALIGN_LEFT);
				obs_sceneitem_set_bounds_type(item, OBS_BOUNDS_SCALE_INNER);
				obs_sceneitem_set_bounds_alignment(item, OBS_ALIGN_CENTER);
				obs_video_info info = {};
				obs_get_video_info(&info);
				const vec2 bounds = {float(info.base_width), float(info.base_height)};
				obs_sceneitem_set_bounds(item, &bounds);
			}
		}
		obs_source_release(source);
		source = obs_source_get_ref(obs_scene_get_source(source_scene));
	}
	obs_view_set_source(view, 0, source);
	obs_source_release(source);
}

void DevicePanel::clear_route()
{
	if (view) {
		obs_view_remove(view);
		obs_view_set_source(view, 0, nullptr);
		obs_view_destroy(view);
		view = nullptr;
	}
	if (source_scene) {
		obs_scene_release(source_scene);
		source_scene = nullptr;
	}
}

static std::wstring quote_argument(const std::wstring &value)
{
	std::wstring quoted = L"\"";
	size_t backslashes = 0;
	for (const wchar_t character : value) {
		if (character == L'\\') {
			++backslashes;
			continue;
		}
		quoted.append(backslashes * (character == L'"' ? 2 : 1), L'\\');
		backslashes = 0;
		if (character == L'"') {
			quoted += L'\\';
		}
		quoted += character;
	}
	quoted.append(backslashes * 2, L'\\');
	return quoted + L'"';
}

bool DevicePanel::launch_watchdog()
{
	watchdog_stop.reset(CreateEventW(nullptr, TRUE, FALSE, object_name(session, L"watchdog").c_str()));
	if (!watchdog_stop) {
		return false;
	}
	Handle ready(CreateEventW(nullptr, TRUE, FALSE, object_name(session, L"registered").c_str()));
	if (!ready) {
		return false;
	}
	const std::wstring executable = data_file("dvc-watchdog.exe");
	const std::wstring parameters =
		L"--register-machine " + quote_argument(session) + L" " + std::to_wstring(GetCurrentProcessId()) +
		L" " + std::to_wstring(process_start(GetCurrentProcess())) + L" " + quote_argument(device_id) + L" " +
		quote_argument(data_file("dvc-filter64.dll")) + L" " + quote_argument(data_file("dvc-filter32.dll")) +
		L" " + quote_argument(device_name.toStdWString());
	SHELLEXECUTEINFOW process = {};
	process.cbSize = sizeof(process);
	process.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC | SEE_MASK_FLAG_NO_UI;
	process.hwnd = reinterpret_cast<HWND>(window()->winId());
	process.lpVerb = L"runas";
	process.lpFile = executable.c_str();
	process.lpParameters = parameters.c_str();
	process.nShow = SW_HIDE;
	if (!ShellExecuteExW(&process)) {
		blog(LOG_WARNING, "[dynamic-virtualcam] Machine registration launch failed: Win32=%lu", GetLastError());
		return false;
	}
	watchdog.reset(process.hProcess);
	HANDLE handles[] = {ready, watchdog};
	if (WaitForMultipleObjects(2, handles, FALSE, 15000) == WAIT_OBJECT_0 &&
	    WaitForSingleObject(watchdog, 0) == WAIT_TIMEOUT) {
		return true;
	}
	DWORD code = STILL_ACTIVE;
	GetExitCodeProcess(watchdog, &code);
	blog(LOG_WARNING, "[dynamic-virtualcam] Machine registration was not ready: helper_exit=%lu", code);
	SetEvent(watchdog_stop);
	return false;
}

bool DevicePanel::start()
{
	const Rate rate = effective_rate();
	if (!rate.valid()) {
		status->setText(obs_module_text("InvalidRate"));
		return false;
	}
	owner_mutex.reset(CreateMutexW(nullptr, TRUE, object_name(device_id, L"owner").c_str()));
	if (!owner_mutex || GetLastError() == ERROR_ALREADY_EXISTS) {
		owner_mutex.reset();
		status->setText(obs_module_text("AlreadyRunning"));
		return false;
	}
	const std::wstring stale = active_session(device_id);
	if (!stale.empty()) {
		unregister_device(stale, device_id);
	}
	GUID id;
	if (FAILED(CoCreateGuid(&id))) {
		status->setText(obs_module_text("OutputFailed"));
		return false;
	}
	wchar_t text[40];
	StringFromGUID2(id, text, 40);
	session = text;
	obs_data_t *parameters = obs_data_create();
	obs_data_set_string(parameters, "session", QString::fromStdWString(session).toUtf8().constData());
	obs_data_set_int(parameters, "fps_num", rate.numerator);
	obs_data_set_int(parameters, "fps_den", rate.denominator);
	obs_data_set_int(parameters, "max_frames", slots->value());
	obs_data_set_int(parameters, "target_frames", target_frames->value());
	obs_data_set_int(parameters, "pixel_format", pixel_format->currentData().toInt());
	obs_data_set_bool(parameters, "follow_obs_fps", follow_obs_fps->isChecked());
	obs_data_set_bool(parameters, "use_obs_interval", use_obs_interval->isChecked());
	obs_data_set_int(parameters, "test_card", test_card->currentIndex());
	obs_data_set_int(parameters, "phase_ns", phase->value());
	const QByteArray output_name = ("DVC-" + QString::fromStdWString(device_id)).toUtf8();
	output = obs_output_create("dynamic_virtualcam_output", output_name.constData(), parameters, nullptr);
	obs_data_release(parameters);
	if (!output) {
		status->setText(obs_module_text("OutputFailed"));
		return false;
	}
	video_t *video = obs_get_video();
	if (type->currentIndex() != 0) {
		view = obs_view_create();
		if (!view) {
			status->setText(obs_module_text("OutputFailed"));
			return false;
		}
		update_route();
		video = obs_view_add(view);
	}
	if (!video) {
		status->setText(obs_module_text("OutputFailed"));
		return false;
	}
	obs_output_set_media(output, video, nullptr);
	signal_handler_connect(
		obs_output_get_signal_handler(output), "deactivate",
		[](void *data, calldata_t *) { SetEvent(static_cast<DevicePanel *>(data)->stopped); }, this);
	ResetEvent(stopped);
	if (!obs_output_start(output)) {
		status->setText(obs_module_text("OutputFailed"));
		return false;
	}
	if (!launch_watchdog()) {
		status->setText(obs_module_text("RegistrationFailed"));
		return false;
	}
	update_phase_range();
	save_settings();
	for (auto *widget : std::array<QWidget *, 9>{type, selection, numerator, denominator, slots, target_frames,
						     pixel_format, follow_obs_fps, use_obs_interval}) {
		widget->setEnabled(false);
	}
	toggle->setText(obs_module_text("Stop"));
	status->setText(obs_module_text("Running"));
	blog(LOG_INFO, "[dynamic-virtualcam] Device registered: scope=machine name=%s id=%s session=%s",
	     device_name.toUtf8().constData(), QString::fromStdWString(device_id).toUtf8().constData(),
	     QString::fromStdWString(session).toUtf8().constData());
	return true;
}

void DevicePanel::stop()
{
	if (watchdog_stop) {
		SetEvent(watchdog_stop);
	}
	if (watchdog && WaitForSingleObject(watchdog, 5000) != WAIT_OBJECT_0) {
		blog(LOG_WARNING, "[dynamic-virtualcam] Machine device cleanup is still pending");
	}
	if (!session.empty()) {
		unregister_device(session, device_id);
	}
	if (output) {
		if (obs_output_active(output)) {
			obs_output_force_stop(output);
			WaitForSingleObject(stopped, INFINITE);
		}
		obs_output_release(output);
		output = nullptr;
	}
	clear_route();
	watchdog.reset();
	watchdog_stop.reset();
	if (owner_mutex) {
		ReleaseMutex(owner_mutex);
	}
	owner_mutex.reset();
	session.clear();
	statistics_time = 0;
	previous_delivered = previous_repeated = 0;
	previous_published = 0;
	statistics->clear();
	for (auto *widget : std::array<QWidget *, 9>{type, selection, numerator, denominator, slots, target_frames,
						     pixel_format, follow_obs_fps, use_obs_interval}) {
		widget->setEnabled(true);
	}
	follow_obs_fps->setEnabled(!use_obs_interval->isChecked());
	numerator->setEnabled(!follow_obs_fps->isChecked());
	denominator->setEnabled(!follow_obs_fps->isChecked());
	selection->setEnabled(type->currentIndex() >= 2);
	toggle->setText(obs_module_text("Start"));
	status->setText(obs_module_text("Stopped"));
	refresh_sources();
}

bool DevicePanel::has_receivers() const
{
	if (!output) {
		return false;
	}
	auto *context = static_cast<Output *>(obs_obj_get_data(output));
	if (!context || !context->queue) {
		return false;
	}
	auto &queue = *context->queue;
	if (!queue.lock_control(0)) {
		return true;
	}
	bool occupied = false;
	for (const auto &client : queue.header()->clients) {
		occupied |= client.pid != 0;
	}
	queue.unlock_control();
	return occupied;
}

void DevicePanel::refresh_statistics()
{
	if (!output) {
		update_phase_range();
		return;
	}
	auto *context = static_cast<Output *>(obs_obj_get_data(output));
	if (!context || !context->queue) {
		return;
	}
	status->setText(obs_module_text(context->card_failed ? "TestCardFailed" : "Running"));
	auto &queue = *context->queue;
	const uint64_t time = now_ns();
	const bool log_clients = statistics_time && time - statistics_time >= 5000000000;
	if (!queue.lock_control(0)) {
		return;
	}
	uint64_t instances = 0, streaming = 0, expired = 0, prepare = 0, receive = 0, late = 0;
	uint64_t buffered = 0, rebuffers = 0, capture_skipped = 0;
	uint32_t result = 0;
	for (auto &client : queue.header()->clients) {
		if (!client.pid) {
			continue;
		}
		if (!process_alive(client.pid, client.process_start)) {
			memset(&client, 0, sizeof(client));
			continue;
		}
		++instances;
		streaming += load(client.streaming) != 0;
		expired += load(client.expired);
		buffered = std::max(buffered, load(client.buffered));
		rebuffers += load(client.rebuffers);
		capture_skipped += load(client.capture_skipped);
		prepare = std::max(prepare, load(client.prepare_ns));
		receive = std::max(receive, load(client.receive_ns));
		late = std::max(late, load(client.late_ns));
		if (load(client.last_result)) {
			result = uint32_t(load(client.last_result));
		}
		if (log_clients) {
			const uint64_t receive_start = load(client.receive_start_ns);
			const uint64_t receive_active = receive_start && time > receive_start ? time - receive_start
											      : 0;
			blog(LOG_INFO,
			     "[dynamic-virtualcam-client] id=%s token=%llu pid=%lu streaming=%llu delivered=%llu repeated=%llu "
			     "expired=%llu buffered=%llu rebuffers=%llu captured=%llu capture_skipped=%llu read_contentions=%llu read_timeouts=%llu "
			     "pts_skipped=%llu pts_late_ms=%.3f last_receive_ms=%.3f max_receive_ms=%.3f receive_active_ms=%.3f",
			     QString::fromStdWString(device_id).toUtf8().constData(),
			     static_cast<unsigned long long>(client.token), static_cast<unsigned long>(client.pid),
			     static_cast<unsigned long long>(load(client.streaming)),
			     static_cast<unsigned long long>(load(client.delivered)),
			     static_cast<unsigned long long>(load(client.repeated)),
			     static_cast<unsigned long long>(load(client.expired)),
			     static_cast<unsigned long long>(load(client.buffered)),
			     static_cast<unsigned long long>(load(client.rebuffers)),
			     static_cast<unsigned long long>(load(client.captured)),
			     static_cast<unsigned long long>(load(client.capture_skipped)),
			     static_cast<unsigned long long>(load(client.read_contentions)),
			     static_cast<unsigned long long>(load(client.read_timeouts)),
			     static_cast<unsigned long long>(load(client.pts_skipped)),
			     double(load(client.pts_late_ns)) / 1000000.0, double(load(client.receive_ns)) / 1000000.0,
			     double(load(client.max_receive_ns)) / 1000000.0, double(receive_active) / 1000000.0);
		}
	}
	queue.unlock_control();
	const uint64_t delivered = load(queue.header()->delivered), repeated = load(queue.header()->repeated);
	const uint64_t published = load(queue.header()->published);
	statistics->setText(QString::fromUtf8(obs_module_text("DeviceStatistics"))
				    .arg(instances)
				    .arg(streaming)
				    .arg(delivered)
				    .arg(repeated)
				    .arg(expired)
				    .arg(double(prepare) / 1000000.0, 0, 'f', 3)
				    .arg(double(receive) / 1000000.0, 0, 'f', 3)
				    .arg(buffered)
				    .arg(rebuffers)
				    .arg(capture_skipped));
	if (!statistics_time) {
		statistics_time = time;
		previous_delivered = delivered;
		previous_repeated = repeated;
		previous_published = published;
	} else if (time - statistics_time >= 5000000000) {
		const double seconds = double(time - statistics_time) / 1000000000.0;
		const uint64_t frames = delivered - previous_delivered, repeats = repeated - previous_repeated;
		blog(LOG_INFO,
		     "[dynamic-virtualcam-stats] device=%s id=%s receivers=%llu streaming=%llu size=%ux%u "
		     "target=%u/%u delivered_fps=%.2f fresh_fps=%.2f repeated=%llu expired=%llu "
		     "last_prepare_ms=%.3f last_receive_ms=%.3f late_ms=%.3f result=0x%08lx published=%llu "
		     "publish_dropped=%llu published_fps=%.2f phase_ms=%.6f format=%u buffer_target=%u buffer_max=%u buffered=%llu rebuffers=%llu",
		     device_name.toUtf8().constData(), QString::fromStdWString(device_id).toUtf8().constData(),
		     static_cast<unsigned long long>(instances), static_cast<unsigned long long>(streaming),
		     queue.header()->width, queue.header()->height, context->rate.numerator, context->rate.denominator,
		     double(frames) / seconds, double(frames - std::min(frames, repeats)) / seconds,
		     static_cast<unsigned long long>(repeated), static_cast<unsigned long long>(expired),
		     double(prepare) / 1000000.0, double(receive) / 1000000.0, double(late) / 1000000.0,
		     static_cast<unsigned long>(result),
		     static_cast<unsigned long long>(load(queue.header()->published)),
		     static_cast<unsigned long long>(load(queue.header()->dropped)),
		     double(published - previous_published) / seconds, double(load(queue.header()->phase)) / 1000000.0,
		     queue.header()->pixel_format, queue.header()->target_frames, queue.header()->slots,
		     static_cast<unsigned long long>(buffered), static_cast<unsigned long long>(rebuffers));
		statistics_time = time;
		previous_delivered = delivered;
		previous_repeated = repeated;
		previous_published = published;
	}
}

void DevicePanel::frontend_event(obs_frontend_event event, void *data)
{
	auto *dialog = static_cast<DevicePanel *>(data);
	QMetaObject::invokeMethod(
		dialog,
		[dialog, event] {
			switch (event) {
			case OBS_FRONTEND_EVENT_SCENE_CHANGED:
			case OBS_FRONTEND_EVENT_PREVIEW_SCENE_CHANGED:
			case OBS_FRONTEND_EVENT_STUDIO_MODE_ENABLED:
			case OBS_FRONTEND_EVENT_STUDIO_MODE_DISABLED:
				if (dialog->type->currentIndex() == 1) {
					dialog->update_route();
				}
				break;
			case OBS_FRONTEND_EVENT_SCENE_LIST_CHANGED:
			case OBS_FRONTEND_EVENT_FINISHED_LOADING:
				dialog->refresh_sources();
				break;
			case OBS_FRONTEND_EVENT_SCENE_COLLECTION_CHANGING:
			case OBS_FRONTEND_EVENT_SCRIPTING_SHUTDOWN:
			case OBS_FRONTEND_EVENT_EXIT:
				dialog->stop();
				break;
			default:
				break;
			}
		},
		Qt::AutoConnection);
}

}
