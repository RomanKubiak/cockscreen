#pragma once

#include <QString>
#include <QWidget>

class QPaintEvent;

namespace cockscreen::runtime
{

class StatusOverlay final : public QWidget
{
  public:
    explicit StatusOverlay(QWidget *parent = nullptr);

    void set_status(QString line, QString message);
    void set_status_overlay_text(QString text);
    QString status_message() const;

  protected:
    void paintEvent(QPaintEvent *event) override;

  private:
    QString line_;
    QString message_;
    QString status_overlay_text_;
};

} // namespace cockscreen::runtime