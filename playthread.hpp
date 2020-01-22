#ifndef PLAYTHREAD_HPP
#define PLAYTHREAD_HPP

#include <QObject>
#include <QThread>
#include <player/vgmplayer.hpp>


class PlayThread : public QThread
{
    Q_OBJECT
    VGMPlayer * player;
    const char * m3uFile = NULL;
    char infoBuf[1024];

    void run() override;

    signals:
      void newSong(const char * tagList);

    public:
      PlayThread();
      void setM3u(const char * fileName);
};

#endif
