#include "qtvgm.h"
#include "playthread.hpp"
#include "./ui_qtvgm.h"

#include <QKeyEvent>

QtVGM::QtVGM(QWidget *parent)
    : QMainWindow(parent)
    , ui(new Ui::QtVGM)
{
    ui->setupUi(this);
    ui->textEdit->setFocusPolicy(Qt::NoFocus);
    setFocusPolicy(Qt::StrongFocus);
    setWindowIcon(QIcon(":/icons/mainicon.ico"));

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

void QtVGM::keyPressEvent(QKeyEvent *event) {
  playThread->postKeyCode(event->key());
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
