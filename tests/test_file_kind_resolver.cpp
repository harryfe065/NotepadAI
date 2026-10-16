/*
 * This file is part of Notepad Next.
 * Copyright 2026 NotepadAI contributors
 *
 * Notepad Next is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Notepad Next is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Notepad Next.  If not, see <https://www.gnu.org/licenses/>.
 */

#include <QtTest>
#include <QFile>
#include <QTemporaryDir>

#include "preview/FileKindResolver.h"

using Kind = FileKindResolver::Kind;

class TestFileKindResolver : public QObject
{
    Q_OBJECT

private:
    static FileKindResolver makeResolver(int *probeCalls = nullptr)
    {
        FileKindResolver r;
        r.addPreviewExtensions(QStringLiteral("image"), {QStringLiteral("png"), QStringLiteral("JPG")});
        r.addPreviewExtensions(QStringLiteral("svg"), {QStringLiteral("svg")});
        r.addSniffer(QStringLiteral("image"), [](QByteArrayView h) { return h.startsWith("\x89PNG"); });
        r.setTextExtensionProbe([probeCalls](const QString &ext) {
            if (probeCalls) ++*probeCalls;
            return ext == QLatin1String("cpp") || ext == QLatin1String("svg") || ext == QLatin1String("txt");
        });
        return r;
    }

    QString writeFile(const QString &name, const QByteArray &bytes)
    {
        const QString path = m_dir.filePath(name);
        QFile f(path);
        if (!f.open(QIODevice::WriteOnly))
            return QString();
        f.write(bytes);
        return path;
    }

    QTemporaryDir m_dir;

private slots:
    void extensionDecidesWithoutProbe()
    {
        int probeCalls = 0;
        const FileKindResolver r = makeResolver(&probeCalls);
        bool known = false;
        const auto res = r.resolveByExtension(QStringLiteral("png"), &known);
        QVERIFY(known);
        QCOMPARE(res.kind, Kind::Preview);
        QCOMPARE(res.typeId, QStringLiteral("image"));
        QCOMPARE(probeCalls, 0);
    }

    void extensionsAreCaseFolded()
    {
        const FileKindResolver r = makeResolver();
        bool known = false;
        QCOMPARE(r.resolveByExtension(QStringLiteral("jpg"), &known).typeId, QStringLiteral("image"));
        QVERIFY(known);
    }

    // A preview extension that is also a Lua text language (svg in xml.lua)
    // still previews: "Open as text" is the way to the editor.
    void previewExtensionBeatsTextLanguage()
    {
        const FileKindResolver r = makeResolver();
        bool known = false;
        const auto res = r.resolveByExtension(QStringLiteral("svg"), &known);
        QCOMPARE(res.kind, Kind::Preview);
        QCOMPARE(res.typeId, QStringLiteral("svg"));
    }

    void knownTextExtensionNeverReadsFile()
    {
        const FileKindResolver r = makeResolver();
        // Binary bytes under a text extension: the extension wins, no read.
        const QString path = writeFile(QStringLiteral("a.cpp"), QByteArray("\x89PNG\0\0\0", 7));
        QCOMPARE(r.resolve(path).kind, Kind::Text);
    }

    void unknownExtensionSniffsMagic()
    {
        const FileKindResolver r = makeResolver();
        const QString path = writeFile(QStringLiteral("texture.bin"), QByteArray("\x89PNG\r\n\x1a\n\0\0", 10));
        const auto res = r.resolve(path);
        QCOMPARE(res.kind, Kind::Preview);
        QCOMPARE(res.typeId, QStringLiteral("image"));
    }

    void missingExtensionSniffsMagic()
    {
        const FileKindResolver r = makeResolver();
        const QString path = writeFile(QStringLiteral("noext"), QByteArray("\x89PNG\r\n\x1a\n", 8));
        QCOMPARE(r.resolve(path).typeId, QStringLiteral("image"));
    }

    void nulMeansBinary()
    {
        const FileKindResolver r = makeResolver();
        const QString path = writeFile(QStringLiteral("blob.dat"), QByteArray("abc\0def", 7));
        QCOMPARE(r.resolve(path).kind, Kind::Binary);
    }

    void plainTextUnknownExtension()
    {
        const FileKindResolver r = makeResolver();
        const QString path = writeFile(QStringLiteral("notes.zzz"), QByteArrayLiteral("hello\nworld\n"));
        QCOMPARE(r.resolve(path).kind, Kind::Text);
    }

    void bomMarksUtf16AsText()
    {
        const FileKindResolver r = makeResolver();
        QCOMPARE(r.classifyHead(QByteArrayView("\xFF\xFEh\0i\0", 6)).kind, Kind::Text);
        QCOMPARE(r.classifyHead(QByteArrayView("\xFE\xFF\0h\0i", 6)).kind, Kind::Text);
        QCOMPARE(r.classifyHead(QByteArrayView("\xEF\xBB\xBFhi", 5)).kind, Kind::Text);
    }

    void emptyFileIsText()
    {
        const FileKindResolver r = makeResolver();
        const QString path = writeFile(QStringLiteral("empty.zzz"), QByteArray());
        QCOMPARE(r.resolve(path).kind, Kind::Text);
        QCOMPARE(r.resolve(m_dir.filePath(QStringLiteral("missing.zzz"))).kind, Kind::Text);
    }

    // NUL beyond the head is not seen: the decision reads kHeadBytes at most.
    void headIsBounded()
    {
        const FileKindResolver r = makeResolver();
        QByteArray bytes(FileKindResolver::kHeadBytes, 'a');
        bytes.append('\0');
        const QString path = writeFile(QStringLiteral("long.zzz"), bytes);
        QCOMPARE(r.resolve(path).kind, Kind::Text);
    }

    void sniffersRunBeforeNulScan()
    {
        const FileKindResolver r = makeResolver();
        QCOMPARE(r.classifyHead(QByteArrayView("\x89PNG\0\0", 6)).kind, Kind::Preview);
    }
};

QTEST_APPLESS_MAIN(TestFileKindResolver)
#include "test_file_kind_resolver.moc"
