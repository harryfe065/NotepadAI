#include "WaveformBuilder.h"

#include <QAudioBuffer>
#include <QAudioDecoder>
#include <QCoreApplication>
#include <QDeadlineTimer>
#include <QElapsedTimer>
#include <QFile>
#include <QMutex>
#include <QPointer>
#include <QSet>
#include <QThread>
#include <QTimer>
#include <QUrl>
#include <QtConcurrent>

#include <atomic>

// One run's link between its worker and the builder. The worker posts through
// it under the mutex; the builder clears `receiver` under the same mutex, so a
// post never targets a builder being destroyed. Posts still queued when the
// builder dies are discarded by Qt with it.
struct WaveformBuilder::Channel {
    std::atomic_bool cancelled{false};
    QMutex mutex;
    WaveformBuilder *receiver = nullptr;

    template <typename F>
    void post(F &&f)
    {
        const QMutexLocker lock(&mutex);
        if (receiver)
            QMetaObject::invokeMethod(receiver, std::forward<F>(f), Qt::QueuedConnection);
    }

    void detach()
    {
        cancelled.store(true);
        const QMutexLocker lock(&mutex);
        receiver = nullptr;
    }
};

namespace {

using Channel = WaveformBuilder::Channel;

constexpr qint64 kPublishMs = 100;
constexpr qint64 kReadBlock = 1 << 20;

// Hands the buckets finished since the last call to the GUI thread.
class Publisher
{
public:
    Publisher(std::shared_ptr<Channel> channel) : m_channel(std::move(channel)) { m_timer.start(); }

    void publish(const WaveformPeaks &peaks, bool force)
    {
        if (!force && m_timer.elapsed() < kPublishMs)
            return;
        m_timer.restart();
        if (peaks.size() <= m_sent)
            return;
        std::vector<WaveformPeaks::Peak> slice(peaks.peaks().begin() + m_sent, peaks.peaks().end());
        m_sent = peaks.size();
        const std::shared_ptr<Channel> channel = m_channel;
        m_channel->post([channel, slice = std::move(slice)]() {
            if (WaveformBuilder *b = channel->receiver)
                emit b->peaksAppended(slice);
        });
    }

    void finish(const WaveformPeaks &peaks)
    {
        publish(peaks, true);
        const std::shared_ptr<Channel> channel = m_channel;
        const bool truncated = peaks.truncated();
        m_channel->post([channel, truncated]() {
            if (WaveformBuilder *b = channel->receiver)
                emit b->finished(truncated);
        });
    }

    void fail(const QString &error)
    {
        const std::shared_ptr<Channel> channel = m_channel;
        m_channel->post([channel, error]() {
            if (WaveformBuilder *b = channel->receiver)
                emit b->failed(error);
        });
    }

private:
    std::shared_ptr<Channel> m_channel;
    QElapsedTimer m_timer;
    qsizetype m_sent = 0;
};

// Raw little-endian PCM straight from the file: the cost is the read.
void scanPcm(const QString &path, AudioFileInfo::Info info, const std::shared_ptr<Channel> &channel)
{
    Publisher publisher(channel);
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly) || !file.seek(info.dataOffset)) {
        publisher.fail(file.errorString());
        return;
    }
    WaveformPeaks peaks;
    peaks.reset(info.sampleRate, info.channels);
    const qint64 frameBytes = qint64(info.channels) * WaveformPeaks::bytesPerSample(info.pcm);
    const qint64 blockBytes = std::max<qint64>(1, kReadBlock / frameBytes) * frameBytes;
    std::vector<char> block(static_cast<size_t>(blockBytes));
    qint64 left = info.dataBytes - info.dataBytes % frameBytes;
    while (left > 0 && !channel->cancelled.load(std::memory_order_relaxed) && !peaks.truncated()) {
        const qint64 got = file.read(block.data(), std::min(left, blockBytes));
        if (got < frameBytes)
            break;
        peaks.add(block.data(), info.pcm, got / frameBytes);
        left -= got;
        publisher.publish(peaks, false);
    }
    if (channel->cancelled.load())
        return;
    peaks.finish();
    publisher.finish(peaks);
}

// Lives on its own thread with the QAudioDecoder, so neither decoding nor
// the envelope ever runs on the GUI thread.
class DecodeWorker : public QObject
{
public:
    DecodeWorker(QString path, std::shared_ptr<Channel> channel)
        : m_path(std::move(path)), m_channel(channel), m_publisher(std::move(channel))
    {
    }

    void run()
    {
        m_decoder = new QAudioDecoder(this);
        connect(m_decoder, &QAudioDecoder::bufferReady, this, &DecodeWorker::onBuffer);
        connect(m_decoder, &QAudioDecoder::finished, this, [this]() { done(true); });
        connect(m_decoder, QOverload<QAudioDecoder::Error>::of(&QAudioDecoder::error), this, [this]() {
            // Buckets already made stay; an error before any is a failure.
            if (m_started)
                done(true);
            else
                fail(m_decoder->errorString());
        });
        // Cancellation is polled: the GUI thread never touches this object.
        auto *poll = new QTimer(this);
        connect(poll, &QTimer::timeout, this, [this]() {
            if (m_channel->cancelled.load())
                done(false);
        });
        poll->start(50);
        m_decoder->setSource(QUrl::fromLocalFile(m_path));
        m_decoder->start();
    }

private:
    void onBuffer()
    {
        const QAudioBuffer buffer = m_decoder->read();
        if (!buffer.isValid() || m_finished)
            return;
        const QAudioFormat format = buffer.format();
        WaveformPeaks::Format sampleFormat;
        switch (format.sampleFormat()) {
        case QAudioFormat::UInt8: sampleFormat = WaveformPeaks::Format::U8; break;
        case QAudioFormat::Int16: sampleFormat = WaveformPeaks::Format::S16; break;
        case QAudioFormat::Int32: sampleFormat = WaveformPeaks::Format::S32; break;
        case QAudioFormat::Float: sampleFormat = WaveformPeaks::Format::F32; break;
        default: return;
        }
        if (!m_started) {
            m_started = true;
            m_peaks.reset(format.sampleRate(), format.channelCount());
        }
        m_peaks.add(buffer.constData<char>(), sampleFormat, buffer.frameCount());
        m_publisher.publish(m_peaks, false);
        if (m_peaks.truncated())
            done(true);
    }

    void done(bool publish)
    {
        if (m_finished)
            return;
        m_finished = true;
        m_decoder->stop();
        if (publish && !m_channel->cancelled.load()) {
            m_peaks.finish();
            m_publisher.finish(m_peaks);
        }
        thread()->quit();
    }

    void fail(const QString &error)
    {
        if (m_finished)
            return;
        m_finished = true;
        m_decoder->stop();
        m_publisher.fail(error);
        thread()->quit();
    }

    QString m_path;
    std::shared_ptr<Channel> m_channel;
    Publisher m_publisher;
    QAudioDecoder *m_decoder = nullptr;
    WaveformPeaks m_peaks;
    bool m_started = false;
    bool m_finished = false;
};

// Decoder threads still running. On quit all are told to stop at once and
// joined against one shared deadline, so quitting never waits per thread.
QSet<QThread *> &liveThreads()
{
    static QSet<QThread *> threads;
    return threads;
}

void joinThreadsOnQuit()
{
    static bool connected = false;
    if (connected)
        return;
    connected = true;
    QObject::connect(qApp, &QCoreApplication::aboutToQuit, qApp, []() {
        for (QThread *thread : std::as_const(liveThreads()))
            thread->quit();
        QDeadlineTimer deadline(2000);
        for (QThread *thread : std::as_const(liveThreads()))
            thread->wait(deadline);
        // A decoder stuck in FFmpeg past the deadline is left to process exit.
    });
}

} // namespace

WaveformBuilder::WaveformBuilder(QObject *parent)
    : QObject(parent)
{
    connect(this, &WaveformBuilder::finished, this, [this]() { m_running = false; });
    connect(this, &WaveformBuilder::failed, this, [this]() { m_running = false; });
}

WaveformBuilder::~WaveformBuilder()
{
    cancel();
}

void WaveformBuilder::cancel()
{
    if (m_channel)
        m_channel->detach();
    m_channel.reset();
    m_running = false;
}

void WaveformBuilder::start(const QString &path, const AudioFileInfo::Info &info)
{
    cancel();
    m_channel = std::make_shared<Channel>();
    m_channel->receiver = this;
    m_running = true;
    emit restarted();

    const std::shared_ptr<Channel> channel = m_channel;

    if (info.pcmFastPath()) {
        QThreadPool::globalInstance()->start([path, info, channel]() { scanPcm(path, info, channel); });
        return;
    }

    joinThreadsOnQuit();
    auto *thread = new QThread;
    thread->setObjectName(QStringLiteral("WaveformDecoder"));
    auto *worker = new DecodeWorker(path, channel);
    worker->moveToThread(thread);
    connect(thread, &QThread::started, worker, [worker]() { worker->run(); });
    connect(thread, &QThread::finished, worker, &QObject::deleteLater);
    connect(thread, &QThread::finished, qApp, [thread]() {
        liveThreads().remove(thread);
        thread->deleteLater();
    });
    liveThreads().insert(thread);
    thread->start(QThread::LowPriority);
}
