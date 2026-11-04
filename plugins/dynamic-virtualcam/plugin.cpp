#include <obs-module.h>
#include <obs-frontend-api.h>
#include "ui.hpp"
#include <QAction>
#include <QMainWindow>
#include <QPointer>

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("dynamic-virtualcam", "en-US")

static QPointer<dvc::Dialog> dialog;
static QPointer<QAction> action;

MODULE_EXPORT const char *obs_module_description(void)
{
	return "Multiple session-scoped virtual cameras with independent phase and test cards";
}

bool obs_module_load(void)
{
	dvc::Output::register_type();
	return true;
}

void obs_module_post_load(void)
{
	dialog = new dvc::Dialog(static_cast<QWidget *>(obs_frontend_get_main_window()));
	action = static_cast<QAction *>(obs_frontend_add_tools_menu_qaction(obs_module_text("Title")));
	QObject::connect(action, &QAction::triggered, dialog.data(), [] {
		dialog->show();
		dialog->raise();
		dialog->activateWindow();
	});
}

void obs_module_unload(void)
{
	delete action.data();
	delete dialog.data();
}
