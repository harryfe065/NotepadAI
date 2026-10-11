#include "UsagePopover.h"

#include <QEvent>
#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QScreen>
#include <QToolButton>
#include <QVBoxLayout>
#include <QVariantAnimation>

#include <algorithm>

#include "UsageStyle.h"
#include "UsageWindowBar.h"

using namespace ai::claudeusage;

namespace {

constexpr int kPadding = 9;       // 8px content inset + 1px border
constexpr int kSpacing = 8;
constexpr int kRadius = 4;
constexpr int kAnchorGap = 6;
constexpr int kScreenMargin = 8;
constexpr int kRefreshIcon = 12;
constexpr int kRefreshButton = 24;
constexpr int kProviderIcon = 14;

QLabel *boldLabel(const QString &text, QWidget *parent)
{
    auto *l = new QLabel(text, parent);
    QFont f = l->font();
    f.setBold(true);
    l->setFont(f);
    return l;
}

// 1px rule in the palette's border color (the spec's section border-top).
class Divider : public QWidget
{
public:
    explicit Divider(QWidget *parent) : QWidget(parent)
    {
        setFixedHeight(1);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter(this).fillRect(rect(), palette().color(QPalette::Mid));
    }
};

} // namespace

UsagePopover::UsagePopover(QWidget *parent)
    : QFrame(parent, Qt::Popup | Qt::FramelessWindowHint | Qt::NoDropShadowWindowHint)
{
    setAttribute(Qt::WA_TranslucentBackground);
    setFixedWidth(kWidth);

    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(kPadding, kPadding, kPadding, kPadding);
    root->setSpacing(kSpacing);

    // Header: title, "% used" toggle, refresh.
    auto *header = new QHBoxLayout;
    header->setContentsMargins(0, 0, 0, 0);
    header->setSpacing(4);
    header->addWidget(boldLabel(tr("Usage"), this));
    header->addStretch(1);

    m_modeButton = new QToolButton(this);
    m_modeButton->setAutoRaise(true);
    m_modeButton->setFixedHeight(20);
    m_modeButton->setCursor(Qt::PointingHandCursor);
    connect(m_modeButton, &QToolButton::clicked, this, &UsagePopover::percentModeToggleRequested);
    header->addWidget(m_modeButton);

    m_refreshButton = new QToolButton(this);
    m_refreshButton->setAutoRaise(true);
    m_refreshButton->setFixedSize(kRefreshButton, kRefreshButton);
    m_refreshButton->setIconSize({kRefreshIcon, kRefreshIcon});
    m_refreshButton->setToolTip(tr("Refresh usage"));
    m_refreshButton->setAccessibleName(tr("Refresh usage"));
    connect(m_refreshButton, &QToolButton::clicked, this, &UsagePopover::refreshRequested);
    header->addWidget(m_refreshButton);
    root->addLayout(header);

    root->addWidget(new Divider(this));

    // Provider head: brand glyph + name.
    auto *head = new QHBoxLayout;
    head->setContentsMargins(0, 0, 0, 0);
    head->setSpacing(4);
    m_providerIcon = new QLabel(this);
    m_providerIcon->setFixedSize(kProviderIcon, kProviderIcon);
    head->addWidget(m_providerIcon);
    head->addWidget(boldLabel(QStringLiteral("Claude"), this));
    head->addStretch(1);
    root->addLayout(head);

    m_body = new QVBoxLayout;
    m_body->setContentsMargins(0, 0, 0, 0);
    m_body->setSpacing(kSpacing);
    m_hint = new QLabel(this);
    m_hint->setWordWrap(true);
    m_hint->setForegroundRole(QPalette::PlaceholderText);
    m_body->addWidget(m_hint);
    root->addLayout(m_body);

    m_spin = new QVariantAnimation(this);
    m_spin->setStartValue(0.0);
    m_spin->setEndValue(360.0);
    m_spin->setDuration(1000);
    m_spin->setLoopCount(-1);
    m_spin->setEasingCurve(QEasingCurve::Linear);
    connect(m_spin, &QVariantAnimation::valueChanged, this,
            [this](const QVariant &v) { updateRefreshIcon(v.toReal()); });

    applyChrome();
    setPercentMode(PercentMode::Used);
}

void UsagePopover::setData(const Usage &usage, bool loaded, bool loading, qint64 nowMs)
{
    m_usage = usage;
    m_loaded = loaded;
    m_loading = loading;
    m_nowMs = nowMs;
    rebuild();
}

void UsagePopover::setPercentMode(PercentMode mode)
{
    m_mode = mode;
    m_modeButton->setText(mode == PercentMode::Used ? tr("% used") : tr("% left"));
    m_modeButton->setToolTip(mode == PercentMode::Used ? tr("Show percent remaining") : tr("Show percent used"));
    rebuild();
}

void UsagePopover::rebuild()
{
    const QVector<NamedWindow> windows = windowsOf(m_usage);

    QString hint;
    if (!m_loaded)
        hint = tr("Loading usage…");
    else if (m_usage.status == Status::Unavailable)
        hint = tr("Not signed in. Run claude and sign in.");
    else if (m_usage.status == Status::Error)
        hint = m_usage.error.isEmpty() ? tr("Usage unavailable") : m_usage.error;
    else if (windows.isEmpty())
        hint = tr("No usage data.");
    const bool showBars = hint.isEmpty();
    m_hint->setText(hint);
    m_hint->setVisible(!showBars);

    const int want = showBars ? int(windows.size()) : 0;
    while (m_bars.size() > want)
        delete m_bars.takeLast();
    while (m_bars.size() < want) {
        auto *bar = new UsageWindowBar(this);
        m_body->addWidget(bar);
        m_bars.push_back(bar);
    }
    for (int i = 0; i < want; ++i)
        m_bars[i]->setWindow(windows[i].label, windows[i].window, m_mode, m_nowMs);

    m_refreshButton->setEnabled(!m_loading);
    setSpinning(m_loading && isVisible());

    if (isVisible()) {
        layout()->activate();
        resize(kWidth, sizeHint().height());
    }
}

void UsagePopover::updateRefreshIcon(qreal angle)
{
    const qreal dpr = devicePixelRatioF();
    QPixmap base = usagestyle::tintedPixmap(QStringLiteral(":/icons/refresh-cw.svg"),
                                            palette().color(QPalette::PlaceholderText), kRefreshIcon, dpr);
    if (angle != 0.0 && !base.isNull()) {
        QPixmap rotated(base.size());
        rotated.setDevicePixelRatio(dpr);
        rotated.fill(Qt::transparent);
        QPainter p(&rotated);
        p.setRenderHint(QPainter::SmoothPixmapTransform);
        const qreal c = kRefreshIcon / 2.0;
        p.translate(c, c);
        p.rotate(angle);
        p.translate(-c, -c);
        p.drawPixmap(0, 0, base);
        base = rotated;
    }
    m_refreshButton->setIcon(QIcon(base));
}

void UsagePopover::setSpinning(bool on)
{
    // The 1s loop only runs while the popup is open and a fetch is in flight.
    if (on == (m_spin->state() == QAbstractAnimation::Running))
        return;
    if (on) {
        m_spin->start();
    } else {
        m_spin->stop();
        updateRefreshIcon(0.0);
    }
}

void UsagePopover::showBelow(QWidget *anchor)
{
    rebuild();
    layout()->activate();
    resize(kWidth, sizeHint().height());

    const QPoint bottomRight = anchor->mapToGlobal(QPoint(anchor->width(), anchor->height()));
    const QRect avail = anchor->screen()->availableGeometry();

    QPoint pos(bottomRight.x() - width(), bottomRight.y() + kAnchorGap);
    pos.setX(std::clamp(pos.x(), avail.left() + kScreenMargin,
                        std::max(avail.left() + kScreenMargin, avail.right() - width() - kScreenMargin + 1)));
    if (pos.y() + height() > avail.bottom())
        pos.setY(std::max(avail.top(), anchor->mapToGlobal(QPoint(0, 0)).y() - height() - kAnchorGap));
    move(pos);

    show();
    setSpinning(m_loading);
}

void UsagePopover::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);

    // Raised surface: nudge the window color toward the text color so it lifts
    // off the chrome in both light and dark themes.
    const QColor win = palette().color(QPalette::Window);
    const QColor fg = palette().color(QPalette::WindowText);
    const QColor bg(win.red() + (fg.red() - win.red()) / 14,
                    win.green() + (fg.green() - win.green()) / 14,
                    win.blue() + (fg.blue() - win.blue()) / 14);

    p.setPen(QPen(palette().color(QPalette::Mid), 1));
    p.setBrush(bg);
    p.drawRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), kRadius, kRadius);
}

void UsagePopover::changeEvent(QEvent *event)
{
    QFrame::changeEvent(event);
    if (event->type() == QEvent::PaletteChange || event->type() == QEvent::StyleChange
        || event->type() == QEvent::ApplicationPaletteChange)
        applyChrome();
}

// Theme-dependent bits that are not painted from the live palette: the tinted
// SVG pixmaps and the link-colored toggle.
void UsagePopover::applyChrome()
{
    const qreal dpr = devicePixelRatioF();
    m_providerIcon->setPixmap(usagestyle::brandPixmap(QStringLiteral(":/icons/claude.svg"), kProviderIcon, dpr));
    updateRefreshIcon(0.0);

    // The "% used" toggle reads as a link.
    QPalette pal = m_modeButton->palette();
    pal.setColor(QPalette::ButtonText, palette().color(QPalette::Link));
    pal.setColor(QPalette::WindowText, palette().color(QPalette::Link));
    m_modeButton->setPalette(pal);
}

void UsagePopover::hideEvent(QHideEvent *event)
{
    setSpinning(false);
    QFrame::hideEvent(event);
    emit closed();
}
