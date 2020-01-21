#ifndef PLAYTHREAD_HPP
#define PLAYTHREAD_HPP

#include <QObject>
#include <QThread>


class PlayThread : public QThread
{
    Q_OBJECT

    const char * test[1] = { "Test" };

    void run() override {

      emit newSong(test);
    }

    signals:
      void newSong(const char* const* tagList);
};

#endif
