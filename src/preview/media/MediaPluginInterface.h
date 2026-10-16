#ifndef MEDIAPLUGININTERFACE_H
#define MEDIAPLUGININTERFACE_H

#include <QPalette>
#include <QString>
#include <QtPlugin>

#include <functional>

class QToolButton;
class QWidget;

// Contract between NotepadAI and the NotepadAIMedia plugin (add-media-preview
// D1). Header-only and free of QObject: the plugin never links the
// executable, and both sides come from one build, so QString and
// std::function cross the boundary safely.
//
// The executable owns the tab (MediaPreviewWidget, an AssetPreviewWidget) and
// implements Host; the plugin implements one View per tab.
namespace MediaPlugin {

enum class Kind { Audio, Video, Pdf };

class Host
{
public:
    enum class Banner { Neutral, Warning, Error };

    // Toolbar controls, left of "Open as text", in call order.
    virtual void addToolWidget(QWidget *widget) = 0;
    // A compact auto-raise button in the toolbar's style.
    virtual QToolButton *addToolButton(const QString &text, const QString &toolTip, bool checkable = false) = 0;
    virtual void addToolGap() = 0;
    virtual void setInfoText(const QString &text) = 0;
    // Tooltip of the info line (long metadata: tags, comments).
    virtual void setInfoToolTip(const QString &text) = 0;
    virtual void showBanner(Banner kind, const QString &text, const QString &actionText = QString(),
                            std::function<void()> action = {}) = 0;
    virtual void hideBanner() = 0;
    // "Open as text" for error banners.
    virtual void openAsText() = 0;

protected:
    ~Host() = default;
};

class View
{
public:
    virtual ~View() = default;

    // The content widget; the host's layout takes ownership (deleting it
    // deletes the view).
    virtual QWidget *widget() = 0;
    // reload == true keeps the view state (position, page, zoom).
    virtual void load(const QString &path, bool reload) = 0;
    // The tab was shown or hidden.
    virtual void setTabVisible(bool visible) = 0;
    virtual void themeChanged(const QPalette &palette, bool isDark) = 0;
};

class Factory
{
public:
    virtual ~Factory() = default;
    // Returns nullptr for a kind the plugin was built without.
    virtual View *create(Kind kind, Host *host, QWidget *parent) = 0;
};

} // namespace MediaPlugin

#define NotepadAIMediaFactory_iid "ai.notepad.MediaPreview.Factory/2"
Q_DECLARE_INTERFACE(MediaPlugin::Factory, NotepadAIMediaFactory_iid)

#endif // MEDIAPLUGININTERFACE_H
