// SPDX-License-Identifier: GPL-2.0-only

#ifndef ACCELREADER_H
#define ACCELREADER_H

#include <QObject>
#include <QTimer>
#include <QDBusInterface>

class AccelReader : public QObject
{
    Q_OBJECT
    Q_PROPERTY(double x READ x NOTIFY readingChanged)
    Q_PROPERTY(double y READ y NOTIFY readingChanged)
    Q_PROPERTY(double z READ z NOTIFY readingChanged)
    Q_PROPERTY(bool active READ active WRITE setActive NOTIFY activeChanged)

public:
    explicit AccelReader(QObject *parent = nullptr);
    ~AccelReader();

    double x() const { return m_x; }
    double y() const { return m_y; }
    double z() const { return m_z; }
    bool active() const { return m_active; }
    void setActive(bool active);

signals:
    void readingChanged();
    void activeChanged();

private slots:
    void readSensor();

private:
    void startSensor();
    void stopSensor();

    QTimer m_timer;
    QDBusInterface *m_sensorIface;
    double m_x = 0;
    double m_y = 0;
    double m_z = 0;
    bool m_active = false;
    int m_sessionId;
};

#endif // ACCELREADER_H
