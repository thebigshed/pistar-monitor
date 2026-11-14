#include "LinkDialog.h"

#include <QComboBox>
#include <QDialogButtonBox>
#include <QHBoxLayout>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QVBoxLayout>

LinkDialog::LinkDialog(const QString &mode, const QString &current, const QJsonArray &entries, bool needsModule,
                       QWidget *parent)
    : QDialog(parent), mode_(mode), needsModule_(needsModule)
{
    setWindowTitle(QString("%1 link").arg(mode));
    auto *root = new QVBoxLayout(this);

    auto *now = new QLabel(QString("Currently: <b>%1</b>").arg(current.isEmpty() ? QString("not linked") : current.toHtmlEscaped()));
    root->addWidget(now);

    search_ = new QLineEdit;
    search_->setPlaceholderText(mode == "P25" || mode == "NXDN" ? "Search, or type a talkgroup number"
                                : mode == "YSF" ? "Search by name or ID, or type a reflector ID"
                                                : "Search, or type a reflector name");
    search_->setClearButtonEnabled(true);
    root->addWidget(search_);

    list_ = new QListWidget;
    list_->setUniformItemSizes(true);
    for (const QJsonValue &v : entries) {
        const QJsonObject o = v.toObject();
        const QString id = o.value("id").toString(), name = o.value("name").toString();
        auto *it = new QListWidgetItem(name.isEmpty() ? id : QString("%1    %2").arg(id, name), list_);
        it->setData(Qt::UserRole, id);
    }
    root->addWidget(list_, 1);

    auto *row = new QHBoxLayout;
    if (needsModule_) {
        module_ = new QComboBox;
        for (char c = 'A'; c <= 'Z'; ++c) module_->addItem(QString(QChar(c)));
        // Preselect the module currently in use, e.g. "DCS011 C" / "M17-672 C"
        const QString cur = current.section(' ', 1, 1);
        module_->setCurrentText(cur.size() == 1 ? cur : QString(mode == "D-Star" ? "C" : "A"));
        row->addWidget(new QLabel("Module"));
        row->addWidget(module_);
        connect(module_, &QComboBox::currentTextChanged, this, &LinkDialog::updateOk);
    }
    chosen_ = new QLabel;
    row->addWidget(chosen_, 1);
    root->addLayout(row);

    auto *buttons = new QDialogButtonBox;
    linkBtn_ = buttons->addButton("Link", QDialogButtonBox::AcceptRole);
    auto *unlinkBtn = buttons->addButton("Unlink", QDialogButtonBox::DestructiveRole);
    buttons->addButton(QDialogButtonBox::Cancel);
    unlinkBtn->setEnabled(!current.isEmpty());
    connect(linkBtn_, &QPushButton::clicked, this, &QDialog::accept);
    connect(unlinkBtn, &QPushButton::clicked, this, [this] { unlink_ = true; accept(); });
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    root->addWidget(buttons);

    connect(search_, &QLineEdit::textChanged, this, &LinkDialog::filter);
    connect(list_, &QListWidget::currentItemChanged, this, &LinkDialog::updateOk);
    connect(list_, &QListWidget::itemDoubleClicked, this, [this] { if (linkBtn_->isEnabled()) accept(); });
    connect(search_, &QLineEdit::returnPressed, this, [this] { if (linkBtn_->isEnabled()) accept(); });

    // Start on the current link if it is in the list
    for (int i = 0; i < list_->count(); ++i)
        if (list_->item(i)->data(Qt::UserRole).toString() == current.section(' ', 0, 0) ||
            list_->item(i)->data(Qt::UserRole).toString() == current.section(' ', 1, 1)) {
            list_->setCurrentRow(i);
            list_->scrollToItem(list_->item(i), QAbstractItemView::PositionAtCenter);
            break;
        }
    updateOk();
    resize(460, 520);
    search_->setFocus();
}

void LinkDialog::filter(const QString &text)
{
    const QString t = text.trimmed();
    QListWidgetItem *firstShown = nullptr;
    for (int i = 0; i < list_->count(); ++i) {
        QListWidgetItem *it = list_->item(i);
        const bool show = t.isEmpty() || it->text().contains(t, Qt::CaseInsensitive);
        it->setHidden(!show);
        if (show && !firstShown) firstShown = it;
    }
    // Typing narrows the list; keep a visible selection, or none so the typed text is used.
    if (list_->currentItem() && list_->currentItem()->isHidden()) list_->setCurrentItem(nullptr);
    if (!list_->currentItem() && firstShown && !t.isEmpty()) {
        // An exact ID match is selected; otherwise the typed text stands on its own.
        if (firstShown->data(Qt::UserRole).toString().compare(t, Qt::CaseInsensitive) == 0)
            list_->setCurrentItem(firstShown);
    }
    updateOk();
}

QString LinkDialog::target() const
{
    QString base = list_->currentItem() && !list_->currentItem()->isHidden()
                       ? list_->currentItem()->data(Qt::UserRole).toString()
                       : search_->text().trimmed().toUpper();
    if (needsModule_ && module_) {
        base = base.section(' ', 0, 0);   // typed "DCS011 C" -> reflector part
        return base.isEmpty() ? QString() : base + " " + module_->currentText();
    }
    return base;
}

void LinkDialog::updateOk()
{
    QString t = target();
    bool ok = !t.isEmpty();
    if (ok && (mode_ == "P25" || mode_ == "NXDN")) {
        bool num = false;
        t.toUInt(&num);
        ok = num;
    }
    linkBtn_->setEnabled(ok);
    chosen_->setText(ok ? QString("→ <b>%1</b>").arg((mode_ == "P25" || mode_ == "NXDN" ? "TG " : "") + t.toHtmlEscaped())
                        : QString());
}
