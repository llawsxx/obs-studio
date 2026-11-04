#pragma once

#include <QEvent>
#include <QMessageBox>
#include <QScopedValueRollback>
#include <functional>
#include <utility>

namespace dvc {

inline bool confirm_usage(QWidget *parent, const QString &title, const QString &message)
{
	QMessageBox prompt(QMessageBox::Question, title, message, QMessageBox::Yes | QMessageBox::No, parent);
	prompt.setTextFormat(Qt::PlainText);
	prompt.setDefaultButton(QMessageBox::No);
	prompt.setEscapeButton(QMessageBox::No);
	return prompt.exec() == QMessageBox::Yes;
}

class CloseGuard final : public QObject {
	std::function<bool()> confirm;
	bool prompting = false;

	bool eventFilter(QObject *, QEvent *event) override
	{
		if (event->type() != QEvent::Close) {
			return false;
		}
		if (prompting) {
			event->ignore();
			return true;
		}
		QScopedValueRollback<bool> guard(prompting, true);
		if (!confirm()) {
			event->ignore();
			return true;
		}
		return false;
	}

public:
	CloseGuard(QObject *watched, QObject *parent, std::function<bool()> confirm)
		: QObject(parent),
		  confirm(std::move(confirm))
	{
		watched->installEventFilter(this);
	}
};

}
