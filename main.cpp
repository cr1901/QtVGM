#include "qtvgm.h"

#include <QApplication>

int main(int argc, char *argv[])
{
    QApplication a(argc, argv);
    QtVGM w;
    w.show();
    return a.exec();
}
