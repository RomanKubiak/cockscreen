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

  protected:
    void paintEvent(QPaintEvent *event) override;

  private:
    QString line_;
    QString message_;
};

} // namespace cockscreen::runtime