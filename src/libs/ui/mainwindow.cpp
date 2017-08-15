/****************************************************************************
**
** Copyright (C) 2015-2016 Oleg Shparber
** Copyright (C) 2013-2014 Jerzy Kozera
** Contact: https://go.zealdocs.org/l/contact
**
** This file is part of Zeal.
**
** Zeal is free software: you can redistribute it and/or modify
** it under the terms of the GNU General Public License as published by
** the Free Software Foundation, either version 3 of the License, or
** (at your option) any later version.
**
** Zeal is distributed in the hope that it will be useful,
** but WITHOUT ANY WARRANTY; without even the implied warranty of
** MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
** GNU General Public License for more details.
**
** You should have received a copy of the GNU General Public License
** along with Zeal. If not, see <https://www.gnu.org/licenses/>.
**
****************************************************************************/

#include "mainwindow.h"
#include "ui_mainwindow.h"

#include "aboutdialog.h"
#include "docsetsdialog.h"
#include "searchitemdelegate.h"
#include "settingsdialog.h"
#include "qxtglobalshortcut/qxtglobalshortcut.h"

#include <core/application.h>
#include <core/settings.h>
#include <registry/docset.h>
#include <registry/docsetregistry.h>
#include <registry/itemdatarole.h>
#include <registry/listmodel.h>
#include <registry/searchmodel.h>

#include <QCloseEvent>
#include <QDesktopServices>
#include <QFileInfo>
#include <QKeyEvent>
#include <QMenu>
#include <QMessageBox>
#include <QScrollBar>
#include <QShortcut>
#include <QSystemTrayIcon>
#include <QTabBar>
#include <QTimer>
#include <QWebFrame>
#include <QWebHistory>
#include <QWebPage>

using namespace Zeal;
using namespace Zeal::WidgetUi;

namespace {
const char startPageUrl[] = "qrc:///browser/welcome.html";
const char DarkModeCssUrl[] = ":/browser/assets/css/darkmode.css";
const char HighlightOnNavigateCssUrl[] = ":/browser/assets/css/highlight.css";
}

namespace Zeal {
namespace WidgetUi {

struct TabState
{
    explicit TabState()
    {
        searchModel = new Registry::SearchModel();
        tocModel = new Registry::SearchModel();

        webPage = new QWebPage();
        webPage->setLinkDelegationPolicy(QWebPage::DelegateExternalLinks);
        webPage->setNetworkAccessManager(Core::Application::instance()->networkManager());
    }

    TabState(const TabState &other)
        : searchQuery(other.searchQuery)
        , selections(other.selections)
        , expansions(other.expansions)
        , searchScrollPosition(other.searchScrollPosition)
        , tocScrollPosition(other.tocScrollPosition)
        , webViewZoomFactor(other.webViewZoomFactor)
    {
        searchModel = new Registry::SearchModel(*other.searchModel);
        tocModel = new Registry::SearchModel(*other.tocModel);

        webPage = new QWebPage();
        webPage->setLinkDelegationPolicy(QWebPage::DelegateExternalLinks);
        webPage->setNetworkAccessManager(Core::Application::instance()->networkManager());

        restoreHistory(other.saveHistory());
    }

    ~TabState()
    {
        delete searchModel;
        delete tocModel;
        // deleteLater() prevents crashing on quit (#577)
        webPage->deleteLater();
    }

    void restoreHistory(const QByteArray &array) const
    {
        QDataStream stream(array);
        stream >> *webPage->history();
    }

    QByteArray saveHistory() const
    {
        QByteArray array;
        QDataStream stream(&array, QIODevice::WriteOnly);
        stream << *webPage->history();
        return array;
    }

    QUrl url() const {
        return webPage->mainFrame()->url();
    }

    void loadUrl(const QUrl &url)
    {
        webPage->mainFrame()->load(url);
    }

    QString title() const
    {
        return webPage->mainFrame()->title();
    }

    QString searchQuery;

    // Content/Search results tree view state
    Registry::SearchModel *searchModel = nullptr;
    QModelIndexList selections;
    QModelIndexList expansions;
    int searchScrollPosition = 0;

    // TOC list view state
    Registry::SearchModel *tocModel = nullptr;
    int tocScrollPosition = 0;

    QWebPage *webPage = nullptr;
    int webViewZoomFactor = 0;
};

} // namespace WidgetUi
} // namespace Zeal

MainWindow::MainWindow(Core::Application *app, QWidget *parent) :
    QMainWindow(parent),
    ui(new Ui::MainWindow),
    m_application(app),
    m_settings(app->settings()),
    m_zealListModel(new Registry::ListModel(app->docsetRegistry(), this)),
    m_globalShortcut(new QxtGlobalShortcut(m_settings->showShortcut, this)),
    m_openDocsetTimer(new QTimer(this))
{
    ui->setupUi(this);

    // initialise key grabber
    connect(m_globalShortcut, &QxtGlobalShortcut::activated, this, &MainWindow::toggleWindow);

    setupTabBar();

    QShortcut *focusSearch = new QShortcut(QStringLiteral("Ctrl+K"), this);
    connect(focusSearch, &QShortcut::activated,
            ui->lineEdit, static_cast<void (SearchEdit::*)()>(&SearchEdit::setFocus));

    QShortcut *duplicate = new QShortcut(QStringLiteral("Ctrl+Alt+T"), this);
    connect(duplicate, &QShortcut::activated, this, [this]() { duplicateTab(m_tabBar->currentIndex()); });

    restoreGeometry(m_settings->windowGeometry);
    ui->splitter->restoreState(m_settings->verticalSplitterGeometry);

    // Menu
    // File
    // Some platform plugins do not define QKeySequence::Quit.
    if (QKeySequence(QKeySequence::Quit).isEmpty())
        ui->actionQuit->setShortcut(QStringLiteral("Ctrl+Q"));
    else
        ui->actionQuit->setShortcut(QKeySequence::Quit);

    connect(ui->actionQuit, &QAction::triggered, qApp, &QCoreApplication::quit);

    // Edit
    ui->actionFind->setShortcut(QKeySequence::Find);
    connect(ui->actionFind, &QAction::triggered, ui->webView, &SearchableWebView::showSearchBar);

    connect(ui->actionPreferences, &QAction::triggered, [this]() {
        m_globalShortcut->setEnabled(false);
        QScopedPointer<SettingsDialog> dialog(new SettingsDialog(m_application, this));
        dialog->exec();
        m_globalShortcut->setEnabled(true);
    });

    ui->actionBack->setShortcut(QKeySequence::Back);
    addAction(ui->actionBack);
    ui->actionForward->setShortcut(QKeySequence::Forward);
    addAction(ui->actionForward);
    connect(ui->actionBack, &QAction::triggered, ui->webView, &SearchableWebView::back);
    connect(ui->actionForward, &QAction::triggered, ui->webView, &SearchableWebView::forward);

    // Tools Menu
    connect(ui->actionDocsets, &QAction::triggered, [this]() {
        QScopedPointer<DocsetsDialog> dialog(new DocsetsDialog(m_application, this));
        dialog->exec();
    });

    // Help Menu
    connect(ui->actionSubmitFeedback, &QAction::triggered, [this]() {
        QDesktopServices::openUrl(QUrl(QStringLiteral("https://github.com/zealdocs/zeal/issues")));
    });
    connect(ui->actionCheckForUpdates, &QAction::triggered,
            m_application, &Core::Application::checkForUpdates);
    connect(ui->actionAboutZeal, &QAction::triggered, [this]() {
        QScopedPointer<AboutDialog> dialog(new AboutDialog(this));
        dialog->exec();
    });
    connect(ui->actionAboutQt, &QAction::triggered, [this]() {
        QMessageBox::aboutQt(this);
    });

    // Update check
    connect(m_application, &Core::Application::updateCheckError, [this](const QString &message) {
        QMessageBox::warning(this, QStringLiteral("Zeal"), message);
    });

    connect(m_application, &Core::Application::updateCheckDone, [this](const QString &version) {
        if (version.isEmpty()) {
            QMessageBox::information(this, QStringLiteral("Zeal"),
                                     tr("You are using the latest version."));
            return;
        }

        const int ret
                = QMessageBox::information(this, QStringLiteral("Zeal"),
                                           tr("Zeal <b>%1</b> is available. Open download page?").arg(version),
                                           QMessageBox::Yes | QMessageBox::Default,
                                           QMessageBox::No | QMessageBox::Escape,
                                           QMessageBox::NoButton);
        if (ret == QMessageBox::Yes)
            QDesktopServices::openUrl(QUrl(QStringLiteral("https://zealdocs.org/download.html")));
    });

    m_backMenu = new QMenu(ui->backButton);
    connect(m_backMenu, &QMenu::aboutToShow, this, [this]() {
        m_backMenu->clear();
        QWebHistory *history = currentTabState()->webPage->history();
        QList<QWebHistoryItem> items = history->backItems(10);
        // TODO: [Qt 5.6]
        //for (auto it = items.crbegin(); it != items.crend(); ++it) {
        for (auto it = items.cend() - 1; it >= items.cbegin(); --it) {
            const QIcon icon = docsetIcon(docsetName(it->url()));
            const QWebHistoryItem item = *it;
            // TODO: [Qt 5.6]
            // m_backMenu->addAction(icon, it->title(), [=](bool) { history->goToItem(item); });
            QAction *action = m_backMenu->addAction(icon, it->title());
            connect(action, &QAction::triggered, [=](bool) { history->goToItem(item); });
        }
    });
    ui->backButton->setDefaultAction(ui->actionBack);
    ui->backButton->setMenu(m_backMenu);

    m_forwardMenu = new QMenu(ui->forwardButton);
    connect(m_forwardMenu, &QMenu::aboutToShow, this, [this]() {
        m_forwardMenu->clear();
        QWebHistory *history = currentTabState()->webPage->history();
        for (const QWebHistoryItem &item: history->forwardItems(10)) {
            const QIcon icon = docsetIcon(docsetName(item.url()));
            // TODO: [Qt 5.6]
            //m_forwardMenu->addAction(icon, item.title(), [=](bool) { history->goToItem(item); });
            QAction *action = m_forwardMenu->addAction(icon, item.title());
            connect(action, &QAction::triggered, [=](bool) { history->goToItem(item); });
        }
    });
    ui->forwardButton->setDefaultAction(ui->actionForward);
    ui->forwardButton->setMenu(m_forwardMenu);

    // treeView and lineEdit
    ui->lineEdit->setTreeView(ui->treeView);
    ui->lineEdit->setFocus();
    setupSearchBoxCompletions();
    SearchItemDelegate *delegate = new SearchItemDelegate(ui->treeView);
    delegate->setDecorationRoles({Registry::ItemDataRole::DocsetIconRole, Qt::DecorationRole});
    connect(ui->lineEdit, &QLineEdit::textChanged, [delegate](const QString &text) {
        delegate->setHighlight(Registry::SearchQuery::fromString(text).query());
    });
    ui->treeView->setItemDelegate(delegate);

    ui->tocListView->setItemDelegate(new SearchItemDelegate(ui->tocListView));
    connect(ui->tocSplitter, &QSplitter::splitterMoved, this, [this]() {
        m_settings->tocSplitterState = ui->tocSplitter->saveState();
    });

    createTab();

    connect(ui->treeView, &QTreeView::clicked, this, &MainWindow::openDocset);
    connect(ui->tocListView, &QListView::clicked, this, &MainWindow::openDocset);
    connect(ui->treeView, &QTreeView::activated, this, &MainWindow::openDocset);
    connect(ui->tocListView, &QListView::activated, this, &MainWindow::openDocset);

    connect(ui->webView, &SearchableWebView::urlChanged, [this](const QUrl &url) {
        const QString name = docsetName(url);
        m_tabBar->setTabIcon(m_tabBar->currentIndex(), docsetIcon(name));

        Registry::Docset *docset = m_application->docsetRegistry()->docset(name);
        if (docset)
            currentTabState()->tocModel->setResults(docset->relatedLinks(url));

        ui->actionBack->setEnabled(ui->webView->canGoBack());
        ui->actionForward->setEnabled(ui->webView->canGoForward());
    });

    connect(ui->webView, &SearchableWebView::titleChanged, [this](const QString &title) {
        if (title.isEmpty())
            return;

        setWindowTitle(QStringLiteral("%1 - Zeal").arg(title));
        m_tabBar->setTabText(m_tabBar->currentIndex(), title);
        m_tabBar->setTabToolTip(m_tabBar->currentIndex(), title);
    });

    connect(ui->webView, &SearchableWebView::linkClicked, [this](const QUrl &url) {
        const QString message = tr("Do you want to open an external link?<br>URL: <b>%1</b>");
        int ret = QMessageBox::question(this, QStringLiteral("Zeal"), message.arg(url.toString()));
        if (ret == QMessageBox::Yes)
            QDesktopServices::openUrl(url);
    });

    connect(m_application->docsetRegistry(), &Registry::DocsetRegistry::searchCompleted,
            this, [this](const QList<Registry::SearchResult> &results) {
        currentTabState()->searchModel->setResults(results);
    });

    connect(m_application->docsetRegistry(), &Registry::DocsetRegistry::docsetAboutToBeRemoved,
            this, [this](const QString &name) {
        for (TabState *tabState : m_tabStates) {
            if (tabState == currentTabState()) {
                // Disable updates because removeSearchResultWithName can
                // call {begin,end}RemoveRows multiple times, and cause
                // degradation of UI responsiveness.
                ui->treeView->setUpdatesEnabled(false);
                tabState->searchModel->removeSearchResultWithName(name);
                ui->treeView->setUpdatesEnabled(true);
            } else {
                tabState->searchModel->removeSearchResultWithName(name);
            }

            if (docsetName(tabState->url()) == name) {
                tabState->tocModel->setResults();
                tabState->loadUrl(QUrl(startPageUrl));
            }

            // TODO: Cleanup history
        }

        setupSearchBoxCompletions();
    });

    connect(m_application->docsetRegistry(), &Registry::DocsetRegistry::docsetAdded,
            this, [this](const QString &) {
        setupSearchBoxCompletions();
    });

    connect(ui->lineEdit, &QLineEdit::textChanged, [this](const QString &text) {
        if (text == currentTabState()->searchQuery)
            return;

        currentTabState()->searchQuery = text;
        m_application->docsetRegistry()->search(text);
    });

    // Setup delayed navigation to a page until user makes a pause in typing a search query.
    m_openDocsetTimer->setInterval(400);
    m_openDocsetTimer->setSingleShot(true);
    connect(m_openDocsetTimer, &QTimer::timeout, this, [this]() {
        QModelIndex index = m_openDocsetTimer->property("index").toModelIndex();
        if (!index.isValid())
            return;

        openDocset(index);

        // Get focus back.
        ui->lineEdit->setFocus(Qt::MouseFocusReason);
    });

    ui->actionNewTab->setShortcut(QKeySequence::AddTab);
    connect(ui->actionNewTab, &QAction::triggered, this, [this]() { createTab(); });
    addAction(ui->actionNewTab);

    // Save expanded items
    connect(ui->treeView, &QTreeView::expanded, [this](QModelIndex index) {
        if (currentTabState()->expansions.indexOf(index) == -1)
            currentTabState()->expansions.append(index);
    });

    connect(ui->treeView, &QTreeView::collapsed, [this](QModelIndex index) {
        currentTabState()->expansions.removeOne(index);
    });

#ifdef Q_OS_WIN32
    ui->actionCloseTab->setShortcut(QKeySequence(Qt::Key_W + Qt::CTRL));
#else
    ui->actionCloseTab->setShortcut(QKeySequence::Close);
#endif
    addAction(ui->actionCloseTab);
    connect(ui->actionCloseTab, &QAction::triggered, this, [this]() { closeTab(); });

    connect(ui->openUrlButton, &QPushButton::clicked, [this]() {
        const QUrl url(ui->webView->page()->history()->currentItem().url());
        if (url.scheme() != QLatin1String("qrc"))
            QDesktopServices::openUrl(url);
    });

    ui->actionNextTab->setShortcuts({QKeySequence::NextChild,
                                     QKeySequence(Qt::ControlModifier| Qt::Key_PageDown)});
    addAction(ui->actionNextTab);
    connect(ui->actionNextTab, &QAction::triggered, [this]() {
        m_tabBar->setCurrentIndex((m_tabBar->currentIndex() + 1) % m_tabBar->count());
    });

    // TODO: Use QKeySequence::PreviousChild, when QTBUG-15746 is fixed.
    ui->actionPreviousTab->setShortcuts({QKeySequence(Qt::ControlModifier | Qt::ShiftModifier | Qt::Key_Tab),
                                         QKeySequence(Qt::ControlModifier| Qt::Key_PageUp)});
    addAction(ui->actionPreviousTab);
    connect(ui->actionPreviousTab, &QAction::triggered, [this]() {
        m_tabBar->setCurrentIndex((m_tabBar->currentIndex() - 1 + m_tabBar->count()) % m_tabBar->count());
    });

#ifdef Q_OS_OSX
    ui->treeView->setAttribute(Qt::WA_MacShowFocusRect, false);
    ui->tocListView->setAttribute(Qt::WA_MacShowFocusRect, false);
#endif

    connect(m_settings, &Core::Settings::updated, this, &MainWindow::applySettings);
    applySettings();

    if (m_settings->checkForUpdate)
        m_application->checkForUpdates(true);
}

MainWindow::~MainWindow()
{
    m_settings->verticalSplitterGeometry = ui->splitter->saveState();
    m_settings->windowGeometry = saveGeometry();

    // Delete the UI first, because it depends on tab states.
    delete ui;
    qDeleteAll(m_tabStates);
}

void MainWindow::search(const Registry::SearchQuery &query)
{
    if (query.isEmpty())
        return;

    ui->lineEdit->setText(query.toString());
    emit ui->treeView->activated(ui->treeView->currentIndex());
}

void MainWindow::openDocset(const QModelIndex &index)
{
    const QVariant url = index.data(Registry::ItemDataRole::UrlRole);
    if (url.isNull())
        return;

    ui->webView->load(url.toUrl());
    ui->webView->focus();
}

QString MainWindow::docsetName(const QUrl &url) const
{
    const QRegExp docsetRegex(QStringLiteral("/([^/]+)[.]docset"));
    return docsetRegex.indexIn(url.path()) != -1 ? docsetRegex.cap(1) : QString();
}

QIcon MainWindow::docsetIcon(const QString &docsetName) const
{
    Registry::Docset *docset = m_application->docsetRegistry()->docset(docsetName);
    return docset ? docset->icon() : QIcon(QStringLiteral(":/icons/logo/icon.png"));
}

void MainWindow::queryCompleted()
{
    m_openDocsetTimer->stop();

    syncTreeView();

    ui->treeView->setCurrentIndex(currentTabState()->searchModel->index(0, 0, QModelIndex()));

    m_openDocsetTimer->setProperty("index", ui->treeView->currentIndex());
    m_openDocsetTimer->start();
}

void MainWindow::closeTab(int index)
{
    if (index == -1)
        index = m_tabBar->currentIndex();

    if (index == -1)
        return;

    delete m_tabStates.takeAt(index);

    m_tabBar->removeTab(index);

    if (m_tabStates.isEmpty())
        createTab();
}

void MainWindow::createTab(int index)
{
    if (m_settings->openNewTabAfterActive)
        index = m_tabBar->currentIndex() + 1;
    else if (index == -1)
        index = m_tabStates.size();

    using Registry::SearchModel;
    TabState *newTab = new TabState();
    connect(newTab->searchModel, &SearchModel::updated, this, &MainWindow::queryCompleted);
    connect(newTab->tocModel, &SearchModel::updated, this, &MainWindow::syncToc);

    newTab->loadUrl(QUrl(startPageUrl));

    m_tabStates.insert(index, newTab);
    m_tabBar->insertTab(index, tr("Loading..."));
    m_tabBar->setCurrentIndex(index);

    ui->lineEdit->setFocus();
}

void MainWindow::duplicateTab(int index)
{
    if (index < 0 || index >= m_tabStates.size())
        return;

    using Registry::SearchModel;
    TabState *newTab = new TabState(*m_tabStates.at(index));
    connect(newTab->searchModel, &SearchModel::updated, this, &MainWindow::queryCompleted);
    connect(newTab->tocModel, &SearchModel::updated, this, &MainWindow::syncToc);

    ++index;
    m_tabStates.insert(index, newTab);
    m_tabBar->insertTab(index, newTab->title());
    m_tabBar->setCurrentIndex(index);
}

void MainWindow::syncTreeView()
{
    TabState *tabState = currentTabState();

    if (!tabState->searchQuery.isEmpty()) {
        ui->treeView->setModel(tabState->searchModel);
        ui->treeView->setRootIsDecorated(false);
    } else {
        ui->treeView->setModel(m_zealListModel);
        ui->treeView->setColumnHidden(1, true);
        ui->treeView->setRootIsDecorated(true);
    }

    ui->treeView->reset();
}

void MainWindow::syncToc()
{
    if (!currentTabState()->tocModel->isEmpty()) {
        ui->tocListView->show();
        ui->tocSplitter->restoreState(m_settings->tocSplitterState);
    } else {
        ui->tocListView->hide();
    }

}

TabState *MainWindow::currentTabState() const
{
    return m_tabStates.at(m_tabBar->currentIndex());
}

// Sets up the search box autocompletions.
void MainWindow::setupSearchBoxCompletions()
{
    QStringList completions;
    for (const Registry::Docset * const docset: m_application->docsetRegistry()->docsets()) {
        if (docset->keywords().isEmpty())
            continue;

        completions << docset->keywords().first() + QLatin1Char(':');
    }

    ui->lineEdit->setCompletions(completions);
}

void MainWindow::setupTabBar()
{
    m_tabBar = new QTabBar(this);

    m_tabBar->installEventFilter(this);

    m_tabBar->setTabsClosable(true);
    m_tabBar->setSelectionBehaviorOnRemove(QTabBar::SelectPreviousTab);
    m_tabBar->setExpanding(false);
    m_tabBar->setUsesScrollButtons(true);
    m_tabBar->setDrawBase(false);
    m_tabBar->setDocumentMode(true);
    m_tabBar->setElideMode(Qt::ElideRight);
    m_tabBar->setStyleSheet(QStringLiteral("QTabBar::tab { min-width: 150px; }"));

    connect(m_tabBar, &QTabBar::currentChanged, this, [this](int index) {
        static const char PreviousTabIndexProperty[] = "previousTabIndex";

        if (index == -1)
            return;

        // Save previous tab state
        const QVariant previousTabIndex = m_tabBar->property(PreviousTabIndexProperty);
        if (previousTabIndex.isValid() && previousTabIndex.toInt() < m_tabStates.size()) {
            TabState *previousTabState = m_tabStates.at(previousTabIndex.toInt());
            previousTabState->selections = ui->treeView->selectionModel()->selectedIndexes();
            previousTabState->searchScrollPosition = ui->treeView->verticalScrollBar()->value();
            previousTabState->tocScrollPosition = ui->tocListView->verticalScrollBar()->value();
            previousTabState->webViewZoomFactor = ui->webView->zoomFactor();
        }

        // Load current tab state
        m_tabBar->setProperty(PreviousTabIndexProperty, index);
        TabState *tabState = m_tabStates.at(index);

        ui->lineEdit->setText(tabState->searchQuery);
        ui->tocListView->setModel(tabState->tocModel);

        syncTreeView();
        syncToc();

        // Bring back the selections and expansions
        ui->treeView->blockSignals(true);
        for (const QModelIndex &selection: tabState->selections)
            ui->treeView->selectionModel()->select(selection, QItemSelectionModel::Select);
        for (const QModelIndex &expandedIndex: tabState->expansions)
            ui->treeView->expand(expandedIndex);
        ui->treeView->blockSignals(false);

        ui->webView->setPage(tabState->webPage);
        ui->webView->setZoomFactor(tabState->webViewZoomFactor);

        ui->actionBack->setEnabled(ui->webView->canGoBack());
        ui->actionForward->setEnabled(ui->webView->canGoForward());

        ui->treeView->verticalScrollBar()->setValue(tabState->searchScrollPosition);
        ui->tocListView->verticalScrollBar()->setValue(tabState->tocScrollPosition);
    });
    connect(m_tabBar, &QTabBar::tabCloseRequested, this, &MainWindow::closeTab);

    for (int i = 1; i < 10; i++) {
        QAction *action = new QAction(m_tabBar);
#ifdef Q_OS_LINUX
        action->setShortcut(QStringLiteral("Alt+%1").arg(i));
#else
        action->setShortcut(QStringLiteral("Ctrl+%1").arg(i));
#endif
        if (i == 9) {
            connect(action, &QAction::triggered, [=]() {
                m_tabBar->setCurrentIndex(m_tabBar->count() - 1);
            });
        } else {
            connect(action, &QAction::triggered, [=]() {
                m_tabBar->setCurrentIndex(i - 1);
            });
        }

        addAction(action);
    }

    QHBoxLayout *layout = reinterpret_cast<QHBoxLayout *>(ui->navigationBar->layout());
    layout->insertWidget(2, m_tabBar, 0, Qt::AlignBottom);
}

void MainWindow::createTrayIcon()
{
    if (m_trayIcon)
        return;

    m_trayIcon = new QSystemTrayIcon(this);
    m_trayIcon->setIcon(windowIcon());
    m_trayIcon->setToolTip(QStringLiteral("Zeal"));

    connect(m_trayIcon, &QSystemTrayIcon::activated, [this](QSystemTrayIcon::ActivationReason reason) {
        if (reason != QSystemTrayIcon::Trigger && reason != QSystemTrayIcon::DoubleClick)
            return;

        toggleWindow();
    });

    QMenu *trayIconMenu = new QMenu(this);
    trayIconMenu->addAction(ui->actionQuit);

    m_trayIcon->setContextMenu(trayIconMenu);

    m_trayIcon->show();
}

void MainWindow::removeTrayIcon()
{
    if (!m_trayIcon)
        return;

    QMenu *trayIconMenu = m_trayIcon->contextMenu();
    delete m_trayIcon;
    m_trayIcon = nullptr;
    delete trayIconMenu;
}

void MainWindow::bringToFront()
{
    show();
    setWindowState((windowState() & ~Qt::WindowMinimized) | Qt::WindowActive);
    raise();
    activateWindow();

    ui->lineEdit->setFocus();
}

void MainWindow::changeEvent(QEvent *event)
{
    if (m_settings->showSystrayIcon && m_settings->minimizeToSystray
            && event->type() == QEvent::WindowStateChange && isMinimized()) {
        hide();
    }
    QMainWindow::changeEvent(event);
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    if (m_settings->showSystrayIcon && m_settings->hideOnClose) {
        event->ignore();
        toggleWindow();
    }
}

bool MainWindow::eventFilter(QObject *object, QEvent *event)
{
    if (object == m_tabBar) {
        switch (event->type()) {
        case QEvent::MouseButtonRelease: {
            QMouseEvent *e = reinterpret_cast<QMouseEvent *>(event);
            if (e->button() == Qt::MiddleButton) {
                const int index = m_tabBar->tabAt(e->pos());
                if (index != -1) {
                    closeTab(index);
                    return true;
                }
            }
            break;
        }
        case QEvent::Wheel:
            // TODO: Remove in case QTBUG-8428 is fixed on all platforms
            return true;
        default:
            break;
        }
    }

    return QMainWindow::eventFilter(object, event);
}

// Captures global events in order to pass them to the search bar.
void MainWindow::keyPressEvent(QKeyEvent *keyEvent)
{
    switch (keyEvent->key()) {
    case Qt::Key_Escape:
        ui->lineEdit->setFocus();
        ui->lineEdit->clearQuery();
        break;
    case Qt::Key_Question:
        ui->lineEdit->setFocus();
        ui->lineEdit->selectQuery();
        break;
    default:
        QMainWindow::keyPressEvent(keyEvent);
        break;
    }
}

void MainWindow::applySettings()
{
    m_globalShortcut->setShortcut(m_settings->showShortcut);

    if (m_settings->showSystrayIcon)
        createTrayIcon();
    else
        removeTrayIcon();

    // Content
    QByteArray ba;
    if (m_settings->darkModeEnabled) {
        QScopedPointer<QFile> file(new QFile(DarkModeCssUrl));
        if (file->open(QIODevice::ReadOnly)) {
            ba += file->readAll();
        }
    }

    if (m_settings->highlightOnNavigateEnabled) {
        QScopedPointer<QFile> file(new QFile(HighlightOnNavigateCssUrl));
        if (file->open(QIODevice::ReadOnly)) {
            ba += file->readAll();
        }
    }

    if (QFileInfo::exists(m_settings->customCssFile)) {
        QScopedPointer<QFile> file(new QFile(m_settings->customCssFile));
        if (file->open(QIODevice::ReadOnly)) {
            ba += file->readAll();
        }
    }

    const QString cssUrl = QLatin1String("data:text/css;charset=utf-8;base64,") + ba.toBase64();
    QWebSettings::globalSettings()->setUserStyleSheetUrl(QUrl(cssUrl));
}

void MainWindow::toggleWindow()
{
    const bool checkActive = sender() == m_globalShortcut;

    if (!isVisible() || (checkActive && !isActiveWindow())) {
        bringToFront();
    } else {
        if (m_trayIcon) {
            hide();
        } else {
            showMinimized();
        }
    }
}
