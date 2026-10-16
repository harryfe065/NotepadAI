#ifndef FILEKINDRESOLVER_H
#define FILEKINDRESOLVER_H

#include <QByteArrayView>
#include <QHash>
#include <QString>
#include <QVector>

#include <functional>

// Decides how a file opens: in a preview tab of a registered type, as an
// unrecognised binary, or as text in the editor. Every open surface asks this
// one resolver, so a tree click, Ctrl+P, a drop and the CLI always agree.
//
// Evidence is checked cheapest first: a registered preview extension or a
// known text extension decides without touching the file; only an unknown or
// missing extension reads one head of the file (kHeadBytes) for magic bytes and
// a NUL scan.
class FileKindResolver
{
public:
    enum class Kind : quint8 { Text, Preview, Binary };

    struct Result {
        Kind kind = Kind::Text;
        QString typeId;  // set for Kind::Preview
    };

    // Each sniffer looks at the head only. Predicates (not plain signatures) so
    // weak magics such as BMP's "BM" can check more of the header.
    using Sniffer = std::function<bool(QByteArrayView head)>;

    static constexpr qsizetype kHeadBytes = 8192;

    void addPreviewExtensions(const QString &typeId, const QStringList &extensions);
    void addSniffer(const QString &typeId, Sniffer sniffer);
    void setTextExtensionProbe(std::function<bool(const QString &extLower)> probe);

    // Extension-only decision. Returns Kind::Text with an empty typeId when the
    // extension is not a registered preview type; `known` reports whether the
    // extension alone settled it (preview or known text).
    Result resolveByExtension(const QString &suffixLower, bool *known) const;

    // Full decision for a local file (reads at most kHeadBytes when needed).
    Result resolve(const QString &filePath) const;

    // Pure head classification (magic, BOM, NUL). Public for tests.
    Result classifyHead(QByteArrayView head) const;

private:
    QHash<QString, QString> m_previewByExt;
    QVector<QPair<QString, Sniffer>> m_sniffers;
    std::function<bool(const QString &)> m_isTextExtension;
};

#endif // FILEKINDRESOLVER_H
