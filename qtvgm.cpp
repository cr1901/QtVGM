#include "qtvgm.h"
#include "playthread.hpp"
#include "./ui_qtvgm.h"

QtVGM::QtVGM(QWidget *parent)
    : QMainWindow(parent)
    , ui(new Ui::QtVGM)
{
    ui->setupUi(this);
    PlayThread * playThread = new PlayThread();

    connect(playThread, &PlayThread::newSong, this, &QtVGM::newSong);
    connect(playThread, &PlayThread::finished, playThread, &QObject::deleteLater);
    playThread->start();
}

QtVGM::~QtVGM()
{
    delete ui;
}

void QtVGM::newSong(const char* const* tagList)
{
  ui->textEdit->setText(tagList[0]);
}
