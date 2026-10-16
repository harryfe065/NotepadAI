#ifndef AUDIOVIEW_H
#define AUDIOVIEW_H

#include "AudioFileInfo.h"
#include "MediaPluginInterface.h"

#include <QElapsedTimer>
#include <QFutureWatcher>
#include <QMediaPlayer>
#include <QTimer>
#include <QWidget>

class QAudioOutput;
class QLabel;
class QScrollBar;
class QSlider;
class QToolButton;
class WaveformBuilder;
class WaveformView;

// Audio tab content (add-media-preview): waveform with seek, zoom and loop
// points; play / pause / stop / loop / volume; metadata from the file's own
// headers (AudioFileInfo) completed by Qt's.
//
// The playhead is interpolated between the player's coarse position updates
// by a frame timer that runs only while playing and visible, and each tick
// repaints only the columns the playhead crossed.
class AudioView : public QWidget, public MediaPlugin::View
{
    Q_OBJECT

public:
    AudioView(MediaPlugin::Host *host, QWidget *parent = nullptr);
    ~AudioView() override;

    QWidget *widget() override { return this; }
    void load(const QString &path, bool reload) override;
    void setTabVisible(bool visible) override;
    void themeChanged(const QPalette &palette, bool isDark) override;

protected:
    void keyPressEvent(QKeyEvent *event) override;

private:
    void onInfoParsed();
    void togglePlay();
    void seek(double seconds);
    void syncScrollBar();
    void updateTicking();
    void tick();
    void updateTime(qint64 positionMs);
    void updateInfo();
    void showInfoLine();
    double durationSeconds() const;

    MediaPlugin::Host *m_host;
    QString m_path;
    QMediaPlayer *m_player = nullptr;
    QAudioOutput *m_output = nullptr;
    WaveformView *m_wave = nullptr;
    QScrollBar *m_scroll = nullptr;
    WaveformBuilder *m_builder = nullptr;
    QFutureWatcher<AudioFileInfo::Info> m_infoWatcher;
    AudioFileInfo::Info m_info;
    qint64 m_fileSize = 0;

    QToolButton *m_play = nullptr;
    QToolButton *m_stop = nullptr;
    QToolButton *m_loop = nullptr;
    QToolButton *m_fit = nullptr;
    QLabel *m_time = nullptr;
    QSlider *m_volume = nullptr;

    QTimer m_tick;
    QElapsedTimer m_sincePosition;
    qint64 m_reportedPosition = 0;
    bool m_visible = false;
    bool m_syncingScroll = false;
    double m_hover = -1.0;
    QString m_infoBase;     // info line without the cursor time
    qint64 m_restorePosition = -1;
    bool m_restorePlaying = false;
};

#endif // AUDIOVIEW_H
