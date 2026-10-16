#include "AudioView.h"

#include "MediaFormat.h"
#include "WaveformBuilder.h"
#include "WaveformView.h"

#include <QAudioOutput>
#include <QFile>
#include <QFileInfo>
#include <QKeyEvent>
#include <QLabel>
#include <QLocale>
#include <QMediaFormat>
#include <QMediaMetaData>
#include <QScrollBar>
#include <QSlider>
#include <QToolButton>
#include <QVBoxLayout>
#include <QtConcurrent>

#include <cmath>

namespace {

constexpr int kScrollSteps = 10000;
constexpr qint64 kSeekStepMs = 5000;


QString loopText(const AudioFileInfo::Info &info)
{
    if (!info.hasLoop || info.sampleRate <= 0)
        return {};
    return QCoreApplication::translate("AudioView", "loop %1–%2")
        .arg(MediaFormat::preciseTimeText(info.loopStart * 1000 / info.sampleRate),
             MediaFormat::preciseTimeText(info.loopEnd * 1000 / info.sampleRate));
}

} // namespace

AudioView::AudioView(MediaPlugin::Host *host, QWidget *parent)
    : QWidget(parent)
    , m_host(host)
{
    setFocusPolicy(Qt::StrongFocus);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    m_wave = new WaveformView(this);
    layout->addWidget(m_wave, 1);
    m_scroll = new QScrollBar(Qt::Horizontal, this);
    m_scroll->hide();
    layout->addWidget(m_scroll);

    m_player = new QMediaPlayer(this);
    m_output = new QAudioOutput(this);
    m_output->setVolume(MediaFormat::sessionVolume());
    m_player->setAudioOutput(m_output);
    m_builder = new WaveformBuilder(this);

    auto button = [this](const QString &text, const QString &tip, bool checkable = false) {
        return m_host->addToolButton(text, tip, checkable);
    };
    m_play = button(tr("Play"), tr("Play or pause (Space)"), false);
    m_stop = button(tr("Stop"), tr("Stop and return to the start (Home)"), false);
    m_loop = button(tr("Loop"), tr("Repeat the file"), true);
    m_host->addToolGap();
    m_time = new QLabel;
    m_time->setMinimumWidth(m_time->fontMetrics().horizontalAdvance(QStringLiteral("00:00:00.000 / 00:00:00")));
    m_host->addToolWidget(m_time);
    m_host->addToolGap();
    m_fit = button(tr("Fit"), tr("Show the whole file (double-click the waveform)"), false);
    m_host->addToolGap();
    m_volume = new QSlider(Qt::Horizontal);
    m_volume->setRange(0, 100);
    m_volume->setFixedWidth(90);
    m_volume->setToolTip(tr("Volume"));
    m_volume->setValue(qRound(QAudio::convertVolume(MediaFormat::sessionVolume(), QAudio::LinearVolumeScale,
                                                    QAudio::LogarithmicVolumeScale) * 100));
    m_host->addToolWidget(m_volume);

    connect(m_play, &QToolButton::clicked, this, &AudioView::togglePlay);
    connect(m_stop, &QToolButton::clicked, this, [this]() {
        m_player->stop();
        seek(0.0);
    });
    connect(m_loop, &QToolButton::toggled, this, [this](bool on) {
        m_player->setLoops(on ? QMediaPlayer::Infinite : QMediaPlayer::Once);
    });
    connect(m_fit, &QToolButton::clicked, m_wave, &WaveformView::fit);
    connect(m_volume, &QSlider::valueChanged, this, [this](int value) {
        const float linear = float(QAudio::convertVolume(value / 100.0, QAudio::LogarithmicVolumeScale,
                                                         QAudio::LinearVolumeScale));
        MediaFormat::sessionVolume() = linear;
        m_output->setVolume(linear);
    });

    connect(m_wave, &WaveformView::seekRequested, this, &AudioView::seek);
    connect(m_wave, &WaveformView::hoverChanged, this, [this](double s) {
        m_hover = s;
        showInfoLine();
    });
    connect(m_wave, &WaveformView::viewChanged, this, &AudioView::syncScrollBar);
    connect(m_scroll, &QScrollBar::valueChanged, this, [this](int value) {
        if (m_syncingScroll || m_wave->viewLength() <= 0.0)
            return;
        const double total = std::max(m_wave->duration(), 0.0);
        m_wave->setView(double(value) / kScrollSteps * total, m_wave->viewLength());
    });

    connect(m_builder, &WaveformBuilder::restarted, m_wave, &WaveformView::clear);
    connect(m_builder, &WaveformBuilder::peaksAppended, m_wave, &WaveformView::appendPeaks);
    connect(m_builder, &WaveformBuilder::finished, this, [this](bool truncated) {
        m_wave->setComplete(true);
        if (truncated)
            m_host->showBanner(MediaPlugin::Host::Banner::Neutral,
                               tr("The waveform stops after %1; playback covers the whole file.")
                                   .arg(MediaFormat::timeText(WaveformPeaks::kMaxBuckets * 1000
                                                              / WaveformPeaks::kBucketsPerSecond)));
    });
    connect(m_builder, &WaveformBuilder::failed, this, [this](const QString &error) {
        m_wave->setComplete(true);
        m_host->showBanner(MediaPlugin::Host::Banner::Warning, tr("No waveform: %1").arg(error));
    });

    connect(m_player, &QMediaPlayer::durationChanged, this, [this](qint64) {
        m_wave->setDuration(durationSeconds());
        updateTime(m_player->position());
        updateInfo();
    });
    connect(m_player, &QMediaPlayer::positionChanged, this, [this](qint64 ms) {
        m_reportedPosition = ms;
        m_sincePosition.restart();
        if (!m_tick.isActive()) {
            m_wave->setPosition(ms / 1000.0);
            updateTime(ms);
        }
    });
    connect(m_player, &QMediaPlayer::playbackStateChanged, this, [this](QMediaPlayer::PlaybackState state) {
        m_play->setText(state == QMediaPlayer::PlayingState ? tr("Pause") : tr("Play"));
        m_reportedPosition = m_player->position();
        m_sincePosition.restart();
        updateTicking();
    });
    connect(m_player, &QMediaPlayer::mediaStatusChanged, this, [this](QMediaPlayer::MediaStatus status) {
        if (status == QMediaPlayer::LoadedMedia && m_restorePosition >= 0) {
            m_player->setPosition(m_restorePosition);
            if (m_restorePlaying)
                m_player->play();
            m_restorePosition = -1;
        }
    });
    connect(m_player, &QMediaPlayer::metaDataChanged, this, &AudioView::updateInfo);
    connect(m_player, &QMediaPlayer::errorOccurred, this, [this](QMediaPlayer::Error, const QString &message) {
        m_host->showBanner(MediaPlugin::Host::Banner::Error, tr("Cannot play this file: %1").arg(message),
                           tr("Open as text"), [this]() { m_host->openAsText(); });
    });

    m_tick.setTimerType(Qt::PreciseTimer);
    m_tick.setInterval(16);
    connect(&m_tick, &QTimer::timeout, this, &AudioView::tick);
    connect(&m_infoWatcher, &QFutureWatcher<AudioFileInfo::Info>::finished, this, &AudioView::onInfoParsed);
}

AudioView::~AudioView()
{
    m_builder->cancel();
    m_player->stop();
}

void AudioView::load(const QString &path, bool reload)
{
    m_path = path;
    if (reload) {
        m_restorePosition = m_player->position();
        m_restorePlaying = m_player->playbackState() == QMediaPlayer::PlayingState;
    } else {
        m_restorePosition = -1;
    }
    m_host->hideBanner();
    // The player keeps its file open; release it first so a reload sees the new bytes.
    m_player->setSource(QUrl());
    if (!QFileInfo::exists(path)) {
        m_host->showBanner(MediaPlugin::Host::Banner::Error, tr("The file no longer exists."));
        return;
    }
    m_fileSize = QFileInfo(path).size();
    m_player->setSource(QUrl::fromLocalFile(path));
    m_builder->cancel();
    m_infoWatcher.setFuture(QtConcurrent::run([path]() {
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly))
            return AudioFileInfo::Info();
        return AudioFileInfo::parse(file);
    }));
}

void AudioView::onInfoParsed()
{
    m_info = m_infoWatcher.result();
    if (m_info.hasLoop && m_info.sampleRate > 0)
        m_wave->setLoop(double(m_info.loopStart) / m_info.sampleRate, double(m_info.loopEnd) / m_info.sampleRate);
    else
        m_wave->setLoop(0.0, 0.0);
    if (m_player->duration() <= 0 && m_info.durationMs > 0)
        m_wave->setDuration(m_info.durationMs / 1000.0);
    m_builder->start(m_path, m_info);
    updateInfo();
}

double AudioView::durationSeconds() const
{
    if (m_player->duration() > 0)
        return m_player->duration() / 1000.0;
    return m_info.durationMs > 0 ? m_info.durationMs / 1000.0 : 0.0;
}

void AudioView::togglePlay()
{
    if (m_player->playbackState() == QMediaPlayer::PlayingState)
        m_player->pause();
    else
        m_player->play();
}

void AudioView::seek(double seconds)
{
    const qint64 ms = qint64(std::llround(std::max(0.0, seconds) * 1000));
    m_player->setPosition(ms);
    m_reportedPosition = ms;
    m_sincePosition.restart();
    m_wave->setPosition(ms / 1000.0);
    updateTime(ms);
}

void AudioView::setTabVisible(bool visible)
{
    m_visible = visible;
    updateTicking();
}

void AudioView::updateTicking()
{
    const bool run = m_visible && m_player->playbackState() == QMediaPlayer::PlayingState;
    if (run && !m_tick.isActive())
        m_tick.start();
    else if (!run && m_tick.isActive())
        m_tick.stop();
    if (!run) {
        m_wave->setPosition(m_player->position() / 1000.0);
        updateTime(m_player->position());
    }
}

void AudioView::tick()
{
    // The player reports position every ~50 ms; advance in between by wall time.
    double ms = double(m_reportedPosition) + double(m_sincePosition.elapsed()) * m_player->playbackRate();
    const qint64 duration = m_player->duration();
    if (duration > 0)
        ms = std::min(ms, double(duration));
    m_wave->setPosition(ms / 1000.0);
    updateTime(qint64(ms));
}

void AudioView::updateTime(qint64 positionMs)
{
    const QString text = QStringLiteral("%1 / %2").arg(MediaFormat::preciseTimeText(positionMs),
                                                       MediaFormat::timeText(qint64(durationSeconds() * 1000)));
    if (m_time->text() != text)
        m_time->setText(text);
}

void AudioView::syncScrollBar()
{
    const double total = m_wave->duration();
    const bool zoomed = m_wave->viewLength() > 0.0 && total > 0.0;
    m_scroll->setVisible(zoomed);
    if (!zoomed)
        return;
    m_syncingScroll = true;
    const int page = std::max(1, int(m_wave->viewLength() / total * kScrollSteps));
    m_scroll->setRange(0, std::max(0, kScrollSteps - page));
    m_scroll->setPageStep(page);
    m_scroll->setSingleStep(std::max(1, page / 10));
    m_scroll->setValue(int(m_wave->viewStart() / total * kScrollSteps));
    m_syncingScroll = false;
}

void AudioView::updateInfo()
{
    const QLocale locale;
    QStringList parts;
    QString container = m_info.container;
    QString codec = m_info.codec;
    if (codec.isEmpty()) {
        const QVariant c = m_player->metaData().value(QMediaMetaData::AudioCodec);
        if (c.isValid())
            codec = QMediaFormat::audioCodecDescription(c.value<QMediaFormat::AudioCodec>());
    }
    if (container.isEmpty())
        container = QFileInfo(m_path).suffix().toUpper();
    parts << container;
    if (!codec.isEmpty() && codec != container) {
        if (m_info.bitsPerSample > 0 && (codec == QLatin1String("PCM") || codec == QLatin1String("FLAC")))
            codec += tr(" %1-bit").arg(m_info.bitsPerSample);
        parts << codec;
    } else if (m_info.bitsPerSample > 0) {
        parts << tr("%1-bit").arg(m_info.bitsPerSample);
    }
    if (m_info.sampleRate > 0)
        parts << tr("%1 Hz").arg(locale.toString(m_info.sampleRate));
    if (m_info.channels > 0)
        parts << MediaFormat::channelsText(m_info.channels);
    const double seconds = durationSeconds();
    if (seconds > 0)
        parts << MediaFormat::timeText(qint64(seconds * 1000));
    qint64 bitrate = m_info.bitrate;
    if (bitrate <= 0)
        bitrate = m_player->metaData().value(QMediaMetaData::AudioBitRate).toLongLong();
    if (bitrate > 0)
        parts << MediaFormat::bitrateText(bitrate);
    const QString loop = loopText(m_info);
    if (!loop.isEmpty())
        parts << loop;
    parts << MediaFormat::bytesText(m_fileSize);
    m_infoBase = parts.join(MediaFormat::separator());
    showInfoLine();

    // Tags go to the tooltip.
    const QMediaMetaData meta = m_player->metaData();
    QStringList tip;
    const QString title = !m_info.title.isEmpty() ? m_info.title : meta.stringValue(QMediaMetaData::Title);
    if (!title.isEmpty())
        tip << tr("Title: %1").arg(title);
    const std::pair<QMediaMetaData::Key, const char *> fields[] = {
        {QMediaMetaData::ContributingArtist, QT_TR_NOOP("Artist: %1")},
        {QMediaMetaData::AlbumTitle, QT_TR_NOOP("Album: %1")},
        {QMediaMetaData::Genre, QT_TR_NOOP("Genre: %1")},
        {QMediaMetaData::Date, QT_TR_NOOP("Date: %1")},
        {QMediaMetaData::Comment, QT_TR_NOOP("Comment: %1")},
        {QMediaMetaData::Copyright, QT_TR_NOOP("Copyright: %1")}};
    for (const auto &[key, label] : fields) {
        const QString value = meta.stringValue(key);
        if (!value.isEmpty())
            tip << tr(label).arg(value);
    }
    m_host->setInfoToolTip(tip.join(QLatin1Char('\n')));
}

void AudioView::showInfoLine()
{
    // Mouse moves only change the cursor time; the metadata part is cached.
    if (m_hover < 0.0)
        m_host->setInfoText(m_infoBase);
    else
        m_host->setInfoText(m_infoBase + MediaFormat::separator()
                            + tr("cursor %1").arg(MediaFormat::preciseTimeText(qint64(m_hover * 1000))));
}

void AudioView::themeChanged(const QPalette &palette, bool isDark)
{
    Q_UNUSED(isDark);
    m_wave->setPalette(palette);
    m_wave->update();
}

void AudioView::keyPressEvent(QKeyEvent *event)
{
    switch (event->key()) {
    case Qt::Key_Space:
        togglePlay();
        break;
    case Qt::Key_Left:
        seek((m_player->position() - kSeekStepMs) / 1000.0);
        break;
    case Qt::Key_Right:
        seek((m_player->position() + kSeekStepMs) / 1000.0);
        break;
    case Qt::Key_Home:
        seek(0.0);
        break;
    case Qt::Key_L:
        m_loop->toggle();
        break;
    default:
        QWidget::keyPressEvent(event);
        return;
    }
    event->accept();
}
