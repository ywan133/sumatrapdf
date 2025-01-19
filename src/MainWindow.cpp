/* Copyright 2022 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

#include "utils/BaseUtil.h"
#include <UIAutomationCore.h>
#include <UIAutomationCoreApi.h>
#include "utils/ScopedWin.h"
#include "utils/FileUtil.h"
#include "utils/WinUtil.h"
#include "utils/Dpi.h"

#include "wingui/UIModels.h"
#include "wingui/Layout.h"
#include "wingui/WinGui.h"

#include "wingui/LabelWithCloseWnd.h"
#include "wingui/FrameRateWnd.h"

#include "Settings.h"
#include "DocController.h"
#include "EngineBase.h"
#include "EngineAll.h"
#include "GlobalPrefs.h"
#include "AppColors.h"
#include "ChmModel.h"
#include "DisplayModel.h"
#include "ProgressUpdateUI.h"
#include "Notifications.h"
#include "TextSelection.h"
#include "TextSearch.h"
#include "Annotation.h"
#include "SumatraPDF.h"
#include "MainWindow.h"
#include "WindowTab.h"
#include "TableOfContents.h"
#include "resource.h"
#include "Commands.h"
#include "Caption.h"
#include "Selection.h"
#include "Flags.h"
#include "StressTesting.h"
#include "Translations.h"
#include "uia/Provider.h"
#include "Theme.h"

#include "utils/Log.h"

struct LinkHandler : ILinkHandler {
    MainWindow* win = nullptr;

    explicit LinkHandler(MainWindow* w) {
        ReportIf(!w);
        win = w;
    }
    ~LinkHandler() override;

    DocController* GetDocController() override {
        return win->ctrl;
    }
    void GotoLink(IPageDestination*) override;
    void GotoNamedDest(const char*) override;
    void ScrollTo(IPageDestination*) override;
    void LaunchURL(const char*) override;
    void LaunchFile(const char* path, IPageDestination*) override;
    IPageDestination* FindTocItem(TocItem* item, const char* name, bool partially) override;
};

LinkHandler::~LinkHandler() {
    // do nothing
}

Vec<MainWindow*> gWindows;

StaticLinkInfo::StaticLinkInfo(Rect rect, const char* target, const char* infotip) {
    this->rect = rect;
    this->target = str::Dup(target);
    this->tooltip = str::Dup(infotip);
}

StaticLinkInfo::StaticLinkInfo(const StaticLinkInfo& other) {
    rect = other.rect;
    str::ReplaceWithCopy(&target, other.target);
    str::ReplaceWithCopy(&tooltip, other.tooltip);
}

StaticLinkInfo& StaticLinkInfo::operator=(const StaticLinkInfo& other) {
    if (this == &other) {
        return *this;
    }
    rect = other.rect;
    str::ReplaceWithCopy(&target, other.target);
    str::ReplaceWithCopy(&tooltip, other.tooltip);
    return *this;
}

StaticLinkInfo::~StaticLinkInfo() {
    str::Free(target);
    str::Free(tooltip);
}

MainWindow::MainWindow(HWND hwnd) {
    hwndFrame = hwnd;
    linkHandler = new LinkHandler(this);
}

static WORD dotPatternBmp[8] = {0x00aa, 0x0055, 0x00aa, 0x0055, 0x00aa, 0x0055, 0x00aa, 0x0055};

void CreateMovePatternLazy(MainWindow* win) {
    if (win->bmpMovePattern) {
        return;
    }
    win->bmpMovePattern = CreateBitmap(8, 8, 1, 1, dotPatternBmp);
    ReportIf(!win->bmpMovePattern);
    win->brMovePattern = CreatePatternBrush(win->bmpMovePattern);
    ReportIf(!win->brMovePattern);
}

MainWindow::~MainWindow() {
    FinishStressTest(this);

    ReportIf(TabCount() > 0);
    // ReportIf(ctrl); // TODO: seen in crash report
    ReportIf(linkOnLastButtonDown);

    UnsubclassToc(this);

    DeleteObject(brMovePattern);
    DeleteObject(bmpMovePattern);
    DeleteObject(brControlBgColor);

    // release our copy of UIA provider
    // the UI automation still might have a copy somewhere
    if (uiaProvider) {
        if (AsFixed()) {
            uiaProvider->OnDocumentUnload();
        }
        uiaProvider->Release();
    }

    delete linkHandler;
    delete buffer;
    delete tabSelectionHistory;
    DeleteCaption(caption);
    DeleteVecMembers(staticLinks);
    auto tabs = Tabs();
    DeleteVecMembers(tabs);
    delete tabsCtrl;
    // cbHandler is passed into DocController and must be deleted afterwards
    // (all controllers should have been deleted prior to MainWindow, though)
    delete cbHandler;

    delete frameRateWnd;
    delete infotip;
    delete tocTreeView;
    if (favTreeView) {
        delete favTreeView->treeModel;
        delete favTreeView;
    }

    delete sidebarSplitter;
    delete favSplitter;
    delete tocLabelWithClose;
    delete favLabelWithClose;
}

void ClearMouseState(MainWindow* win) {
    win->linkOnLastButtonDown = nullptr;
    win->annotationUnderCursor = nullptr;
}

bool MainWindow::HasDocsLoaded() const {
    int nTabs = TabCount();
    if (nTabs == 0) {
        // logf("HasDocsLoaded: false because nTabs == 0\n");
        return true;
    }
    for (int i = 0; i < nTabs; i++) {
        auto tab = GetTab(i);
        if (!tab->IsAboutTab()) {
            // logf("HasDocsLoaded: true because GetTab(i) !IsAboutTab()\n");
            return true;
        }
    }
    // logf("HasDocsLoaded: false because all %d tabs are IsAboutTab()\n", nTabs);
    return false;
}

bool MainWindow::IsCurrentTabAbout() const {
    return nullptr == CurrentTab() || CurrentTab()->IsAboutTab();
}

bool MainWindow::IsDocLoaded() const {
    bool isLoaded = (ctrl != nullptr);
    bool isTabLoaded = (CurrentTab() && CurrentTab()->ctrl != nullptr);
    if (isLoaded != isTabLoaded) {
        logfa("MainWindow::IsDocLoaded(): isLoaded: %d, isTabLoaded: %d\n", (int)isLoaded, (int)isTabLoaded);
        ReportIf(!gPluginMode);
    }
    return isLoaded;
}

WindowTab* MainWindow::CurrentTab() const {
    WindowTab* curr = currentTabTemp;
    if (curr != nullptr) {
        return curr;
    }
    if (!tabsCtrl) {
        return nullptr;
    }
    int i = tabsCtrl->GetSelected();
    if (i >= 0) {
        curr = GetTab(i);
        return curr;
    }
#if 0
    int nTabs = TabCount();
    ReportIf(nTabs > 0);
    if (nTabs > 0) {
        curr = GetTab(0);
        return curr;
    }
#endif
    return nullptr;
}

int MainWindow::TabCount() const {
    return tabsCtrl->TabCount();
}

WindowTab* MainWindow::GetTab(int idx) const {
    WindowTab* tab = GetTabsUserData<WindowTab*>(tabsCtrl, idx);
    return tab;
}

int MainWindow::GetTabIdx(WindowTab* tab) const {
    int nTabs = tabsCtrl->TabCount();
    for (int i = 0; i < nTabs; i++) {
        WindowTab* t = GetTabsUserData<WindowTab*>(tabsCtrl, i);
        if (t == tab) {
            return i;
        }
    }
    return -1;
}

Vec<WindowTab*> MainWindow::Tabs() const {
    Vec<WindowTab*> res;
    int nTabs = tabsCtrl->TabCount();
    for (int i = 0; i < nTabs; i++) {
        WindowTab* tab = GetTabsUserData<WindowTab*>(tabsCtrl, i);
        res.Append(tab);
    }
    return res;
}

DisplayModel* MainWindow::AsFixed() const {
    return ctrl ? ctrl->AsFixed() : nullptr;
}

ChmModel* MainWindow::AsChm() const {
    return ctrl ? ctrl->AsChm() : nullptr;
}

// Notify both display model and double-buffer (if they exist)
// about a potential change of available canvas size
void MainWindow::UpdateCanvasSize() {
    Rect rc = ClientRect(hwndCanvas);
    if (buffer && canvasRc == rc) {
        return;
    }
    canvasRc = rc;

    // create a new output buffer and notify the model
    // about the change of the canvas size
    delete buffer;
    buffer = new DoubleBuffer(hwndCanvas, canvasRc);

    if (IsDocLoaded()) {
        // the display model needs to know the full size (including scroll bars)
        ctrl->SetViewPortSize(GetViewPortSize());
    }
    if (CurrentTab()) {
        CurrentTab()->canvasRc = canvasRc;
    }

    RelayoutNotifications(hwndCanvas);
}

Size MainWindow::GetViewPortSize() const {
    Size size = canvasRc.Size();
    ReportDebugIf(size.IsEmpty());

    DWORD style = GetWindowLong(hwndCanvas, GWL_STYLE);
    if ((style & WS_VSCROLL)) {
        size.dx += GetSystemMetrics(SM_CXVSCROLL);
    }
    if ((style & WS_HSCROLL)) {
        size.dy += GetSystemMetrics(SM_CYHSCROLL);
    }
    ReportIf((style & (WS_VSCROLL | WS_HSCROLL)) && !AsFixed());
    return size;
}

void MainWindow::RedrawAll(bool update) const {
    // logf("MainWindow::RedrawAll, update: %d  RenderCache:\n", (int)update);
    InvalidateRect(this->hwndCanvas, nullptr, false);
    if (update) {
        UpdateWindow(this->hwndCanvas);
    }
}

void MainWindow::RedrawAllIncludingNonClient() const {
    // logf("MainWindow::RedrawAllIncludingNonClient RenderCache:\n");
    InvalidateRect(this->hwndCanvas, nullptr, false);
    RedrawWindow(this->hwndCanvas, nullptr, nullptr, RDW_FRAME | RDW_INVALIDATE);
}

void MainWindow::ChangePresentationMode(PresentationMode mode) {
    presentation = mode;
    if (PM_BLACK_SCREEN == mode || PM_WHITE_SCREEN == mode) {
        DeleteToolTip();
    }
    RedrawAll();
}

bool MainWindow::InPresentation() const {
    return presentation != PM_DISABLED;
}

static HWND FindModalOwnedBy(HWND hwndParent) {
    HWND hwnd = nullptr;
    while (true) {
        hwnd = FindWindowExW(HWND_DESKTOP, hwnd, nullptr, nullptr);
        if (hwnd == nullptr) {
            break;
        }
        bool isDlg = (GetWindowStyle(hwnd) & WS_DLGFRAME) != 0;
        if (!isDlg) {
            continue;
        }
        if (GetWindow(hwnd, GW_OWNER) != hwndParent) {
            continue;
        }
        return hwnd;
    }
    return nullptr;
}

void MainWindow::Focus() const {
    HwndToForeground(hwndFrame);
    // set focus to an owned modal dialog if there is one
    HWND hwnd = FindModalOwnedBy(hwndFrame);
    if (hwnd != nullptr) {
        HwndSetFocus(hwnd);
        return;
    }
    HwndSetFocus(hwndFrame);
}

void MainWindow::ToggleZoom() const {
    if (CurrentTab()) {
        CurrentTab()->ToggleZoom();
    }
}

void MainWindow::MoveDocBy(int dx, int dy) const {
    ReportIf(!CurrentTab());
    CurrentTab()->MoveDocBy(dx, dy);
}

void MainWindow::ShowToolTip(const char* text, Rect& rc, bool multiline) const {
    if (str::IsEmpty(text)) {
        DeleteToolTip();
        return;
    }
    infotip->SetSingle(text, rc, multiline);
}

void MainWindow::DeleteToolTip() const {
    infotip->Delete();
}

bool MainWindow::CreateUIAProvider() {
    if (uiaProvider) {
        return true;
    }
    uiaProvider = new SumatraUIAutomationProvider(this->hwndCanvas);
    if (!uiaProvider) {
        return false;
    }
    // load data to provider
    if (AsFixed()) {
        uiaProvider->OnDocumentLoad(AsFixed());
    }
    return true;
}

void LinkHandler::GotoLink(IPageDestination* dest) {
    ReportIf(!win || win->linkHandler != this);
    if (!dest || !win || !win->IsDocLoaded()) {
        return;
    }

    Kind kind = dest->GetKind();

    if (kindDestinationScrollTo == kind) {
        // TODO: respect link->ld.gotor.new_window for PDF documents ?
        ScrollTo(dest);
        return;
    }
    if (kindDestinationLaunchURL == kind) {
        auto d = (PageDestinationURL*)dest;
        LaunchURL(d->url);
        return;
    }
    if (kindDestinationLaunchFile == kind) {
        PageDestinationFile* fileDest = (PageDestinationFile*)dest;
        this->LaunchFile(fileDest->path, dest);
        return;
    }
    if (kindDestinationLaunchEmbedded == kind) {
        // Not handled here. Must use context menu to trigger launching
        // embedded files
        return;
    }

    if (kindDestinationAttachment == kind) {
        // Not handled here. Must use context menu to trigger launching
        // embedded files
        return;
    }

    if (kindDestinationLaunchURL == kind) {
        return;
    }

    logf("LinkHandler::GotoLink: unhandled kind %s\n", kind);
    ReportIf(true);
}

void LinkHandler::ScrollTo(IPageDestination* dest) {
    ReportIf(!win || !win->ctrl || win->linkHandler != this);
    if (!dest || !win || !win->ctrl || !win->IsDocLoaded()) {
        return;
    }
    // TODO: this seems like a hack, there should be a better way
    // https://github.com/sumatrapdfreader/sumatrapdf/issues/3499
    ChmModel* chm = win->ctrl->AsChm();
    if (chm) {
        chm->HandleLink(dest, nullptr);
        return;
    }
    int pageNo = PageDestGetPageNo(dest);
    if (!win->ctrl->ValidPageNo(pageNo)) {
        return;
    }
    RectF rect = PageDestGetRect(dest);
    float zoom = PageDestGetZoom(dest);
    win->ctrl->ScrollTo(pageNo, rect, zoom);
}

void LinkHandler::LaunchURL(const char* uri) {
    if (!uri) {
        /* ignore missing URLs */;
        return;
    }

    char* path = str::DupTemp(uri);
    char* colon = str::FindChar(path, ':');
    char* hash = str::FindChar(path, '#');
    if (!colon || (hash && colon > hash)) {
        // treat relative URIs as file paths (without fragment identifier)
        if (hash) {
            *hash = '\0';
        }
        str::TransCharsInPlace(path, "/", "\\");
        url::DecodeInPlace(path);
        // LaunchFile will reject unsupported file types
        this->LaunchFile(path, nullptr);
    } else {
        // LaunchBrowser will reject unsupported URI schemes
        // TODO: support file URIs?
        SumatraLaunchBrowser(path);
    }
}

// for safety, only handle relative paths and only open them in SumatraPDF
// (unless they're of an allowed perceived type) and never launch any external
// file in plugin mode (where documents are supposed to be self-contained)
void LinkHandler::LaunchFile(const char* pathOrig, IPageDestination* remoteLink) {
    if (gPluginMode || !CanAccessDisk()) {
        return;
    }

    TempStr path = str::ReplaceTemp(pathOrig, "/", "\\");
    if (str::StartsWith(path, ".\\")) {
        path = path + 2;
    }

    TempStr fullPath = path;
    bool isAbsPath = str::StartsWith(path, "\\");
    if (str::Len(path) >= 2 && path[1] == ':') {
        /* technically c: is not abs, only c:\\ */
        isAbsPath = true;
    }
#if 0
    // we used to not allow absolute links due to security, but if we can open
    // the doc we should assume we can handle it securely
    if (isAbsPath) {
        return;
    }
#endif
    if (!isAbsPath) {
        auto dir = path::GetDirTemp(win->ctrl->GetFilePath());
        fullPath = path::JoinTemp(dir, path);
    }
    path::Type pathType = path::GetType(fullPath);
    if (pathType == path::Type::None) {
        auto win = gWindows[0];
        ShowErrorLoadingNotification(win, fullPath, true);
        return;
    }
    if (pathType == path::Type::Dir) {
        SumatraOpenPathInExplorer(fullPath);
        return;
    }

    // TODO: respect link->ld.gotor.new_window for PDF documents ?
    MainWindow* newWin = FindMainWindowByFile(fullPath, true);
    // TODO: don't show window until it's certain that there was no error
    if (!newWin) {
        LoadArgs args(fullPath, win);
        newWin = LoadDocument(&args);
        if (!newWin) {
            return;
        }
    }

    if (!newWin->IsDocLoaded()) {
        bool quitIfLast = false;
        CloseCurrentTab(newWin, quitIfLast);
        // OpenFileExternally rejects files we'd otherwise
        // have to show a notification to be sure (which we
        // consider bad UI and thus simply don't)
        bool ok = OpenFileExternally(fullPath);
        if (!ok) {
            ShowErrorLoadingNotification(newWin, fullPath, true);
        }
        return;
    }

    newWin->Focus();
    if (!remoteLink) {
        return;
    }

    char* destName = PageDestGetName(remoteLink);
    if (destName) {
        IPageDestination* dest = newWin->ctrl->GetNamedDest(destName);
        if (dest) {
            newWin->linkHandler->ScrollTo(dest);
            delete dest;
        }
    } else {
        newWin->linkHandler->ScrollTo(remoteLink);
    }
}

// normalizes case and whitespace in the string
// caller needs to free() the result
static char* NormalizeFuzzy(const char* str) {
    char* dup = str::Dup(str);
    str::ToLowerInPlace(dup);
    str::NormalizeWSInPlace(dup);
    // cf. AddTocItemToView
    return dup;
}

static bool MatchFuzzy(const char* s1, const char* s2, bool partially) {
    if (!partially) {
        return str::Eq(s1, s2);
    }

    // only match at the start of a word (at the beginning and after a space)
    for (const char* last = s1; (last = str::Find(last, s2)) != nullptr; last++) {
        if (last == s1 || *(last - 1) == ' ') {
            return true;
        }
    }
    return false;
}

// finds the first ToC entry that (partially) matches a given normalized name
// (ignoring case and whitespace differences)
IPageDestination* LinkHandler::FindTocItem(TocItem* item, const char* name, bool partially) {
    for (; item; item = item->next) {
        if (item->title) {
            AutoFreeStr fuzTitle = NormalizeFuzzy(item->title);
            if (MatchFuzzy(fuzTitle, name, partially)) {
                return item->GetPageDestination();
            }
        }
        IPageDestination* dest = FindTocItem(item->child, name, partially);
        if (dest) {
            return dest;
        }
    }
    return nullptr;
}

void LinkHandler::GotoNamedDest(const char* name) {
    ReportIf(!win || win->linkHandler != this);
    DocController* ctrl = win->ctrl;
    if (!ctrl) {
        return;
    }

    // Match order:
    // 1. Exact match on internal destination name
    // 2. Fuzzy match on full ToC item title
    // 3. Fuzzy match on a part of a ToC item title
    // 4. Exact match on page label
    IPageDestination* dest = ctrl->GetNamedDest(name);
    bool hasDest = dest != nullptr;
    if (dest) {
        ScrollTo(dest);
        delete dest;
    } else if (ctrl->HasToc()) {
        auto* docTree = ctrl->GetToc();
        TocItem* root = docTree->root;
        AutoFreeStr fuzName = NormalizeFuzzy(name);
        dest = FindTocItem(root, fuzName, false);
        if (!dest) {
            dest = FindTocItem(root, fuzName, true);
        }
        if (dest) {
            ScrollTo(dest);
            hasDest = true;
        }
    }
    if (!hasDest && ctrl->HasPageLabels()) {
        int pageNo = ctrl->GetPageByLabel(name);
        if (ctrl->ValidPageNo(pageNo)) {
            ctrl->GoToPage(pageNo, true);
        }
    }
}

void UpdateControlsColors(MainWindow* win) {
    COLORREF bgCol = ThemeControlBackgroundColor();
    COLORREF txtCol = ThemeWindowTextColor();

    // logfa("retrieved doc colors in tree control: 0x%x 0x%x\n", treeTxtCol, treeBgCol);

    COLORREF splitterCol = GetSysColor(COLOR_BTNFACE);
    bool flatTreeWnd = false;

    {
        auto tocTreeView = win->tocTreeView;
        tocTreeView->SetColors(txtCol, bgCol);

        win->tocLabelWithClose->SetColors(txtCol, bgCol);
        win->sidebarSplitter->SetColors(kColorNoChange, splitterCol);
        SetWindowExStyle(tocTreeView->hwnd, WS_EX_STATICEDGE, !flatTreeWnd);
        uint flags = SWP_NOSIZE | SWP_NOMOVE | SWP_NOZORDER | SWP_FRAMECHANGED;
        SetWindowPos(tocTreeView->hwnd, nullptr, 0, 0, 0, 0, flags);
    }

    auto favTreeView = win->favTreeView;
    if (favTreeView) {
        favTreeView->SetColors(txtCol, bgCol);
        win->favLabelWithClose->SetColors(txtCol, bgCol);
        win->favSplitter->SetColors(kColorNoChange, splitterCol);

        SetWindowExStyle(favTreeView->hwnd, WS_EX_STATICEDGE, !flatTreeWnd);
        uint flags = SWP_NOSIZE | SWP_NOMOVE | SWP_NOZORDER | SWP_FRAMECHANGED;
        SetWindowPos(favTreeView->hwnd, nullptr, 0, 0, 0, 0, flags);
    }
    // TODO: more work needed to to ensure consistent look of the ebook window:
    // - change the tree item text color
    // - change the tree item background color when selected (for both focused and non-focused cases)
    // - ultimately implement owner-drawn scrollbars in a simpler style (like Chrome or VS 2013)
    //   and match their colors as well
}

void ClearFindBox(MainWindow* win) {
    HWND hwndFocused = GetFocus();
    if (hwndFocused == win->hwndFindEdit) {
        HwndSetFocus(win->hwndFrame);
    }
    HwndSetText(win->hwndFindEdit, "");
}

bool IsRightDragging(MainWindow* win) {
    if (win->mouseAction != MouseAction::Dragging) {
        return false;
    }
    return win->dragRightClick;
}

// sometimes we stash MainWindow pointer, do something on a thread and
// then go back on main thread to finish things. At that point MainWindow
// could have been destroyed so we need to check if it's still valid
bool IsMainWindowValid(MainWindow* win) {
    return gWindows.Contains(win);
}

MainWindow* FindMainWindowByHwnd(HWND hwnd) {
    if (!::IsWindow(hwnd)) {
        return nullptr;
    }
    for (MainWindow* win : gWindows) {
        if ((win->hwndFrame == hwnd) || ::IsChild(win->hwndFrame, hwnd)) {
            return win;
        }
    }
    return nullptr;
}

// Find MainWindow using WindowTab. Diffrent than WindowTab->win in that
// it validates that WindowTab is still valid
MainWindow* FindMainWindowByTab(WindowTab* tabToFind) {
    for (MainWindow* win : gWindows) {
        for (WindowTab* tab : win->Tabs()) {
            if (tab == tabToFind) {
                return win;
            }
        }
    }
    return nullptr;
}

MainWindow* FindMainWindowByController(DocController* ctrl) {
    for (auto& win : gWindows) {
        for (auto& tab : win->Tabs()) {
            if (tab->ctrl == ctrl) {
                return win;
            }
        }
    }
    return nullptr;
}

// temporarily highlight this tab
void HighlightTab(MainWindow* win, WindowTab* tab) {
    if (!win) {
        return;
    }
    int idx = -1;
    if (tab) {
        idx = win->GetTabIdx(tab);
    }
    win->tabsCtrl->SetHighlighted(idx);
}
