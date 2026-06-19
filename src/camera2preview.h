#pragma once
#include <QQuickFramebufferObject>
#include <QObject>

class Camera2Session;

// QML item that renders the Camera2 preview stream via AHardwareBuffer → EGL.
// Drop-in replacement for VideoOutput when using the Camera2 back camera.
// Usage in QML:
//   Camera2Preview { anchors.fill: parent; session: camera2session; displayRotation: 90 }
class Camera2Preview : public QQuickFramebufferObject
{
    Q_OBJECT
    Q_PROPERTY(Camera2Session* session READ session WRITE setSession NOTIFY sessionChanged)
    Q_PROPERTY(int displayRotation READ displayRotation WRITE setDisplayRotation NOTIFY displayRotationChanged)

public:
    explicit Camera2Preview(QQuickItem* parent = nullptr);

    Renderer* createRenderer() const override;

    Camera2Session* session() const { return m_session; }
    void setSession(Camera2Session* s);

    int displayRotation() const { return m_displayRotation; }
    void setDisplayRotation(int r) { if (m_displayRotation != r) { m_displayRotation = r; emit displayRotationChanged(); } }

signals:
    void sessionChanged();
    void displayRotationChanged();

private:
    Camera2Session* m_session       = nullptr;
    int             m_displayRotation = 90;
};
