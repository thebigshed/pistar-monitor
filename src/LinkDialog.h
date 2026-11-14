#pragma once
// Picks what a mode should be linked to (reflector / talkgroup), from the
// Pi-Star host lists fetched by remote_link.py.
#include <QDialog>
#include <QJsonArray>

class QLineEdit;
class QListWidget;
class QComboBox;
class QLabel;

class LinkDialog : public QDialog {
    Q_OBJECT
public:
    LinkDialog(const QString &mode, const QString &current, const QJsonArray &entries, bool needsModule,
               QWidget *parent = nullptr);
    bool unlinkRequested() const { return unlink_; }
    QString target() const;   // e.g. "DCS011 C", "00007", "31672", "M17-672 C"

private:
    void filter(const QString &text);
    void updateOk();

    QString mode_;
    bool needsModule_ = false;
    bool unlink_ = false;
    QLineEdit *search_ = nullptr;
    QListWidget *list_ = nullptr;
    QComboBox *module_ = nullptr;
    QLabel *chosen_ = nullptr;
    QPushButton *linkBtn_ = nullptr;
};
