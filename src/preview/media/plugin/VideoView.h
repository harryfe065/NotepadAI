#ifndef VIDEOVIEW_H
#define VIDEOVIEW_H

#include "MediaPluginInterface.h"

#include <QMediaPlayer>
#include <QWidget>

class QAudioOutput;
class QComboBox;
class QLabel;
class QSlider;
class QToolButton;
class QVideoWidget;

// Video tab content (add-media-preview D4): QVideoWidget (GPU composited by
// the backend), a full-width timeline, frame steps, speed, loop and volume.
// Playback pauses when the tab is hidden.
class VideoView : public QWidget, public MediaPlugin::View
{
    Q_OBJECT

public:
    VideoView(MediaPlugin::Host *host, QWidget *parent = nullptr);
    ~VideoView() override;

    QWidget *widget() override { return this; }
    void load(const QString &path, bool reload) override;
    void setTabVisible(bool visible) override;
    void themeChanged(const QPalette &palette, bool isDark) override;

protected:
    void keyPressEvent(QKeyEvent *event) override;

private:
    void togglePlay();
    void stepFrame(int direction);
    void seek(qint64 ms);
    double frameRate() const;
    void updateTime(qint64 ms);
    void updateInfo();

    MediaPlugin::Host *m_host;
    QString m_path;
    qint64 m_fileSize = 0;
    QMediaPlayer *m_player = nullptr;
    QAudioOutput *m_output = nullptr;
    QVideoWidget *m_video = nullptr;
    QSlider *m_timeline = nullptr;
    QLabel *m_time = nullptr;

    QToolButton *m_play = nullptr;
    QToolButton *m_prevFrame = nullptr;
    QToolButton *m_nextFrame = nullptr;
    QComboBox *m_speed = nullptr;
    QToolButton *m_loop = nullptr;
    QToolButton *m_mute = nullptr;
    QSlider *m_volume = nullptr;

    qint64 m_restorePosition = -1;
};

#endif // VIDEOVIEW_H
