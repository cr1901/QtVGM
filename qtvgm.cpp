#include "qtvgm.h"
#include "./ui_qtvgm.h"

QtVGM::QtVGM(QWidget *parent)
    : QMainWindow(parent)
    , ui(new Ui::QtVGM)
{
    ui->setupUi(this);
}

QtVGM::~QtVGM()
{
    delete ui;
}

