#include "qtvgm.h"

#include <QApplication>

int main(int argc, char *argv[])
{
    if(argc < 2) {
      return 1;
    }

    QApplication a(argc, argv);
    QtVGM w;
    w.show();
    w.startPlayer(argv[1]);
    return a.exec();
}
