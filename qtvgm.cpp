#include "qtvgm.h"
#include "playthread.hpp"
#include "./ui_qtvgm.h"

QtVGM::QtVGM(QWidget *parent)
    : QMainWindow(parent)
    , ui(new Ui::QtVGM)
{
    ui->setupUi(this);
    playThread = new PlayThread();

    connect(playThread, &PlayThread::newSong, this, &QtVGM::newSong);
    connect(playThread, &PlayThread::finished, playThread, &QObject::deleteLater);
}

QtVGM::~QtVGM()
{
    delete ui;
    // delete playThread; // FIXME: Should be stopped before running this.
                          // How do I guarantee it?
}

void QtVGM::startPlayer(const char * playList)
{
  playThread->setM3u(playList);
  playThread->start();
}

void QtVGM::newSong(const char* tagList)
{
  ui->textEdit->setText(tagList);
}
