#ifndef MEDIASNIFFERS_H
#define MEDIASNIFFERS_H

#include <QByteArrayView>

// Content sniffers for files whose extension is missing or unknown
// (FileKindResolver). Each looks at the head only. Weak signatures (a bare
// MPEG frame sync, ADTS) are left to the extension.
namespace MediaSniffers {

bool isAudio(QByteArrayView head);
bool isVideo(QByteArrayView head);
bool isPdf(QByteArrayView head);

} // namespace MediaSniffers

#endif // MEDIASNIFFERS_H
