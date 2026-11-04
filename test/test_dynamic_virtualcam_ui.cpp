#include "usage-guard.hpp"

#include <QAbstractButton>
#include <QApplication>
#include <QCloseEvent>
#include <QPushButton>
#include <QTimer>
#include <cstdio>
#include <cstdlib>

#define CHECK(condition) do { if (!(condition)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition); abort(); } } while (0)

class TestWindow final : public QWidget {
public:
	int closes = 0;
	bool allow_close = true;

	void closeEvent(QCloseEvent *event) override
	{
		++closes;
		event->setAccepted(allow_close);
	}
};

static bool send_close(QWidget &window)
{
	QCloseEvent event;
	QApplication::sendEvent(&window, &event);
	return event.isAccepted();
}

static void test_close_guard()
{
	TestWindow main;
	QWidget manager(&main);
	bool occupied = false;
	bool allow = false;
	bool nested_close = false;
	int queries = 0;
	int prompts = 0;
	auto *guard = new dvc::CloseGuard(&main, &manager, [&] {
		++queries;
		if (!occupied) {
			return true;
		}
		++prompts;
		if (nested_close) {
			CHECK(!send_close(main));
		}
		return allow;
	});
	CHECK(send_close(main));
	CHECK(main.closes == 1 && prompts == 0 && queries == 1);
	occupied = true;
	CHECK(!send_close(main));
	CHECK(main.closes == 1 && prompts == 1);
	nested_close = true;
	CHECK(!send_close(main));
	CHECK(main.closes == 1 && prompts == 2 && queries == 3);
	allow = true;
	CHECK(send_close(main));
	CHECK(main.closes == 2 && prompts == 3);
	main.allow_close = false;
	CHECK(!send_close(main));
	CHECK(prompts == 4);
	main.allow_close = true;
	CHECK(send_close(main));
	CHECK(prompts == 5);
	CHECK(send_close(manager));
	main.hide();
	QEvent state_change(QEvent::WindowStateChange);
	QApplication::sendEvent(&main, &state_change);
	CHECK(prompts == 5 && queries == 6);
	delete guard;
	CHECK(send_close(main));
	CHECK(prompts == 5 && queries == 6);
}

static void test_confirmation()
{
	QWidget parent;
	const QString message = "Camera <b>name</b> is in use.\nStop?";
	for (const auto answer : {QMessageBox::No, QMessageBox::Yes, QMessageBox::Cancel}) {
		QTimer::singleShot(0, [&] {
			auto *prompt = qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
			CHECK(prompt);
			CHECK(prompt->defaultButton() == prompt->button(QMessageBox::No));
			CHECK(prompt->escapeButton() == prompt->button(QMessageBox::No));
			CHECK(prompt->textFormat() == Qt::PlainText && prompt->text() == message);
			if (answer == QMessageBox::Cancel) {
				prompt->close();
			} else {
				prompt->button(answer)->click();
			}
		});
		CHECK(dvc::confirm_usage(&parent, "Camera in use", message) == (answer == QMessageBox::Yes));
	}
}

int main(int argc, char **argv)
{
	QApplication app(argc, argv);
	app.setQuitOnLastWindowClosed(false);
	test_close_guard();
	test_confirmation();
	puts("Dynamic virtual camera UI confirmation tests passed.");
	return 0;
}
