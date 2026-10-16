#ifndef ASSETPREVIEWWIDGET_H
#define ASSETPREVIEWWIDGET_H

#include "PreviewContentWidget.h"

#include <QString>

#include <functional>

class QFrame;
class QHBoxLayout;
class QLabel;
class QPushButton;
class QToolButton;
class QVBoxLayout;

// Base of every file-backed asset viewer (images, models, maps, media, hex).
//
// Owns the chrome shared by all of them: a toolbar (subclass controls on the
// left, "Open as text" on the right), a status banner under it, the content
// area and a muted info line at the bottom. Loading is deferred: loadFromFile()
// only records the path, and the subclass's startLoad() runs on the first show,
// so restoring a session with many asset tabs decodes nothing until a tab is
// looked at. reloadFromDisk() reloads now when visible, else on the next show.
class AssetPreviewWidget : public PreviewContentWidget
{
    Q_OBJECT

public:
    AssetPreviewWidget(const QString &typeId, const QString &displayName, QWidget *parent = nullptr);

    QString typeId() const override { return m_typeId; }
    QString displayName() const override { return m_displayName; }

    // Asset previews are file-path previews: the text route is unused.
    void setContent(const QString &text, const QString &basePath) override { Q_UNUSED(text); Q_UNUSED(basePath); }
    void refresh(const QString &text) override { Q_UNUSED(text); }
    void applyTheme(const QPalette &palette, bool isDark) override;

    void loadFromFile(const QString &path) override;
    void reloadFromDisk() override;

    QString filePath() const { return m_path; }

signals:
    void openAsTextRequested(const QString &path);

protected:
    enum class BannerKind : std::uint8_t { Neutral, Warning, Error };

    // reload == true keeps the view state (zoom, pan, camera, frame).
    virtual void startLoad(bool reload) = 0;
    // Called after applyTheme(); re-tint subclass icons here.
    virtual void themeChanged(bool isDark) { Q_UNUSED(isDark); }

    QHBoxLayout *toolbarLayout() const { return m_toolbarLayout; }
    void setContentWidget(QWidget *content);
    void setInfoText(const QString &text);
    void setInfoToolTip(const QString &text);
    // "Open as text" for the current file (the toolbar button's action).
    void requestOpenAsText();

    void showBanner(BannerKind kind, const QString &text,
                    const QString &actionText = QString(), std::function<void()> action = {});
    void hideBanner();

    // A compact auto-raise toolbar button (ui-dna toolbar style).
    QToolButton *addToolButton(const QString &text, const QString &toolTip, bool checkable = false);
    // Any other control (combo, slider, label), inserted after the previous one.
    void addToolWidget(QWidget *widget);
    // Wider gap between groups (whitespace, no divider line).
    void addToolGap();

    bool isDarkTheme() const { return m_isDark; }

    void showEvent(QShowEvent *event) override;

private:
    void runPendingLoad();

    QString m_typeId;
    QString m_displayName;
    QString m_path;
    bool m_loadPending = false;
    bool m_loadedOnce = false;
    bool m_isDark = false;

    QVBoxLayout *m_rootLayout = nullptr;
    QFrame *m_toolbar = nullptr;
    QHBoxLayout *m_toolbarLayout = nullptr;
    QToolButton *m_openAsText = nullptr;
    QFrame *m_banner = nullptr;
    QLabel *m_bannerText = nullptr;
    QPushButton *m_bannerAction = nullptr;
    std::function<void()> m_bannerCallback;
    BannerKind m_bannerKind = BannerKind::Neutral;
    QWidget *m_content = nullptr;
    QLabel *m_info = nullptr;
};

#endif // ASSETPREVIEWWIDGET_H
