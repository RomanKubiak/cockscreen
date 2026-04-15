#include "../../include/cockscreen/runtime/StatusOverlay.hpp"

#include <QFont>
#include <QFontDatabase>
#include <QPaintEvent>
#include <QPainter>

#include <utility>

namespace cockscreen::runtime
{

StatusOverlay::StatusOverlay(QWidget *parent) : QWidget{parent}
{
    setAttribute(Qt::WA_TransparentForMouseEvents, true);
    setAttribute(Qt::WA_OpaquePaintEvent, true);
    setAttribute(Qt::WA_TranslucentBackground, true);
    setAutoFillBackground(false);
}

void StatusOverlay::set_status(QString line, QString message)
{
    if (line_ == line && message_ == message && status_overlay_text_.isEmpty())
    {
        return;
    }

    line_ = std::move(line);
    message_ = std::move(message);
    status_overlay_text_.clear();
    update();
}

void StatusOverlay::set_status_overlay_text(QString text)
{
    if (status_overlay_text_ == text)
    {
        return;
    }

    status_overlay_text_ = std::move(text);
    line_.clear();
    message_.clear();
    update();
}

QString StatusOverlay::status_message() const
{
    return status_overlay_text_;
}

void StatusOverlay::paintEvent(QPaintEvent *)
{
    QPainter painter{this};
    painter.setCompositionMode(QPainter::CompositionMode_Source);
    painter.fillRect(rect(), QColor{0, 0, 0, 220});
    painter.setCompositionMode(QPainter::CompositionMode_SourceOver);
    painter.setRenderHint(QPainter::TextAntialiasing, true);

    QFont status_font = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    status_font.setPointSize(8);
    painter.setFont(status_font);
    painter.setPen(Qt::white);

    QString overlay_text = status_overlay_text_;
    if (overlay_text.isEmpty())
    {
        overlay_text = line_;
        if (!message_.isEmpty())
        {
            overlay_text.append('\n');
            overlay_text.append(message_);
        }
    }

    painter.drawText(rect().adjusted(18, 18, -18, -18), Qt::AlignLeft | Qt::AlignTop | Qt::TextWordWrap,
                     overlay_text);
}

} // namespace cockscreen::runtime