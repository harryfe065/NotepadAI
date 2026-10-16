#include "PdfView.h"

#include "MediaFormat.h"

#include <QFile>
#include <QFileInfo>
#include <QInputDialog>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QPdfDocument>
#include <QPdfPageNavigator>
#include <QPdfView>
#include <QPointer>
#include <QSpinBox>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWheelEvent>
#include <QtConcurrent>

#include <algorithm>
#include <cmath>

namespace {

constexpr double kZoomStep = 1.2;
constexpr double kMinZoom = 0.1;
constexpr double kMaxZoom = 16.0;

// "A4", "Letter", ... for common paper sizes in either orientation.
QString paperName(QSizeF points)
{
    struct Paper {
        const char *name;
        double w, h;
    };
    static const Paper papers[] = {{"A3", 842, 1191}, {"A4", 595, 842},     {"A5", 420, 595},
                                   {"Letter", 612, 792}, {"Legal", 612, 1008}, {"Tabloid", 792, 1224}};
    const double a = std::min(points.width(), points.height()), b = std::max(points.width(), points.height());
    for (const Paper &p : papers) {
        if (std::abs(a - p.w) < 3 && std::abs(b - p.h) < 3)
            return QString::fromLatin1(p.name);
    }
    return {};
}

} // namespace

PdfView::PdfView(MediaPlugin::Host *host, QWidget *parent)
    : QWidget(parent)
    , m_host(host)
{
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    m_doc = new QPdfDocument(this);
    m_view = new QPdfView(this);
    m_view->setDocument(m_doc);
    m_view->setPageMode(QPdfView::PageMode::MultiPage);
    m_view->setZoomMode(QPdfView::ZoomMode::FitToWidth);
    m_view->setPageSpacing(8);
    m_view->viewport()->installEventFilter(this);
    layout->addWidget(m_view);
    setFocusProxy(m_view);

    auto button = [this](const QString &text, const QString &tip, bool checkable = false) {
        auto *b = new QToolButton;
        b->setText(text);
        b->setToolTip(tip);
        b->setAutoRaise(true);
        b->setCheckable(checkable);
        m_host->addToolWidget(b);
        return b;
    };
    m_prev = button(tr("◀"), tr("Previous page"));
    m_page = new QSpinBox;
    m_page->setToolTip(tr("Page"));
    m_page->setMinimum(1);
    m_page->setKeyboardTracking(false);
    m_page->setButtonSymbols(QAbstractSpinBox::NoButtons);
    m_page->setAlignment(Qt::AlignRight);
    m_host->addToolWidget(m_page);
    m_pageCount = new QLabel;
    m_host->addToolWidget(m_pageCount);
    m_next = button(tr("▶"), tr("Next page"));
    m_host->addToolGap();
    m_zoomOut = button(tr("−"), tr("Zoom out (Ctrl+-, Ctrl+wheel)"));
    m_zoomLabel = new QLabel;
    m_zoomLabel->setMinimumWidth(m_zoomLabel->fontMetrics().horizontalAdvance(QStringLiteral("1600%")));
    m_zoomLabel->setAlignment(Qt::AlignCenter);
    m_host->addToolWidget(m_zoomLabel);
    m_zoomIn = button(tr("+"), tr("Zoom in (Ctrl+=, Ctrl+wheel)"));
    m_fitWidth = button(tr("Fit width"), tr("Fit the page width"), true);
    m_fitPage = button(tr("Fit page"), tr("Fit the whole page"), true);

    auto jump = [this](int page) {
        if (page < 0 || page >= m_doc->pageCount())
            return;
        m_view->pageNavigator()->jump(page, QPointF(), m_view->pageNavigator()->currentZoom());
    };
    connect(m_prev, &QToolButton::clicked, this, [this, jump]() { jump(m_view->pageNavigator()->currentPage() - 1); });
    connect(m_next, &QToolButton::clicked, this, [this, jump]() { jump(m_view->pageNavigator()->currentPage() + 1); });
    connect(m_page, QOverload<int>::of(&QSpinBox::valueChanged), this, [this, jump](int value) {
        if (!m_syncing)
            jump(value - 1);
    });
    connect(m_zoomOut, &QToolButton::clicked, this, [this]() { zoomBy(1.0 / kZoomStep); });
    connect(m_zoomIn, &QToolButton::clicked, this, [this]() { zoomBy(kZoomStep); });
    connect(m_fitWidth, &QToolButton::clicked, this, [this]() { setFit(true, false); });
    connect(m_fitPage, &QToolButton::clicked, this, [this]() { setFit(false, true); });

    connect(m_view->pageNavigator(), &QPdfPageNavigator::currentPageChanged, this, &PdfView::syncControls);
    connect(m_view, &QPdfView::zoomFactorChanged, this, &PdfView::syncControls);
    connect(m_view, &QPdfView::zoomModeChanged, this, &PdfView::syncControls);
    connect(m_doc, &QPdfDocument::statusChanged, this, &PdfView::onStatusChanged);
    connect(&m_reader, &QFutureWatcher<ReadResult>::finished, this, &PdfView::onRead);
    syncControls();
}

PdfView::~PdfView()
{
    m_doc->close();
}

void PdfView::load(const QString &path, bool reload)
{
    m_path = path;
    m_restorePage = reload ? m_view->pageNavigator()->currentPage() : -1;
    const QFileInfo info(path);
    if (!info.exists()) {
        m_host->showBanner(MediaPlugin::Host::Banner::Error, tr("The file no longer exists."));
        return;
    }
    m_fileSize = info.size();
    m_fromDisk = m_fileSize > kMaxInMemoryBytes;
    if (m_fromDisk) {
        openDocument();
        return;
    }
    m_reader.setFuture(QtConcurrent::run([path]() {
        ReadResult r;
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly))
            r.error = file.errorString();
        else
            r.bytes = file.readAll();
        return r;
    }));
}

void PdfView::onRead()
{
    const ReadResult r = m_reader.result();
    if (!r.error.isEmpty()) {
        m_host->showBanner(MediaPlugin::Host::Banner::Error, tr("Cannot read the file: %1").arg(r.error));
        return;
    }
    m_doc->close();
    m_buffer.close();
    m_buffer.setData(r.bytes);
    m_buffer.open(QIODevice::ReadOnly);
    openDocument();
}

void PdfView::openDocument()
{
    m_host->hideBanner();
    if (m_fromDisk) {
        m_doc->close();
        m_doc->load(m_path);    // synchronous; statusChanged reports the outcome
    } else {
        m_buffer.seek(0);
        m_doc->load(&m_buffer);
    }
}

void PdfView::onStatusChanged()
{
    switch (m_doc->status()) {
    case QPdfDocument::Status::Ready:
        m_host->hideBanner();
        if (m_restorePage >= 0) {
            const int page = std::min(m_restorePage, m_doc->pageCount() - 1);
            m_restorePage = -1;
            m_view->pageNavigator()->jump(page, QPointF(), m_view->pageNavigator()->currentZoom());
        }
        syncControls();
        updateInfo();
        break;
    case QPdfDocument::Status::Error:
        if (m_doc->error() == QPdfDocument::Error::IncorrectPassword) {
            m_host->showBanner(MediaPlugin::Host::Banner::Neutral, tr("This PDF is protected by a password."),
                               tr("Enter password"), [this]() { askPassword(); });
        } else {
            QString reason;
            switch (m_doc->error()) {
            case QPdfDocument::Error::FileNotFound: reason = tr("the file was not found"); break;
            case QPdfDocument::Error::InvalidFileFormat: reason = tr("it is not a valid PDF"); break;
            case QPdfDocument::Error::UnsupportedSecurityScheme: reason = tr("its encryption is not supported"); break;
            default: reason = tr("unknown error"); break;
            }
            m_host->showBanner(MediaPlugin::Host::Banner::Error, tr("Cannot open the PDF: %1.").arg(reason),
                               tr("Open as text"), [this]() { m_host->openAsText(); });
        }
        syncControls();
        updateInfo();
        break;
    default:
        break;
    }
}

void PdfView::askPassword()
{
    // The tab may close while the dialog runs.
    const QPointer<PdfView> self(this);
    bool ok = false;
    const QString password = QInputDialog::getText(this, tr("PDF password"), tr("Password for %1:")
                                                                                 .arg(QFileInfo(m_path).fileName()),
                                                   QLineEdit::Password, QString(), &ok);
    if (!self || !ok)
        return;
    m_doc->setPassword(password);
    openDocument();
}

void PdfView::zoomBy(double factor)
{
    const double zoom = std::clamp(m_view->zoomFactor() * factor, kMinZoom, kMaxZoom);
    m_view->setZoomMode(QPdfView::ZoomMode::Custom);
    m_view->setZoomFactor(zoom);
}

void PdfView::setFit(bool width, bool page)
{
    if (width)
        m_view->setZoomMode(QPdfView::ZoomMode::FitToWidth);
    else if (page)
        m_view->setZoomMode(QPdfView::ZoomMode::FitInView);
    syncControls();
}

void PdfView::syncControls()
{
    m_syncing = true;
    const int pages = m_doc->status() == QPdfDocument::Status::Ready ? m_doc->pageCount() : 0;
    const int current = m_view->pageNavigator()->currentPage();
    m_page->setMaximum(std::max(1, pages));
    m_page->setValue(current + 1);
    m_page->setEnabled(pages > 1);
    m_page->setFixedWidth(m_page->fontMetrics().horizontalAdvance(QString::number(std::max(pages, 10)))
                          + m_page->fontMetrics().horizontalAdvance(QLatin1Char('0')) * 2);
    m_pageCount->setText(tr("/ %1").arg(pages));
    m_prev->setEnabled(current > 0);
    m_next->setEnabled(current + 1 < pages);
    const QPdfView::ZoomMode mode = m_view->zoomMode();
    m_fitWidth->setChecked(mode == QPdfView::ZoomMode::FitToWidth);
    m_fitPage->setChecked(mode == QPdfView::ZoomMode::FitInView);
    m_zoomLabel->setText(QStringLiteral("%1%").arg(qRound(m_view->zoomFactor() * 100)));
    m_syncing = false;
}

void PdfView::updateInfo()
{
    QStringList parts;
    parts << QStringLiteral("PDF");
    const bool ready = m_doc->status() == QPdfDocument::Status::Ready;
    if (ready) {
        parts << tr("%n pages", nullptr, m_doc->pageCount());
        if (m_doc->pageCount() > 0) {
            const QSizeF pt = m_doc->pagePointSize(0);
            const QString name = paperName(pt);
            const QString mm = QStringLiteral("%1 × %2 mm")
                                   .arg(qRound(pt.width() * 25.4 / 72.0))
                                   .arg(qRound(pt.height() * 25.4 / 72.0));
            parts << (name.isEmpty() ? mm : QStringLiteral("%1 (%2)").arg(name, mm));
        }
        const QString title = m_doc->metaData(QPdfDocument::MetaDataField::Title).toString().trimmed();
        if (!title.isEmpty())
            parts << title;
        const QString author = m_doc->metaData(QPdfDocument::MetaDataField::Author).toString().trimmed();
        if (!author.isEmpty())
            parts << author;
    }
    parts << MediaFormat::bytesText(m_fileSize);
    m_host->setInfoText(parts.join(MediaFormat::separator()));

    QStringList tip;
    if (ready) {
        const std::pair<QPdfDocument::MetaDataField, const char *> fields[] = {
            {QPdfDocument::MetaDataField::Subject, QT_TR_NOOP("Subject: %1")},
            {QPdfDocument::MetaDataField::Keywords, QT_TR_NOOP("Keywords: %1")},
            {QPdfDocument::MetaDataField::Creator, QT_TR_NOOP("Creator: %1")},
            {QPdfDocument::MetaDataField::Producer, QT_TR_NOOP("Producer: %1")}};
        for (const auto &[field, label] : fields) {
            const QString value = m_doc->metaData(field).toString().trimmed();
            if (!value.isEmpty())
                tip << tr(label).arg(value);
        }
        const QDateTime created = m_doc->metaData(QPdfDocument::MetaDataField::CreationDate).toDateTime();
        if (created.isValid())
            tip << tr("Created: %1").arg(QLocale().toString(created, QLocale::ShortFormat));
    }
    m_host->setInfoToolTip(tip.join(QLatin1Char('\n')));
}

void PdfView::themeChanged(const QPalette &palette, bool isDark)
{
    Q_UNUSED(isDark);
    m_view->setPalette(palette);
}

bool PdfView::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == m_view->viewport() && event->type() == QEvent::Wheel) {
        auto *wheel = static_cast<QWheelEvent *>(event);
        if (wheel->modifiers() & Qt::ControlModifier) {
            const int steps = wheel->angleDelta().y();
            if (steps != 0)
                zoomBy(std::pow(kZoomStep, steps / 120.0));
            return true;
        }
    }
    return QWidget::eventFilter(watched, event);
}

void PdfView::keyPressEvent(QKeyEvent *event)
{
    if (event->modifiers() & Qt::ControlModifier) {
        if (event->key() == Qt::Key_Plus || event->key() == Qt::Key_Equal) {
            zoomBy(kZoomStep);
            return;
        }
        if (event->key() == Qt::Key_Minus) {
            zoomBy(1.0 / kZoomStep);
            return;
        }
        if (event->key() == Qt::Key_0) {
            setFit(true, false);
            return;
        }
    }
    QWidget::keyPressEvent(event);
}
