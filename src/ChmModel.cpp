/* Copyright 2022 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

#include "utils/BaseUtil.h"
#include "utils/Dict.h"
#include "utils/UITask.h"
#include "utils/ScopedWin.h"
#include "utils/WinUtil.h"

#include "wingui/HtmlWindow.h"
#include "wingui/UIModels.h"

#include "Settings.h"
#include "DisplayMode.h"
#include "DocController.h"
#include "EngineBase.h"
#include "EbookBase.h"
#include "ChmFile.h"
#include "GlobalPrefs.h"
#include "ChmModel.h"

#include "utils/Log.h"

static IPageDestination* NewChmNamedDest(const char* url, int pageNo) {
    if (!url) {
        return nullptr;
    }
    IPageDestination* dest = nullptr;
    if (IsExternalUrl(url)) {
        dest = new PageDestinationURL(url);
    } else {
        auto pdest = new PageDestination();
        pdest->kind = kindDestinationScrollTo;
        pdest->name = str::Dup(url);
        dest = pdest;
    }
    dest->pageNo = pageNo;
    ReportIf(!dest->kind);
    dest->rect = RectF(DEST_USE_DEFAULT, DEST_USE_DEFAULT, DEST_USE_DEFAULT, DEST_USE_DEFAULT);
    return dest;
}

static TocItem* NewChmTocItem(TocItem* parent, const char* title, int pageNo, const char* url) {
    auto res = new TocItem(parent, title, pageNo);
    res->dest = NewChmNamedDest(url, pageNo);
    return res;
}

class HtmlWindowHandler : public HtmlWindowCallback {
    ChmModel* cm;

  public:
    explicit HtmlWindowHandler(ChmModel* cm) : cm(cm) {
    }
    ~HtmlWindowHandler() override = default;

    bool OnBeforeNavigate(const char* url, bool newWindow) override {
        return cm->OnBeforeNavigate(url, newWindow);
    }
    void OnDocumentComplete(const char* url) override {
        cm->OnDocumentComplete(url);
    }
    void OnLButtonDown() override {
        cm->OnLButtonDown();
    }
    ByteSlice GetDataForUrl(const char* url) override {
        return cm->GetDataForUrl(url);
    }
    void DownloadData(const char* url, const ByteSlice& data) override {
        cm->DownloadData(url, data);
    }
};

struct ChmTocTraceItem {
    const char* title = nullptr; // owned by ChmModel::poolAllocator
    const char* url = nullptr;   // owned by ChmModel::poolAllocator
    int level = 0;
    int pageNo = 0;
};

ChmModel::ChmModel(DocControllerCallback* cb) : DocController(cb) {
    InitializeCriticalSection(&docAccess);
}

ChmModel::~ChmModel() {
    EnterCriticalSection(&docAccess);
    // TODO: deleting htmlWindow seems to spin a modal loop which
    //       can lead to WM_PAINT being dispatched for the parent
    //       hwnd and then crashing in SumatraPDF.cpp's DrawDocument
    delete htmlWindow;
    delete htmlWindowCb;
    delete doc;
    delete tocTrace;
    delete tocTree;
    DeleteVecMembers(urlDataCache);
    LeaveCriticalSection(&docAccess);
    DeleteCriticalSection(&docAccess);
}

const char* ChmModel::GetFilePath() const {
    return fileName;
}

const char* ChmModel::GetDefaultFileExt() const {
    return ".chm";
}

int ChmModel::PageCount() const {
    return pages.Size();
}

TempStr ChmModel::GetPropertyTemp(const char* name) {
    return doc->GetPropertyTemp(name);
}

int ChmModel::CurrentPageNo() const {
    return currentPageNo;
}

void ChmModel::GoToPage(int pageNo, bool) {
    ReportIf(!ValidPageNo(pageNo));
    if (!ValidPageNo(pageNo)) {
        return;
    }
    DisplayPage(pages.At(pageNo - 1));
}

bool ChmModel::SetParentHwnd(HWND hwnd) {
    ReportIf(htmlWindow || htmlWindowCb);
    htmlWindowCb = new HtmlWindowHandler(this);
    htmlWindow = HtmlWindow::Create(hwnd, htmlWindowCb);
    if (!htmlWindow) {
        delete htmlWindowCb;
        htmlWindowCb = nullptr;
        return false;
    }
    return true;
}

void ChmModel::RemoveParentHwnd() {
    delete htmlWindow;
    htmlWindow = nullptr;
    delete htmlWindowCb;
    htmlWindowCb = nullptr;
}

void ChmModel::PrintCurrentPage(bool showUI) const {
    if (htmlWindow) {
        htmlWindow->PrintCurrentPage(showUI);
    }
}

void ChmModel::FindInCurrentPage() const {
    if (htmlWindow) {
        htmlWindow->FindInCurrentPage();
    }
}

void ChmModel::SelectAll() const {
    if (htmlWindow) {
        htmlWindow->SelectAll();
    }
}

void ChmModel::CopySelection() const {
    if (htmlWindow) {
        htmlWindow->CopySelection();
    }
}

static bool gSendingHtmlWindowMsg = false;

LRESULT ChmModel::PassUIMsg(UINT msg, WPARAM wp, LPARAM lp) const {
    if (!htmlWindow || gSendingHtmlWindowMsg) {
        return 0;
    }
    gSendingHtmlWindowMsg = true;
    auto res = htmlWindow->SendMsg(msg, wp, lp);
    gSendingHtmlWindowMsg = false;
    return res;
}

bool ChmModel::DisplayPage(const char* pageUrl) {
    if (!pageUrl) {
        return false;
    }
    if (IsExternalUrl(pageUrl)) {
        // open external links in an external browser
        // (same as for PDF, XPS, etc. documents)
        if (cb) {
            // TODO: optimize, create just destination
            auto item = NewChmTocItem(nullptr, nullptr, 0, pageUrl);
            cb->GotoLink(item->dest);
            delete item;
        }
        return true;
    }

    TempStr url = url::GetFullPathTemp(pageUrl);
    int pageNo = pages.Find(url) + 1;
    if (pageNo > 0) {
        currentPageNo = pageNo;
    }

    // This is a hack that seems to be needed for some chm files where
    // url starts with "..\" even though it's not accepted by ie as
    // a correct its: url. There's a possibility it breaks some other
    // chm files (I don't know such cases, though).
    // A more robust solution would try to match with the actual
    // names of files inside chm package.
    if (str::StartsWith(pageUrl, "..\\")) {
        pageUrl += 3;
    }

    if (str::StartsWith(pageUrl, "/")) {
        pageUrl++;
    }

    htmlWindow->NavigateToDataUrl(pageUrl);
    return pageNo > 0;
}

void ChmModel::ScrollTo(int, RectF, float) {
    ReportIf(true);
}

bool ChmModel::HandleLink(IPageDestination* link, ILinkHandler*) {
    Kind k = link->GetKind();
    if (k != kindDestinationScrollTo) {
        logf("ChmModel::HandleLink: unsupported kind '%s'\n", k);
        ReportIfQuick(link->GetKind() != kindDestinationScrollTo);
    }
    char* url = PageDestGetName(link);
    if (DisplayPage(url)) {
        return true;
    }
    int pageNo = PageDestGetPageNo(link);
    GoToPage(pageNo, false);
    return true;
}

bool ChmModel::CanNavigate(int dir) const {
    if (!htmlWindow) {
        return false;
    }
    if (dir < 0) {
        return htmlWindow->canGoBack;
    }
    return htmlWindow->canGoForward;
}

void ChmModel::Navigate(int dir) {
    if (!htmlWindow) {
        return;
    }

    if (dir < 0) {
        for (; dir < 0 && CanNavigate(dir); dir++) {
            htmlWindow->GoBack();
        }
    } else {
        for (; dir > 0 && CanNavigate(dir); dir--) {
            htmlWindow->GoForward();
        }
    }
}

void ChmModel::SetDisplayMode(DisplayMode, bool) {
    // no-op
}

DisplayMode ChmModel::GetDisplayMode() const {
    return DisplayMode::SinglePage;
}

void ChmModel::SetInPresentation(bool) {
    // no-op
}

void ChmModel::SetViewPortSize(Size) {
    // no-op
}

ChmModel* ChmModel::AsChm() {
    return this;
}

void ChmModel::SetZoomVirtual(float zoom, Point*) {
    if (zoom > 0) {
        zoom = limitValue(zoom, kZoomMin, kZoomMax);
    }
    if (zoom <= 0 || !IsValidZoom(zoom)) {
        zoom = 100.0f;
    }
    ZoomTo(zoom);
    initZoom = zoom;
}

void ChmModel::ZoomTo(float zoomLevel) const {
    if (htmlWindow) {
        htmlWindow->SetZoomPercent((int)zoomLevel);
    }
}

float ChmModel::GetZoomVirtual(bool) const {
    if (!htmlWindow) {
        return 100;
    }
    return (float)htmlWindow->GetZoomPercent();
}

class ChmTocBuilder : public EbookTocVisitor {
    ChmFile* doc = nullptr;

    StrVec* pages = nullptr;
    Vec<ChmTocTraceItem>* tocTrace = nullptr;
    Allocator* allocator = nullptr;
    // TODO: could use dict::MapStrToInt instead of StrList in the caller as well
    dict::MapStrToInt urlsSet;

    // We fake page numbers by doing a depth-first traversal of
    // toc tree and considering each unique html page in toc tree
    // as a page
    int CreatePageNoForURL(const char* url) {
        if (!url || IsExternalUrl(url)) {
            return 0;
        }

        TempStr plainUrl = url::GetFullPathTemp(url);
        int pageNo = pages->Size() + 1;
        bool inserted = urlsSet.Insert(plainUrl, pageNo, &pageNo);
        if (inserted) {
            pages->Append(plainUrl);
            ReportIf(pageNo != pages->Size());
        } else {
            ReportIf(pageNo == pages->Size() + 1);
        }
        return pageNo;
    }

  public:
    ChmTocBuilder(ChmFile* doc, StrVec* pages, Vec<ChmTocTraceItem>* tocTrace, Allocator* allocator) {
        this->doc = doc;
        this->pages = pages;
        this->tocTrace = tocTrace;
        this->allocator = allocator;
        int n = pages->Size();
        for (int i = 0; i < n; i++) {
            const char* url = pages->At(i);
            bool inserted = urlsSet.Insert(url, i + 1, nullptr);
            ReportIf(!inserted);
        }
    }

    void Visit(const char* name, const char* url, int level) override {
        name = str::Dup(allocator, name, (size_t)-1);
        url = str::Dup(allocator, url, (size_t)-1);
        int pageNo = CreatePageNoForURL(url);
        auto item = ChmTocTraceItem{name, url, level, pageNo};
        tocTrace->Append(item);
    }
};

bool ChmModel::Load(const char* fileName) {
    this->fileName.SetCopy(fileName);
    doc = ChmFile::CreateFromFile(fileName);
    if (!doc) {
        return false;
    }

    // always make the document's homepage page 1
    char* page = strconv::AnsiToUtf8(doc->GetHomePath());
    pages.Append(page);
    str::Free(page);

    // parse the ToC here, since page numbering depends on it
    tocTrace = new Vec<ChmTocTraceItem>();
    ChmTocBuilder tmpTocBuilder(doc, &pages, tocTrace, &poolAlloc);
    doc->ParseToc(&tmpTocBuilder);
    ReportIf(pages.Size() == 0);
    return pages.Size() > 0;
}

struct ChmCacheEntry {
    // owned by ChmModel::poolAllocator
    const char* url = nullptr;
    ByteSlice data;

    explicit ChmCacheEntry(const char* url);
    ~ChmCacheEntry() {
        data.Free();
    };
};

ChmCacheEntry::ChmCacheEntry(const char* url) {
    this->url = url;
}

ChmCacheEntry* ChmModel::FindDataForUrl(const char* url) const {
    size_t n = urlDataCache.size();
    for (size_t i = 0; i < n; i++) {
        ChmCacheEntry* e = urlDataCache.at(i);
        if (str::Eq(url, e->url)) {
            return e;
        }
    }
    return nullptr;
}

// Called after html document has been loaded.
// Sync the state of the ui with the page (show
// the right page number, select the right item in toc tree)
void ChmModel::OnDocumentComplete(const char* url) {
    if (!url || IsBlankUrl(url)) {
        return;
    }
    if (*url == '/') {
        ++url;
    }
    TempStr toFind = url::GetFullPathTemp(url);
    int pageNo = pages.Find(toFind) + 1;
    if (!pageNo) {
        return;
    }
    currentPageNo = pageNo;
    // TODO: setting zoom before the first page is loaded seems not to work
    // (might be a regression from between r4593 and r4629)
    if (IsValidZoom(initZoom)) {
        SetZoomVirtual(initZoom, nullptr);
        initZoom = kInvalidZoom;
    }
    if (cb) {
        cb->PageNoChanged(this, pageNo);
    }
}

// Called before we start loading html for a given url. Will block
// loading if returns false.
bool ChmModel::OnBeforeNavigate(const char* url, bool newWindow) {
    // ensure that JavaScript doesn't keep the focus
    // in the HtmlWindow when a new page is loaded
    if (cb) {
        cb->FocusFrame(false);
    }

    if (!newWindow) {
        return true;
    }

    // don't allow new MSIE windows to be opened
    // instead pass the URL to the system's default browser
    if (url && cb) {
        // TODO: optimize, create just destination
        auto item = NewChmTocItem(nullptr, nullptr, 0, url);
        cb->GotoLink(item->dest);
        delete item;
    }
    return false;
}

// Load and cache data for a given url inside CHM file.
ByteSlice ChmModel::GetDataForUrl(const char* url) {
    ScopedCritSec scope(&docAccess);
    TempStr plainUrl = url::GetFullPathTemp(url);
    ChmCacheEntry* e = FindDataForUrl(plainUrl);
    if (!e) {
        char* s = str::Dup(&poolAlloc, plainUrl);
        e = new ChmCacheEntry(s);
        e->data = doc->GetData(plainUrl);
        if (e->data.empty()) {
            delete e;
            return {};
        }
        urlDataCache.Append(e);
    }
    return e->data;
}

void ChmModel::DownloadData(const char* url, const ByteSlice& data) {
    if (!cb) {
        return;
    }
    cb->SaveDownload(url, data);
}

void ChmModel::OnLButtonDown() {
    if (cb) {
        cb->FocusFrame(true);
    }
}

// named destinations are either in-document URLs or Alias topic IDs
IPageDestination* ChmModel::GetNamedDest(const char* name) {
    TempStr url = url::GetFullPathTemp(name);
    int pageNo = pages.Find(url) + 1;
    if (pageNo >= 1) {
        return NewChmNamedDest(url, pageNo);
    }
    if (doc->HasData(url)) {
        return NewChmNamedDest(url, 1);
    }
    unsigned int topicID;
    if (!str::Parse(name, "%u%$", &topicID)) {
        return nullptr;
    }
    char* topicURL = doc->ResolveTopicID(topicID);
    if (!topicURL) {
        return nullptr;
    }
    url = str::DupTemp(topicURL);
    str::Free(topicURL);
    if (!doc->HasData(url)) {
        return nullptr;
    }
    pageNo = pages.Find(url) + 1;
    if (pageNo < 1) {
        // some documents use redirection URLs which aren't listed in the ToC
        // return pageNo=1 for these, as HandleLink will ignore that anyway
        // but LinkHandler::ScrollTo doesn't
        pageNo = 1;
    }
    return NewChmNamedDest(url, pageNo);
}

TocTree* ChmModel::GetToc() {
    if (tocTree) {
        return tocTree;
    }
    if (tocTrace->size() == 0) {
        return nullptr;
    }

    TocItem* root = nullptr;
    bool foundRoot = false;
    TocItem** nextChild = &root;
    Vec<TocItem*> levels;
    int idCounter = 0;

    for (ChmTocTraceItem& ti : *tocTrace) {
        // TODO: set parent
        TocItem* item = NewChmTocItem(nullptr, ti.title, ti.pageNo, ti.url);
        item->id = ++idCounter;
        // append the item at the correct level
        ReportIf(ti.level < 1);
        if ((size_t)ti.level <= levels.size()) {
            levels.RemoveAt(ti.level, levels.size() - ti.level);
            levels.Last()->AddSiblingAtEnd(item);
        } else {
            *nextChild = item;
            levels.Append(item);
            foundRoot = true;
        }
        nextChild = &item->child;
    }
    if (!foundRoot) {
        return nullptr;
    }
    auto realRoot = new TocItem();
    realRoot->child = root;
    tocTree = new TocTree(realRoot);
    return tocTree;
}

// adapted from DisplayModel::NextZoomStep
float ChmModel::GetNextZoomStep(float towardsLevel) const {
    float currZoom = GetZoomVirtual(true);
    if (MaybeGetNextZoomByIncrement(&currZoom, towardsLevel)) {
        // chm uses browser control which only supports integer zoom levels
        // this ensures we're not stuck on a given zoom level i.e. advance by at least 1%
        int iCurrZoom2 = (int)GetZoomVirtual(true);
        int iCurrZoom = (int)currZoom;
        if (iCurrZoom == iCurrZoom2) {
            currZoom += 1.f;
        }
        return currZoom;
    }

    int nZoomLevels;
    float* zoomLevels = GetDefaultZoomLevels(&nZoomLevels);

    // chm uses browser control which only supports integer zoom levels
    // this ensures we're not stuck on a given zoom level
    // due to float => int truncation
    int iCurrZoom = (int)currZoom;
    int iTowardsLevel = (int)towardsLevel;
    int iNewZoom = iTowardsLevel;
    if (iCurrZoom < towardsLevel) {
        for (int i = 0; i < nZoomLevels; i++) {
            int iZoom = (int)zoomLevels[i];
            if (iZoom > iCurrZoom) {
                iNewZoom = iZoom;
                break;
            }
        }
    } else if (iCurrZoom > towardsLevel) {
        for (int i = nZoomLevels - 1; i >= 0; i--) {
            int iZoom = (int)zoomLevels[i];
            if (iZoom < iCurrZoom) {
                iNewZoom = iZoom;
                break;
            }
        }
    }

    return (float)iNewZoom;
}

void ChmModel::GetDisplayState(FileState* fs) {
    char* fileNameA = fileName;
    if (!fs->filePath || !str::EqI(fs->filePath, fileNameA)) {
        SetFileStatePath(fs, fileNameA);
    }

    fs->useDefaultState = !gGlobalPrefs->rememberStatePerDocument;

    str::ReplaceWithCopy(&fs->displayMode, DisplayModeToString(GetDisplayMode()));
    ZoomToString(&fs->zoom, GetZoomVirtual(), fs);

    fs->pageNo = CurrentPageNo();
    fs->scrollPos = PointF();
}

struct ChmThumbnailTask : HtmlWindowCallback {
    ChmFile* doc = nullptr;
    HWND hwnd = nullptr;
    HtmlWindow* hw = nullptr;
    bool didSave = false;
    Size size;
    const OnBitmapRendered* saveThumbnail = nullptr;
    AutoFreeStr homeUrl;
    Vec<ByteSlice> data;
    CRITICAL_SECTION docAccess;

    ChmThumbnailTask(ChmFile* doc, HWND hwnd, Size size, const OnBitmapRendered* saveThumbnail);
    ~ChmThumbnailTask() override;
    void StartCreateThumbnail(HtmlWindow* hw);
    bool OnBeforeNavigate(const char*, bool newWindow) override;
    void OnDocumentComplete(const char* url) override;
    ByteSlice GetDataForUrl(const char* url) override;
    void OnLButtonDown() override;
    void DownloadData(const char*, const ByteSlice&) override;
};

static void SafeDeleteChmThumbnailTask(ChmThumbnailTask* d) {
    logf("SafeDeleteChmThumbnailTask: about to delete ChmThumbnailTask: 0x%p\n", (void*)d);
    delete d;
}

ChmThumbnailTask::ChmThumbnailTask(ChmFile* doc, HWND hwnd, Size size, const OnBitmapRendered* saveThumbnail) {
    this->doc = doc;
    this->hwnd = hwnd;
    this->size = size;
    this->saveThumbnail = saveThumbnail;
    this->didSave = false;
    InitializeCriticalSection(&docAccess);
}

ChmThumbnailTask::~ChmThumbnailTask() {
    EnterCriticalSection(&docAccess);
    delete hw;
    DestroyWindow(hwnd);
    delete doc;
    for (auto&& d : data) {
        str::Free(d.data());
    }
    LeaveCriticalSection(&docAccess);
    DeleteCriticalSection(&docAccess);
}

bool ChmThumbnailTask::OnBeforeNavigate(const char*, bool newWindow) {
    return !newWindow;
}

void ChmThumbnailTask::StartCreateThumbnail(HtmlWindow* hw) {
    this->hw = hw;
    homeUrl.Set(strconv::AnsiToUtf8(doc->GetHomePath()));
    if (*homeUrl == '/') {
        homeUrl.SetCopy(homeUrl + 1);
    }
    hw->NavigateToDataUrl(homeUrl);
}

ByteSlice ChmThumbnailTask::GetDataForUrl(const char* url) {
    ScopedCritSec scope(&docAccess);
    char* plainUrl = url::GetFullPathTemp(url);
    auto d = doc->GetData(plainUrl);
    data.Append(d);
    return d;
}

void ChmThumbnailTask::OnDocumentComplete(const char* url) {
    if (url && *url == '/') {
        url++;
    }
    if (!str::Eq(url, homeUrl)) {
        return;
    }
    logf("ChmThumbnailTask::OnDocumentComplete: '%s'\n", url);
    if (didSave) {
        // maybe prevent crash generating .chm thumbnails
        // https://github.com/sumatrapdfreader/sumatrapdf/issues/4519
        ReportIfQuick(didSave);
        return;
    }
    Rect area(0, 0, size.dx * 2, size.dy * 2);
    HBITMAP hbmp = hw->TakeScreenshot(area, size);
    if (hbmp) {
        RenderedBitmap* bmp = new RenderedBitmap(hbmp, size);
        saveThumbnail->Call(bmp);
    }
    // delay deleting because ~ChmThumbnailTask() deletes HtmlWindow
    // and we're currently processing HtmlWindow messages
    // TODO: it's possible we still have timing issue
    auto fn = MkFunc0<ChmThumbnailTask>(SafeDeleteChmThumbnailTask, this);
    uitask::Post(fn, "SafeDeleteChmThumbnailTask");
}

void ChmThumbnailTask::OnLButtonDown() {
}

void ChmThumbnailTask::DownloadData(const char*, const ByteSlice&) {
}

static void CreateChmThumbnail(const char* path, const Size& size, const OnBitmapRendered* saveThumbnail) {
    // doc and window will be destroyed by the callback once it's invoked
    ChmFile* doc = ChmFile::CreateFromFile(path);
    if (!doc) {
        return;
    }

    // We render twice the size of thumbnail and scale it down
    int dx = size.dx * 2 + GetSystemMetrics(SM_CXVSCROLL);
    int dy = size.dy * 2 + GetSystemMetrics(SM_CYHSCROLL);
    // reusing WC_STATIC. I don't think exact class matters (WndProc
    // will be taken over by HtmlWindow anyway) but it can't be nullptr.
    HWND hwnd =
        CreateWindowExW(0, WC_STATIC, L"BrowserCapture", WS_POPUP, 0, 0, dx, dy, nullptr, nullptr, nullptr, nullptr);
    if (!hwnd) {
        delete doc;
        return;
    }
#if 0 // when debugging set to 1 to see the window
    ShowWindow(hwnd, SW_SHOW);
#endif

    ChmThumbnailTask* thumbnailTask = new ChmThumbnailTask(doc, hwnd, size, saveThumbnail);
    HtmlWindow* hw = HtmlWindow::Create(hwnd, thumbnailTask);
    if (!hw) {
        delete thumbnailTask;
        return;
    }
    // is deleted in ChmThumbnailTask::OnDocumentComplete
    thumbnailTask->StartCreateThumbnail(hw);
}

// Create a thumbnail of chm document by loading it again and rendering
// its first page to a hwnd specially created for it.
void ChmModel::CreateThumbnail(Size size, const OnBitmapRendered* saveThumbnail) {
    CreateChmThumbnail(fileName, size, saveThumbnail);
}

bool ChmModel::IsSupportedFileType(Kind kind) {
    return ChmFile::IsSupportedFileType(kind);
}

ChmModel* ChmModel::Create(const char* fileName, DocControllerCallback* cb) {
    ChmModel* cm = new ChmModel(cb);
    if (!cm->Load(fileName)) {
        delete cm;
        return nullptr;
    }
    return cm;
}
