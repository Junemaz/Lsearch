// Lsearch GUI (Qt5)：Everything 风格的桌面搜索界面（V2）
// 复用 ipc/client 访问 lsearchd（自动拉起守护进程），不直接触碰 core。
// 特性：无系统边框(自绘深色标题栏+拖拽+缩放) + 实时搜索 + 表格 + 工具栏 + 托盘常驻。
#include "core/config.h"
#include "core/entry.h"
#include "core/util.h"
#include "ipc/client.h"

#include <QAction>
#include <QApplication>
#include <QBrush>
#include <QCloseEvent>
#include <QColor>
#include <QDesktopServices>
#include <QDir>
#include <QEvent>
#include <QFrame>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QIcon>
#include <QKeySequence>
#include <QLabel>
#include <QLineEdit>
#include <QMainWindow>
#include <QMenu>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPainter>
#include <QPalette>
#include <QPixmap>
#include <QProcess>
#include <QStatusBar>
#include <QSystemTrayIcon>
#include <QTableWidget>
#include <QTimer>
#include <QToolBar>
#include <QToolButton>
#include <QUrl>
#include <QVBoxLayout>
#include <QWidget>

#include <atomic>
#include <condition_variable>
#include <cstdlib>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr int kColPath = 0;
constexpr int kColType = 1;
constexpr int kColSize = 2;
constexpr int kColMtime = 3;
constexpr int kMaxRows = 2000;
constexpr int kEdge = 6;  // 无边框窗口的边缘缩放手感区

QString humanSize(int64_t n) { return QString::fromStdString(lsearch::humanSize(n)); }
QString isoTime(int64_t t) { return QString::fromStdString(lsearch::isoTime(t)); }

void xdgOpen(const QString& path) { QProcess::startDetached("xdg-open", QStringList() << path); }

QIcon makeAppIcon() {
  QPixmap pm(64, 64);
  pm.fill(Qt::transparent);
  QPainter p(&pm);
  p.setRenderHint(QPainter::Antialiasing);
  p.setBrush(QColor(30, 36, 51));
  p.setPen(Qt::NoPen);
  p.drawRoundedRect(0, 0, 64, 64, 14, 14);
  p.setPen(QPen(QColor(97, 175, 239), 7, Qt::SolidLine, Qt::RoundCap));
  p.setBrush(QColor(15, 18, 25));
  p.drawEllipse(14, 14, 30, 30);
  p.setPen(QPen(QColor(97, 175, 239), 8, Qt::SolidLine, Qt::RoundCap));
  p.drawLine(QPointF(39, 39), QPointF(51, 51));
  p.end();
  return QIcon(pm);
}

void applyModernTheme(QApplication& app) {
  app.setStyle("Fusion");
  QPalette pal;
  const QColor window(30, 36, 51), base(15, 18, 25), alt(27, 34, 48);
  const QColor text(232, 234, 240), faint(154, 167, 189), accent(97, 175, 239);
  const QColor border(51, 60, 78), toolbg(35, 43, 59);
  pal.setColor(QPalette::Window, window);
  pal.setColor(QPalette::WindowText, text);
  pal.setColor(QPalette::Base, base);
  pal.setColor(QPalette::AlternateBase, alt);
  pal.setColor(QPalette::Text, text);
  pal.setColor(QPalette::Button, toolbg);
  pal.setColor(QPalette::ButtonText, text);
  pal.setColor(QPalette::BrightText, Qt::white);
  pal.setColor(QPalette::Highlight, accent);
  pal.setColor(QPalette::HighlightedText, QColor(255, 255, 255));
  pal.setColor(QPalette::ToolTipBase, toolbg);
  pal.setColor(QPalette::ToolTipText, text);
  pal.setColor(QPalette::Light, border);
  pal.setColor(QPalette::Midlight, QColor(45, 53, 70));
  pal.setColor(QPalette::Mid, QColor(40, 48, 63));
  pal.setColor(QPalette::Dark, QColor(24, 29, 41));
  pal.setColor(QPalette::Shadow, QColor(10, 12, 18));
#if QT_VERSION >= QT_VERSION_CHECK(5, 12, 0)
  pal.setColor(QPalette::PlaceholderText, faint);
#endif
  pal.setColor(QPalette::Link, accent);
  app.setPalette(pal);

  app.setStyleSheet(R"(
    QMainWindow, QDialog { background: #1e2433; }
    #titleBar { background: #171c28; border-bottom: 1px solid #2c3450; }
    #titleText { font-weight: 600; font-size: 13px; }
    #btnMin, #btnClose {
      background: transparent; color: #9aa7bd;
      border: none; border-radius: 4px; font-size: 13px;
    }
    #btnMin:hover { background: #2c456e; color: #e8eaf0; }
    #btnClose:hover { background: #e05561; color: #ffffff; }
    QLineEdit {
      background: #0f1219; color: #e8eaf0;
      border: 1px solid #333c4e; border-radius: 8px;
      padding: 8px 12px; font-size: 14px;
      selection-background-color: #61afef; selection-color: #ffffff;
    }
    QLineEdit:focus { border: 1px solid #61afef; }
    QHeaderView::section {
      background: #232b3b; color: #9aa7bd; border: none;
      border-bottom: 1px solid #333c4e;
      padding: 6px 8px; font-weight: 600;
    }
    QTableWidget, QAbstractScrollArea {
      background: #171c28; alternate-background-color: #1b2230;
      color: #e8eaf0; border: none; gridline-color: transparent;
      selection-background-color: #2c456e; selection-color: #ffffff;
      outline: none;
    }
    QTableWidget::item { padding: 4px 6px; border: none; }
    QToolBar {
      background: #232b3b; border: none;
      border-bottom: 1px solid #333c4e;
      spacing: 4px; padding: 4px;
    }
    QToolButton { background: transparent; color: #e8eaf0; border: none; border-radius: 5px; padding: 5px 12px; }
    QToolButton:hover { background: #2c456e; }
    QToolButton:checked, QToolButton:pressed { background: #2c456e; color: #ffffff; }
    QToolBar::separator { background: #333c4e; width: 1px; margin: 4px 6px; }
    QStatusBar { background: #232b3b; color: #9aa7bd; }
    QMenu {
      background: #232b3b; color: #e8eaf0;
      border: 1px solid #333c4e; border-radius: 6px; padding: 4px;
    }
    QMenu::item { padding: 6px 20px; border-radius: 4px; }
    QMenu::item:selected { background: #2c456e; color: #ffffff; }
    QMenu::separator { background: #333c4e; height: 1px; margin: 4px 8px; }
    QToolTip { background: #232b3b; color: #e8eaf0; border: 1px solid #333c4e; }
    QLabel { color: #e8eaf0; }
  )");
}

}  // namespace

class MainWindow : public QMainWindow {
  Q_OBJECT

 public:
  MainWindow() {
    setWindowTitle("Lsearch");
    setWindowIcon(makeAppIcon());
    // 无系统边框：白框/系统标题栏一去不返，全部自绘
    setWindowFlags(Qt::Window | Qt::FramelessWindowHint);
    resize(920, 640);
    setMouseTracking(true);

    buildUi();
    setupTray();

    worker_ = std::thread([this] { searchLoop(); });
  }

  ~MainWindow() override {
    running_ = false;
    cv_.notify_all();
    if (worker_.joinable()) worker_.join();
    if (tray_) tray_->hide();
  }

 protected:
  void closeEvent(QCloseEvent* e) override {
    if (tray_ && !quitting_) {
      e->ignore();
      hide();
      tray_->showMessage("Lsearch", "已最小化到托盘，双击托盘图标可恢复。",
                         QSystemTrayIcon::Information, 2000);
    } else {
      e->accept();
    }
  }

  // 无边框窗口：拖拽/缩放统一入口（过滤标题栏与主要子控件）
  bool eventFilter(QObject* obj, QEvent* ev) override {
    if (ev->type() == QEvent::MouseButtonPress || ev->type() == QEvent::MouseMove ||
        ev->type() == QEvent::MouseButtonRelease || ev->type() == QEvent::MouseButtonDblClick) {
      auto* m = static_cast<QMouseEvent*>(ev);
      QPoint winPos = (obj == this) ? m->pos() : static_cast<QWidget*>(obj)->mapTo(this, m->pos());
      if (handleTopLevelMouse(static_cast<QWidget*>(obj), m, winPos)) return true;
    }
    return QMainWindow::eventFilter(obj, ev);
  }

 private slots:
  void onQueryChanged(const QString&) { timer_->start(); }

  void onOpen(int row, int) {
    auto* item = table_->item(row, kColPath);
    if (item && !item->text().isEmpty()) xdgOpen(item->text());
  }

  void runSearch() {
    std::string q = input_->text().toStdString();
    {
      std::lock_guard<std::mutex> lk(gmut_);
      query_ = std::move(q);
      pending_ = true;
    }
    cv_.notify_one();
  }

 private:
  bool handleTopLevelMouse(QWidget* src, QMouseEvent* m, const QPoint& winPos) {
    if (m->button() == Qt::LeftButton && m->type() == QEvent::MouseButtonPress) {
      // 边缘 -> 开始缩放
      edges_ = 0;
      if (winPos.x() <= kEdge) edges_ |= 1;        // W
      if (width() - winPos.x() <= kEdge) edges_ |= 2;  // E
      if (winPos.y() <= kEdge) edges_ |= 4;        // N
      if (height() - winPos.y() <= kEdge) edges_ |= 8;  // S
      if (edges_) {
        resizing_ = true;
        pressGlobal_ = m->globalPos();
        startGeo_ = geometry();
        return true;
      }
      // 标题栏空白区 -> 开始拖动
      if (src == titleBar_) {
        dragging_ = true;
        dragOffset_ = m->globalPos() - frameGeometry().topLeft();
        return true;
      }
    } else if (m->type() == QEvent::MouseButtonDblClick && src == titleBar_) {
      if (isMaximized()) showNormal(); else showMaximized();
      return true;
    } else if (m->type() == QEvent::MouseMove) {
      if (resizing_ && (m->buttons() & Qt::LeftButton)) {
        QPoint d = m->globalPos() - pressGlobal_;
        QRect g = startGeo_;
        if (edges_ & 2) g.setWidth(startGeo_.width() + d.x());                // E
        if (edges_ & 1) { g.setX(startGeo_.x() + d.x()); g.setWidth(startGeo_.width() - d.x()); }  // W
        if (edges_ & 8) g.setHeight(startGeo_.height() + d.y());              // S
        if (edges_ & 4) { g.setY(startGeo_.y() + d.y()); g.setHeight(startGeo_.height() - d.y()); }  // N
        if (g.width() < 420) g.setWidth(420);
        if (g.height() < 320) g.setHeight(320);
        setGeometry(g);
        return true;
      }
      if (dragging_ && (m->buttons() & Qt::LeftButton)) {
        move(m->globalPos() - dragOffset_);
        return true;
      }
      // 悬浮边缘时给缩放光标
      int e = 0;
      if (winPos.x() <= kEdge) e |= 1;
      if (width() - winPos.x() <= kEdge) e |= 2;
      if (winPos.y() <= kEdge) e |= 4;
      if (height() - winPos.y() <= kEdge) e |= 8;
      Qt::CursorShape cur = Qt::ArrowCursor;
      if (e == (1 | 2)) cur = Qt::SizeHorCursor;
      else if (e == (4 | 8)) cur = Qt::SizeVerCursor;
      else if (e == (1 | 4) || e == (2 | 8)) cur = Qt::SizeFDiagCursor;
      else if (e == (2 | 4) || e == (1 | 8)) cur = Qt::SizeBDiagCursor;
      else if (e & 1 || e & 2) cur = Qt::SizeHorCursor;
      else if (e & 4 || e & 8) cur = Qt::SizeVerCursor;
      src->setCursor(cur);
      if (!e) src->unsetCursor();
      return false;  // 边缘之外继续交给子控件
    } else if (m->type() == QEvent::MouseButtonRelease) {
      if (resizing_) { resizing_ = false; edges_ = 0; src->unsetCursor(); return true; }
      if (dragging_) { dragging_ = false; return true; }
    }
    return false;
  }

  void buildUi() {
    // ---- 自绘标题栏 ----
    titleBar_ = new QWidget(this);
    titleBar_->setObjectName("titleBar");
    titleBar_->setFixedHeight(38);
    titleBar_->setMouseTracking(true);
    auto* tl = new QHBoxLayout(titleBar_);
    tl->setContentsMargins(10, 0, 6, 0);
    tl->setSpacing(8);
    auto* icon = new QLabel(titleBar_);
    icon->setPixmap(makeAppIcon().pixmap(18, 18));
    auto* title = new QLabel("Lsearch", titleBar_);
    title->setObjectName("titleText");
    minBtn_ = new QToolButton(titleBar_);
    minBtn_->setObjectName("btnMin");
    minBtn_->setText("—");
    minBtn_->setFixedSize(34, 26);
    closeBtn_ = new QToolButton(titleBar_);
    closeBtn_->setObjectName("btnClose");
    closeBtn_->setText("✕");
    closeBtn_->setFixedSize(34, 26);
    connect(minBtn_, &QToolButton::clicked, this, &MainWindow::showMinimized);
    connect(closeBtn_, &QToolButton::clicked, this, [this] { close(); });
    tl->addWidget(icon);
    tl->addWidget(title);
    tl->addStretch(1);
    tl->addWidget(minBtn_);
    tl->addWidget(closeBtn_);
    setMenuWidget(titleBar_);  // 位于中央区之上，天然排在工具栏上方

    input_ = new QLineEdit(this);
    input_->setPlaceholderText("输入关键词…（仅匹配文件名，大小写不敏感；* ? 为通配符）");
    input_->setMouseTracking(true);

    table_ = new QTableWidget(this);
    table_->setColumnCount(4);
    table_->setHorizontalHeaderLabels({"路径", "类型", "大小", "修改时间"});
    table_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    table_->setSelectionMode(QAbstractItemView::SingleSelection);
    table_->setSortingEnabled(true);
    table_->setAlternatingRowColors(true);
    table_->setShowGrid(false);
    table_->setFrameShape(QFrame::NoFrame);
    table_->verticalHeader()->setVisible(false);
    table_->setColumnWidth(1, 60);
    table_->setColumnWidth(2, 80);
    table_->setColumnWidth(3, 150);
    table_->setMouseTracking(true);

    auto* central = new QWidget(this);
    auto* lay = new QVBoxLayout(central);
    lay->setContentsMargins(8, 8, 8, 4);
    lay->setSpacing(6);
    lay->addWidget(input_);
    lay->addWidget(table_);
    setCentralWidget(central);

    statusBar()->showMessage("连接 lsearchd …");

    // ---- 工具栏（Everything 式操作）----
    auto* tb = addToolBar("工具");
    tb->setMovable(false);
    tb->setMouseTracking(true);

    QAction* rebuildAct = tb->addAction("重建索引");
    rebuildAct->setShortcut(QKeySequence("Ctrl+R"));
    connect(rebuildAct, &QAction::triggered, this, [this] { doRebuild(); });

    tb->addSeparator();

    dirsAct_ = tb->addAction("仅目录");
    dirsAct_->setCheckable(true);
    filesAct_ = tb->addAction("仅文件");
    filesAct_->setCheckable(true);
    connect(dirsAct_, &QAction::toggled, this, [this](bool on) {
      if (on) filesAct_->setChecked(false);
      onFilterChanged();
    });
    connect(filesAct_, &QAction::toggled, this, [this](bool on) {
      if (on) dirsAct_->setChecked(false);
      onFilterChanged();
    });

    tb->addSeparator();

    QAction* statsAct = tb->addAction("索引统计");
    statsAct->setShortcut(QKeySequence("Ctrl+I"));
    connect(statsAct, &QAction::triggered, this, [this] { showStats(); });

    QAction* cfgAct = tb->addAction("打开配置");
    connect(cfgAct, &QAction::triggered, this, [this] {
      std::string f = lsearch::Config::load("").config_file;
      QDir dir(QString::fromStdString(lsearch::dirName(f)));
      QDesktopServices::openUrl(QUrl::fromLocalFile(dir.path()));
    });

    tb->addSeparator();
    QAction* exitAct = tb->addAction("退出");
    connect(exitAct, &QAction::triggered, this, [this] {
      quitting_ = true;
      if (tray_) tray_->hide();
      QApplication::quit();
    });

    connect(input_, &QLineEdit::textChanged, this, &MainWindow::onQueryChanged);
    connect(table_, &QTableWidget::cellDoubleClicked, this, &MainWindow::onOpen);

    timer_ = new QTimer(this);
    timer_->setSingleShot(true);
    timer_->setInterval(150);
    connect(timer_, &QTimer::timeout, this, &MainWindow::runSearch);

    // 无边框窗口的鼠标跟踪：标题栏 + 各主要控件
    titleBar_->installEventFilter(this);
    input_->installEventFilter(this);
    table_->installEventFilter(this);
    tb->installEventFilter(this);
    central->installEventFilter(this);
  }

  void setupTray() {
    if (!QSystemTrayIcon::isSystemTrayAvailable()) {
      tray_ = nullptr;
      return;
    }
    tray_ = new QSystemTrayIcon(makeAppIcon(), this);
    tray_->setToolTip("Lsearch");
    auto* menu = new QMenu(this);
    menu->addAction("显示 / 隐藏", this, [this] { toggleWindow(); });
    menu->addSeparator();
    menu->addAction("重建索引", this, [this] { doRebuild(); });
    menu->addSeparator();
    menu->addAction("退出", this, [this] {
      quitting_ = true;
      tray_->hide();
      QApplication::quit();
    });
    connect(tray_, &QSystemTrayIcon::activated, this,
            [this](QSystemTrayIcon::ActivationReason r) {
              if (r == QSystemTrayIcon::Trigger || r == QSystemTrayIcon::DoubleClick) toggleWindow();
            });
    tray_->setContextMenu(menu);
    tray_->show();
  }

  void toggleWindow() {
    if (isVisible()) { hide(); } else { showNormal(); raise(); activateWindow(); }
  }

  void doRebuild() {
    statusBar()->showMessage("已触发索引重建…");
    std::thread([] {
      lsearch::Client c;
      std::string err;
      if (lsearch::Client::connectOrSpawn(lsearch::Config::load("").sock_path, true, c, err))
        c.command("rebuild", err);
    }).detach();
  }

  void onFilterChanged() {
    {
      std::lock_guard<std::mutex> lk(gmut_);
      dirsOnly_ = dirsAct_->isChecked();
      filesOnly_ = filesAct_->isChecked();
    }
    runSearch();
  }

  void showStats() {
    std::string err;
    lsearch::Client c;
    if (!lsearch::Client::connectOrSpawn(lsearch::Config::load("").sock_path, true, c, err)) {
      QMessageBox::warning(this, "索引统计", "无法连接 lsearchd: " + QString::fromStdString(err));
      return;
    }
    std::vector<std::pair<std::string, std::string>> kv;
    if (!c.stats(kv, err)) {
      QMessageBox::warning(this, "索引统计", "获取失败: " + QString::fromStdString(err));
      return;
    }
    QString text;
    for (auto& [k, v] : kv) {
      if (k == "size") text += "总大小: " + humanSize(atoll(v.c_str())) + "\n";
      else if (k == "uptime") text += QString("运行时长: %1 秒\n").arg(qlonglong(atoll(v.c_str())));
      else if (k == "roots") text += "索引根: " + QString::fromStdString(v) + "\n";
      else if (k == "rebuilding") text += QString("重建中: %1\n").arg(v == "1" ? "是" : "否");
      else text += QString::fromStdString(k) + ": " + QString::fromStdString(v) + "\n";
    }
    QMessageBox::information(this, "索引统计", text);
  }

  void searchLoop() {
    lsearch::Client c;
    std::string err;
    if (!lsearch::Client::connectOrSpawn(lsearch::Config::load("").sock_path, true, c, err)) {
      showStatus("无法连接 lsearchd: " + QString::fromStdString(err));
      return;
    }
    showStatus("lsearchd 已连接");

    while (running_) {
      std::string q;
      bool dirs, files;
      {
        std::unique_lock<std::mutex> lk(gmut_);
        cv_.wait(lk, [&] { return pending_ || !running_; });
        if (!running_) break;
        pending_ = false;
        q = query_;
        dirs = dirsOnly_;
        files = filesOnly_;
      }
      if (q.empty()) {
        fillTable({}, 0);
        continue;
      }
      std::vector<lsearch::SearchResult> res;
      size_t total = 0;
      if (c.search(q, lsearch::SortKey::Name, kMaxRows, dirs, files, res, &total, err)) {
        fillTable(res, total);
      } else {
        showStatus("搜索出错: " + QString::fromStdString(err));
      }
    }
  }

  void showStatus(const QString& msg) {
    QMetaObject::invokeMethod(this, [this, msg] { statusBar()->showMessage(msg); });
  }

  void fillTable(const std::vector<lsearch::SearchResult>& res, size_t total) {
    QMetaObject::invokeMethod(this, [this, res, total] {
      table_->setSortingEnabled(false);
      table_->setRowCount(static_cast<int>(res.size()));
      for (size_t i = 0; i < res.size(); ++i) {
        const auto& r = res[i].entry;
        int row = static_cast<int>(i);
        auto* path = new QTableWidgetItem(QString::fromStdString(r.path));
        table_->setItem(row, kColPath, path);
        auto* type = new QTableWidgetItem(r.is_dir ? "目录" : "文件");
        if (r.is_dir) type->setForeground(QColor(152, 195, 121));
        table_->setItem(row, kColType, type);
        auto* sz = new QTableWidgetItem(r.is_dir ? "-" : humanSize(r.size));
        sz->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
        table_->setItem(row, kColSize, sz);
        auto* mt = new QTableWidgetItem(isoTime(r.mtime));
        table_->setItem(row, kColMtime, mt);
      }
      table_->setSortingEnabled(true);
      statusBar()->showMessage(QString("命中 %1 条").arg(static_cast<qulonglong>(total)));
    });
  }

  QWidget* titleBar_ = nullptr;
  QToolButton* minBtn_ = nullptr;
  QToolButton* closeBtn_ = nullptr;
  QLineEdit* input_ = nullptr;
  QTableWidget* table_ = nullptr;
  QTimer* timer_ = nullptr;
  QSystemTrayIcon* tray_ = nullptr;
  QAction* dirsAct_ = nullptr;
  QAction* filesAct_ = nullptr;
  bool quitting_ = false;
  bool resizing_ = false;
  bool dragging_ = false;
  int edges_ = 0;
  QPoint pressGlobal_;
  QPoint dragOffset_;
  QRect startGeo_;
  bool dirsOnly_ = false;
  bool filesOnly_ = false;
  std::thread worker_;
  std::atomic<bool> running_{true};
  std::atomic<bool> pending_{false};
  std::mutex gmut_;
  std::condition_variable cv_;
  std::string query_;
};

int main(int argc, char** argv) {
  QApplication app(argc, argv);
  applyModernTheme(app);
  QApplication::setQuitOnLastWindowClosed(false);

  MainWindow w;
  w.show();
  return QApplication::exec();
}

#include "main.moc"