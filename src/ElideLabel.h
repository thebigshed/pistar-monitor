#pragma once
// A plain-text label that never asks the window for more width: when the text is too
// long for the space it has, it is shortened with "…" (the full text is in the tooltip).
// setText()/text() hide QLabel's, so use it through an ElideLabel pointer.
#include <QLabel>
#include <QResizeEvent>

class ElideLabel : public QLabel {
public:
    explicit ElideLabel(QWidget *parent = nullptr) : QLabel(parent)
    {
        setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        setTextFormat(Qt::PlainText);
    }
    void setText(const QString &text)
    {
        if (text == full_) return;
        full_ = text;
        relayout();
    }
    QString text() const { return full_; }
    QSize minimumSizeHint() const override { return {0, QLabel::minimumSizeHint().height()}; }

protected:
    void resizeEvent(QResizeEvent *e) override
    {
        QLabel::resizeEvent(e);
        relayout();
    }

private:
    void relayout()
    {
        const QString shown = fontMetrics().elidedText(full_, Qt::ElideRight, std::max(0, contentsRect().width()));
        QLabel::setText(shown);
        setToolTip(shown == full_ ? QString() : full_);
    }
    QString full_;
};
