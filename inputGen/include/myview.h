#ifndef MYVIEW_H
#define MYVIEW_H

#include <QGraphicsView>

#include "primitive.h"
#include "types.h"

class MyScene;

class MyView : public QGraphicsView
{
    Q_OBJECT
public:

    explicit MyView(QWidget *parent = 0);
    void setPrimitives(std::vector<InputGen::Application::Primitive> *s);

signals:
    void samplesChanged(InputGen::Application::PointSet *set);

public slots:

};

#endif // MYVIEW_H
