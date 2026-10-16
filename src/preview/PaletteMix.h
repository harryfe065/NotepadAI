#ifndef PALETTEMIX_H
#define PALETTEMIX_H

#include <QColor>

// Linear blend of two palette colours (t = 0: a, t = 1: b). The asset viewers
// derive their muted / accent tones from the live palette with it, so both
// themes stay readable without hard-coded colours (ui-dna).
inline QColor paletteMix(const QColor &a, const QColor &b, double t)
{
    return QColor::fromRgbF(float(a.redF() * (1 - t) + b.redF() * t), float(a.greenF() * (1 - t) + b.greenF() * t),
                            float(a.blueF() * (1 - t) + b.blueF() * t));
}

#endif // PALETTEMIX_H
