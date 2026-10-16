// The NotepadAIMedia plugin end to end (add-media-preview D1): loaded through
// QPluginLoader like the app does, its views driven through a fake Host.
// Covers the plugin metadata, the factory, the audio info line (the file's
// own headers) and the PDF view's in-memory load. Offscreen; no audio device
// is needed because the info line comes from the file, not the player.

#include "MediaPluginInterface.h"

#include <QPluginLoader>
#include <QTemporaryDir>
#include <QToolButton>
#include <QtEndian>
#include <QtTest>

#include <memory>

namespace {

class FakeHost final : public MediaPlugin::Host
{
public:
    QWidget toolbar;
    QString info;
    QString banner;
    int toolWidgets = 0;

    void addToolWidget(QWidget *widget) override
    {
        widget->setParent(&toolbar);
        ++toolWidgets;
    }
    QToolButton *addToolButton(const QString &text, const QString &, bool checkable) override
    {
        auto *b = new QToolButton(&toolbar);
        b->setText(text);
        b->setCheckable(checkable);
        ++toolWidgets;
        return b;
    }
    void addToolGap() override {}
    void setInfoText(const QString &text) override { info = text; }
    void setInfoToolTip(const QString &) override {}
    void showBanner(Banner, const QString &text, const QString &, std::function<void()>) override { banner = text; }
    void hideBanner() override { banner.clear(); }
    void openAsText() override {}
};

void le16(QByteArray &b, quint16 v) { char c[2]; qToLittleEndian(v, c); b.append(c, 2); }
void le32(QByteArray &b, quint32 v) { char c[4]; qToLittleEndian(v, c); b.append(c, 4); }

// One second of 24-bit stereo silence at 48 kHz with a loop.
QByteArray wavFile()
{
    QByteArray fmt;
    le16(fmt, 1);
    le16(fmt, 2);
    le32(fmt, 48000);
    le32(fmt, 48000 * 6);
    le16(fmt, 6);
    le16(fmt, 24);
    QByteArray smpl(36, '\0');
    smpl[28] = 1;                       // one loop
    le32(smpl, 0);
    le32(smpl, 0);
    le32(smpl, 24000);
    le32(smpl, 47999);
    le32(smpl, 0);
    le32(smpl, 0);
    QByteArray body("WAVE");
    body += "fmt ";
    le32(body, quint32(fmt.size()));
    body += fmt;
    body += "data";
    le32(body, 48000 * 6);
    body += QByteArray(48000 * 6, '\0');
    body += "smpl";
    le32(body, quint32(smpl.size()));
    body += smpl;
    QByteArray out("RIFF");
    le32(out, quint32(body.size()));
    return out + body;
}

// A one-page A4 PDF with a correct cross-reference table.
QByteArray pdfFile()
{
    QByteArray pdf("%PDF-1.4\n");
    QList<qsizetype> offsets;
    const char *objects[] = {"<< /Type /Catalog /Pages 2 0 R >>", "<< /Type /Pages /Kids [3 0 R] /Count 1 >>",
                             "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 595 842] >>"};
    for (int i = 0; i < 3; ++i) {
        offsets << pdf.size();
        pdf += QByteArray::number(i + 1) + " 0 obj\n" + objects[i] + "\nendobj\n";
    }
    const qsizetype xref = pdf.size();
    pdf += "xref\n0 4\n0000000000 65535 f \n";
    for (qsizetype offset : offsets)
        pdf += QByteArray::number(offset).rightJustified(10, '0') + " 00000 n \n";
    pdf += "trailer\n<< /Size 4 /Root 1 0 R >>\nstartxref\n" + QByteArray::number(xref) + "\n%%EOF\n";
    return pdf;
}

QString writeFile(const QTemporaryDir &dir, const QString &name, const QByteArray &bytes)
{
    const QString path = dir.filePath(name);
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly))
        return {};
    file.write(bytes);
    return path;
}

} // namespace

class TestMediaPlugin : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase()
    {
        m_loader = std::make_unique<QPluginLoader>(QStringLiteral(NOTEPADAI_MEDIA_PLUGIN_PATH));
        QObject *instance = m_loader->instance();
        QVERIFY2(instance, qPrintable(m_loader->errorString()));
        m_factory = qobject_cast<MediaPlugin::Factory *>(instance);
        QVERIFY(m_factory);
        QVERIFY(m_dir.isValid());
    }

    void audioViewShowsTheFileHeaders()
    {
        FakeHost host;
        QWidget parent;
        std::unique_ptr<MediaPlugin::View> view(m_factory->create(MediaPlugin::Kind::Audio, &host, &parent));
        QVERIFY(view);
        QVERIFY(host.toolWidgets > 0);
        view->load(writeFile(m_dir, QStringLiteral("loop.wav"), wavFile()), false);
        QTRY_VERIFY_WITH_TIMEOUT(host.info.contains(QStringLiteral("PCM 24-bit")), 5000);
        QVERIFY2(host.info.contains(QStringLiteral("stereo")), qPrintable(host.info));
        QVERIFY2(host.info.contains(QStringLiteral("0:00.500")), qPrintable(host.info));   // loop start
        view.reset();   // tears down the player and the waveform worker
    }

    void videoViewBuilds()
    {
        FakeHost host;
        QWidget parent;
        std::unique_ptr<MediaPlugin::View> view(m_factory->create(MediaPlugin::Kind::Video, &host, &parent));
        QVERIFY(view);
        view->load(m_dir.filePath(QStringLiteral("missing.mp4")), false);
        QVERIFY(!host.banner.isEmpty());
    }

    void pdfViewLoadsFromMemory()
    {
#ifdef NOTEPADAI_MEDIA_HAS_PDF
        FakeHost host;
        QWidget parent;
        std::unique_ptr<MediaPlugin::View> view(m_factory->create(MediaPlugin::Kind::Pdf, &host, &parent));
        QVERIFY(view);
        const QString path = writeFile(m_dir, QStringLiteral("page.pdf"), pdfFile());
        view->load(path, false);
        QTRY_VERIFY_WITH_TIMEOUT(host.info.contains(QStringLiteral("A4")), 5000);
        // Read into memory: the file can be replaced while the tab is open.
        QVERIFY(QFile::remove(path));
#else
        QSKIP("Built without Qt Pdf");
#endif
    }

private:
    std::unique_ptr<QPluginLoader> m_loader;
    MediaPlugin::Factory *m_factory = nullptr;
    QTemporaryDir m_dir;
};

QTEST_MAIN(TestMediaPlugin)
#include "test_media_plugin.moc"
