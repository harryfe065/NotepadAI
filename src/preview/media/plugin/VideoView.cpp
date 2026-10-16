#include "VideoView.h"

#include "MediaFormat.h"

#include <QAudioOutput>
#include <QComboBox>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLocale>
#include <QMediaFormat>
#include <QMediaMetaData>
#include <QSlider>
#include <QToolButton>
#include <QVBoxLayout>
#include <QVideoWidget>

#include <algorithm>
#include <climits>
#include <cmath>

namespace {

constexpr double kFallbackFps = 30.0;
constexpr qint64 kSeekStepMs = 5000;


} // namespace

VideoView::VideoView(MediaPlugin::Host *host, QWidget *parent)
    : QWidget(parent)
    , m_host(host)
{
    setFocusPolicy(Qt::StrongFocus);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    m_video = new QVideoWidget(this);
    m_video->setFocusPolicy(Qt::NoFocus);
    layout->addWidget(m_video, 1);

    auto *bar = new QWidget(this);
    auto *barLayout = new QHBoxLayout(bar);
    barLayout->setContentsMargins(8, 4, 8, 4);
    barLayout->setSpacing(8);
    m_timeline = new QSlider(Qt::Horizontal, bar);
    m_timeline->setToolTip(tr("Position"));
    m_timeline->setFocusPolicy(Qt::NoFocus);
    barLayout->addWidget(m_timeline, 1);
    m_time = new QLabel(bar);
    m_time->setMinimumWidth(m_time->fontMetrics().horizontalAdvance(QStringLiteral("00:00:00.000 / 00:00:00")));
    barLayout->addWidget(m_time);
    layout->addWidget(bar);

    m_player = new QMediaPlayer(this);
    m_output = new QAudioOutput(this);
    m_output->setVolume(MediaFormat::sessionVolume());
    m_player->setAudioOutput(m_output);
    m_player->setVideoOutput(m_video);

    auto button = [this](const QString &text, const QString &tip, bool checkable = false) {
        return m_host->addToolButton(text, tip, checkable);
    };
    m_play = button(tr("Play"), tr("Play or pause (Space)"), false);
    m_prevFrame = button(tr("◀ Frame"), tr("Previous frame (,)"), false);
    m_nextFrame = button(tr("Frame ▶"), tr("Next frame (.)"), false);
    m_host->addToolGap();
    m_speed = new QComboBox;
    m_speed->setToolTip(tr("Playback speed"));
    for (double rate : {0.25, 0.5, 0.75, 1.0, 1.25, 1.5, 2.0})
        m_speed->addItem(QStringLiteral("%1×").arg(rate), rate);
    m_speed->setCurrentIndex(3);
    m_host->addToolWidget(m_speed);
    m_loop = button(tr("Loop"), tr("Repeat the video (L)"), true);
    m_host->addToolGap();
    m_mute = button(tr("Mute"), tr("Mute (M)"), true);
    m_volume = new QSlider(Qt::Horizontal);
    m_volume->setRange(0, 100);
    m_volume->setFixedWidth(90);
    m_volume->setToolTip(tr("Volume"));
    m_volume->setValue(qRound(QAudio::convertVolume(MediaFormat::sessionVolume(), QAudio::LinearVolumeScale,
                                                    QAudio::LogarithmicVolumeScale) * 100));
    m_host->addToolWidget(m_volume);

    connect(m_play, &QToolButton::clicked, this, &VideoView::togglePlay);
    connect(m_prevFrame, &QToolButton::clicked, this, [this]() { stepFrame(-1); });
    connect(m_nextFrame, &QToolButton::clicked, this, [this]() { stepFrame(1); });
    connect(m_speed, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this]() {
        m_player->setPlaybackRate(m_speed->currentData().toDouble());
    });
    connect(m_loop, &QToolButton::toggled, this, [this](bool on) {
        m_player->setLoops(on ? QMediaPlayer::Infinite : QMediaPlayer::Once);
    });
    connect(m_mute, &QToolButton::toggled, m_output, &QAudioOutput::setMuted);
    connect(m_volume, &QSlider::valueChanged, this, [this](int value) {
        const float linear = float(QAudio::convertVolume(value / 100.0, QAudio::LogarithmicVolumeScale,
                                                         QAudio::LinearVolumeScale));
        MediaFormat::sessionVolume() = linear;
        m_output->setVolume(linear);
    });
    connect(m_timeline, &QSlider::sliderMoved, this, [this](int value) { seek(value); });
    connect(m_timeline, &QSlider::actionTriggered, this, [this](int action) {
        // Page clicks on the groove jump; the slider's own value is applied after this signal.
        if (action == QAbstractSlider::SliderPageStepAdd || action == QAbstractSlider::SliderPageStepSub)
            seek(m_timeline->sliderPosition());
    });

    connect(m_player, &QMediaPlayer::durationChanged, this, [this](qint64 duration) {
        m_timeline->setRange(0, int(std::min<qint64>(duration, INT_MAX)));
        m_timeline->setPageStep(int(std::clamp<qint64>(duration / 20, 1000, 60000)));
        updateTime(m_player->position());
        updateInfo();
    });
    connect(m_player, &QMediaPlayer::positionChanged, this, [this](qint64 ms) {
        if (!m_timeline->isSliderDown())
            m_timeline->setValue(int(std::min<qint64>(ms, INT_MAX)));
        updateTime(ms);
    });
    connect(m_player, &QMediaPlayer::playbackStateChanged, this, [this](QMediaPlayer::PlaybackState state) {
        m_play->setText(state == QMediaPlayer::PlayingState ? tr("Pause") : tr("Play"));
    });
    connect(m_player, &QMediaPlayer::mediaStatusChanged, this, [this](QMediaPlayer::MediaStatus status) {
        if (status == QMediaPlayer::LoadedMedia && m_restorePosition >= 0) {
            m_player->setPosition(m_restorePosition);
            m_restorePosition = -1;
        }
        if (status == QMediaPlayer::LoadedMedia && !m_player->hasVideo())
            m_host->showBanner(MediaPlugin::Host::Banner::Neutral, tr("This file has no video track."));
    });
    connect(m_player, &QMediaPlayer::metaDataChanged, this, &VideoView::updateInfo);
    connect(m_player, &QMediaPlayer::tracksChanged, this, &VideoView::updateInfo);
    connect(m_player, &QMediaPlayer::errorOccurred, this, [this](QMediaPlayer::Error, const QString &message) {
        m_host->showBanner(MediaPlugin::Host::Banner::Error, tr("Cannot play this file: %1").arg(message),
                           tr("Open as text"), [this]() { m_host->openAsText(); });
    });
}

VideoView::~VideoView()
{
    m_player->stop();
}

void VideoView::load(const QString &path, bool reload)
{
    m_path = path;
    m_restorePosition = reload ? m_player->position() : -1;
    m_host->hideBanner();
    // Release the old file first so a reload reopens the new bytes.
    m_player->setSource(QUrl());
    if (!QFileInfo::exists(path)) {
        m_host->showBanner(MediaPlugin::Host::Banner::Error, tr("The file no longer exists."));
        return;
    }
    m_fileSize = QFileInfo(path).size();
    m_player->setSource(QUrl::fromLocalFile(path));
    updateInfo();
}

void VideoView::setTabVisible(bool visible)
{
    // A hidden video costs a decoder and a GPU surface; the user resumes.
    if (!visible && m_player->playbackState() == QMediaPlayer::PlayingState)
        m_player->pause();
}

void VideoView::themeChanged(const QPalette &palette, bool isDark)
{
    Q_UNUSED(palette);
    Q_UNUSED(isDark);
}

void VideoView::togglePlay()
{
    if (m_player->playbackState() == QMediaPlayer::PlayingState)
        m_player->pause();
    else
        m_player->play();
}

double VideoView::frameRate() const
{
    const double fps = m_player->metaData().value(QMediaMetaData::VideoFrameRate).toDouble();
    return fps > 0.0 && fps < 1000.0 ? fps : kFallbackFps;
}

void VideoView::stepFrame(int direction)
{
    // No frame-accurate API: pause and move one frame duration. The backend
    // decodes the target frame and shows it while paused.
    if (m_player->playbackState() == QMediaPlayer::PlayingState)
        m_player->pause();
    const qint64 frameMs = std::max<qint64>(1, qint64(std::llround(1000.0 / frameRate())));
    seek(m_player->position() + direction * frameMs);
}

void VideoView::seek(qint64 ms)
{
    const qint64 duration = m_player->duration();
    ms = std::max<qint64>(0, duration > 0 ? std::min(ms, duration) : ms);
    m_player->setPosition(ms);
    updateTime(ms);
}

void VideoView::updateTime(qint64 ms)
{
    const QString text = QStringLiteral("%1 / %2").arg(MediaFormat::preciseTimeText(ms),
                                                       MediaFormat::timeText(m_player->duration()));
    if (m_time->text() != text)
        m_time->setText(text);
}

void VideoView::updateInfo()
{
    const QMediaMetaData meta = m_player->metaData();
    const QLocale locale;
    QStringList parts;
    const QVariant format = meta.value(QMediaMetaData::FileFormat);
    QString container = format.isValid() ? QMediaFormat::fileFormatName(format.value<QMediaFormat::FileFormat>())
                                         : QString();
    if (container.isEmpty() || container == QLatin1String("Unspecified"))
        container = QFileInfo(m_path).suffix().toUpper();
    parts << container;
    const QVariant videoCodec = meta.value(QMediaMetaData::VideoCodec);
    if (videoCodec.isValid())
        parts << QMediaFormat::videoCodecName(videoCodec.value<QMediaFormat::VideoCodec>());
    const QSize resolution = meta.value(QMediaMetaData::Resolution).toSize();
    if (resolution.isValid())
        parts << QStringLiteral("%1 × %2").arg(resolution.width()).arg(resolution.height());
    const double fps = meta.value(QMediaMetaData::VideoFrameRate).toDouble();
    if (fps > 0.0)
        parts << tr("%1 fps").arg(locale.toString(fps, 'g', 4));
    const QVariant audioCodec = meta.value(QMediaMetaData::AudioCodec);
    if (audioCodec.isValid() && audioCodec.value<QMediaFormat::AudioCodec>() != QMediaFormat::AudioCodec::Unspecified)
        parts << QMediaFormat::audioCodecName(audioCodec.value<QMediaFormat::AudioCodec>());
    const qint64 bitrate = meta.value(QMediaMetaData::VideoBitRate).toLongLong()
                           + meta.value(QMediaMetaData::AudioBitRate).toLongLong();
    if (bitrate > 0)
        parts << MediaFormat::bitrateText(bitrate);
    if (m_player->duration() > 0)
        parts << MediaFormat::timeText(m_player->duration());
    const int audioTracks = int(m_player->audioTracks().size());
    if (audioTracks > 1)
        parts << tr("%n audio tracks", nullptr, audioTracks);
    parts << MediaFormat::bytesText(m_fileSize);
    m_host->setInfoText(parts.join(MediaFormat::separator()));

    QStringList tip;
    for (QMediaMetaData::Key key : {QMediaMetaData::Title, QMediaMetaData::Author, QMediaMetaData::Date,
                                    QMediaMetaData::Comment, QMediaMetaData::Copyright}) {
        const QString value = meta.stringValue(key);
        if (!value.isEmpty())
            tip << QStringLiteral("%1: %2").arg(QMediaMetaData::metaDataKeyToString(key), value);
    }
    m_host->setInfoToolTip(tip.join(QLatin1Char('\n')));
}

void VideoView::keyPressEvent(QKeyEvent *event)
{
    switch (event->key()) {
    case Qt::Key_Space:
        togglePlay();
        break;
    case Qt::Key_Comma:
        stepFrame(-1);
        break;
    case Qt::Key_Period:
        stepFrame(1);
        break;
    case Qt::Key_Left:
        seek(m_player->position() - kSeekStepMs);
        break;
    case Qt::Key_Right:
        seek(m_player->position() + kSeekStepMs);
        break;
    case Qt::Key_Home:
        seek(0);
        break;
    case Qt::Key_L:
        m_loop->toggle();
        break;
    case Qt::Key_M:
        m_mute->toggle();
        break;
    default:
        QWidget::keyPressEvent(event);
        return;
    }
    event->accept();
}
