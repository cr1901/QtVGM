#ifndef QTVGM_H
#define QTVGM_H

#include "playthread.hpp"
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
    void startPlayer(const char * playList);

private slots:
  void newSong(const char* tagList);

private:
    Ui::QtVGM *ui;
    PlayThread * playThread;
};
#endif // QTVGM_H
