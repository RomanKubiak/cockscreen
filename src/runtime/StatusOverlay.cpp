#include "../../include/cockscreen/runtime/StatusOverlay.hpp"

#include <QFont>
#include <QPaintEvent>
#include <QPainter>

#include <utility>

namespace cockscreen::runtime
{

StatusOverlay::StatusOverlay(QWidget *parent) : QWidget{parent}
{
    setAttribute(Qt::WA_TransparentForMouseEvents, true);
    setAttribute(Qt::WA_OpaquePaintEvent, true);
    setAutoFillBackground(false);
}

void StatusOverlay::set_status(QString line, QString message)
{
    if (line_ == line && message_ == message)
    {
        return;
    }

    line_ = std::move(line);
    message_ = std::move(message);
    update();
}

void StatusOverlay::paintEvent(QPaintEvent *)
{
    QPainter painter{this};
    painter.setCompositionMode(QPainter::CompositionMode_Source);
    painter.fillRect(rect(), Qt::black);
    painter.setCompositionMode(QPainter::CompositionMode_SourceOver);
    painter.setRenderHint(QPainter::TextAntialiasing, true);

    QFont status_font{"Sans Serif", 10};
    status_font.setBold(true);
    painter.setFont(status_font);
    painter.setPen(Qt::white);

    painter.drawText(rect().adjusted(16, 0, -16, 0), Qt::AlignVCenter | Qt::AlignLeft, line_);
    if (!message_.isEmpty())
    {
        painter.drawText(rect().adjusted(16, 0, -16, 0), Qt::AlignVCenter | Qt::AlignRight, message_);
    }
}

} // namespace cockscreen::runtime