#ifndef WAVEFORMBUILDER_H
#define WAVEFORMBUILDER_H

#include "AudioFileInfo.h"
#include "WaveformPeaks.h"

#include <QObject>

#include <memory>
#include <vector>

// Builds a file's waveform envelope off the GUI thread (add-media-preview D2).
// WAV PCM is read straight from the file on a pool thread; anything else is
// decoded by QAudioDecoder on a thread of its own. Either way the new buckets
// arrive every 100 ms through peaksAppended(), on the GUI thread.
//
// start() abandons the previous run; stale results never arrive. Destroying
// the builder cancels without waiting: the workers stop at their next check.
class WaveformBuilder : public QObject
{
    Q_OBJECT

public:
    explicit WaveformBuilder(QObject *parent = nullptr);
    ~WaveformBuilder() override;

    void start(const QString &path, const AudioFileInfo::Info &info);
    void cancel();
    bool isRunning() const { return m_running; }

    struct Channel;

signals:
    // A run started; previous peaks are void.
    void restarted();
    void peaksAppended(const std::vector<WaveformPeaks::Peak> &peaks);
    void finished(bool truncated);
    void failed(const QString &error);

private:
    friend struct Channel;
    std::shared_ptr<Channel> m_channel;
    bool m_running = false;
};

#endif // WAVEFORMBUILDER_H
