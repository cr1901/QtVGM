#ifndef QTVGM_H
#define QTVGM_H

#include <QMainWindow>

QT_BEGIN_NAMESPACE
namespace Ui { class QtVGM; }
QT_END_NAMESPACE

class QtVGM : public QMainWindow
{
    Q_OBJECT

public:
    QtVGM(QWidget *parent = nullptr);
    ~QtVGM();

private:
    Ui::QtVGM *ui;
};
#endif // QTVGM_H
