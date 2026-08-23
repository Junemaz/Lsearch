// Lsearch GUI (Qt5)：Everything 风格的桌面搜索界面（V2）
// 复用 ipc/client 访问 lsearchd（自动拉起守护进程），不直接触碰 core。
// 特性：实时搜索 + 结果表格 + 双击打开 + 系统托盘常驻 + 深色现代主题。
#include "core/config.h"
#include "core/entry.h"
#include "core/util.h"
#include "ipc/client.h"

#include <QApplication>
#include <QBrush>
#include <QColor>
#include <QCloseEvent>
#include <QHeaderView>
#include <QIcon>
#include <QLineEdit>
#include <QMainWindow>
#include <QMenu>
#include <QPainter>
#include <QPalette>
#include <QPixmap>
#include <QProcess>
#include <QStatusBar>
#include <QSystemTrayIcon>
#include <QTableWidget>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>

#include <atomic>
#include <condition_variable>
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

QString humanSize(int64_t n) { return QString::fromStdString(lsearch::humanSize(n)); }
QString isoTime(int64_t t) { return QString::fromStdString(lsearch::isoTime(t)); }

void xdgOpen(const QString& path) { QProcess::startDetached("xdg-open", QStringList() << path); }

// 自绘图标：深色圆角底 + 放大镜（避免依赖资源文件）
QIcon makeAppIcon() {
  QPixmap pm(64, 64);
  pm.fill(Qt::transparent);
  QPainter p(&pm);
  p.setRenderHint(QPainter::Antialiasing);
  p.setBrush(QColor(30, 36, 51));
  p.setPen(Qt::NoPen);
  p.drawRoundedRect(0, 0, 64, 64, 14, 14);
  // 放大镜：镜圈
  p.setPen(QPen(QColor(97, 175, 239), 7, Qt::SolidLine, Qt::RoundCap));
  p.setBrush(QColor(15, 18, 25));
  p.drawEllipse(14, 14, 30, 30);
  // 镜柄
  p.setPen(QPen(QColor(97, 175, 239), 8, Qt::SolidLine, Qt::RoundCap));
  p.drawLine(QPointF(39, 39), QPointF(51, 51));
  p.end();
  return QIcon(pm);
}

// 深色现代主题（Fusion + 调色板 + QSS）
void applyModernTheme(QApplication& app) {
  app.setStyle("Fusion");

  QPalette pal;
  const QColor window(30, 36, 51), base(15, 18, 25), alt(27, 34, 48);
  const QColor text(232, 234, 240), faint(154, 167, 189), accent(97, 175, 239);
  pal.setColor(QPalette::Window, window);
  pal.setColor(QPalette::WindowText, text);
  pal.setColor(QPalette::Base, base);
  pal.setColor(QPalette::AlternateBase, alt);
  pal.setColor(QPalette::Text, text);
  pal.setColor(QPalette::Button, QColor(35, 43, 59));
  pal.setColor(QPalette::ButtonText, text);
  pal.setColor(QPalette::Highlight, accent);
  pal.setColor(QPalette::HighlightedText, QColor(255, 255, 255));
  pal.setColor(QPalette::ToolTipBase, QColor(35, 43, 59));
  pal.setColor(QPalette::ToolTipText, text);
#if QT_VERSION >= QT_VERSION_CHECK(5, 12, 0)
  pal.setColor(QPalette::PlaceholderText, faint);
#endif
  pal.setColor(QPalette::Disabled, QPalette::Text, faint);
  app.setPalette(pal);

  app.setStyleSheet(R"(
    QMainWindow { background: #1e2433; }
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
    QTableWidget {
      background: #171c28; alternate-background-color: #1b2230;
      color: #e8eaf0; border: none; gridline-color: transparent;
      selection-background-color: #2c456e; selection-color: #ffffff;
      outline: none;
    }
    QTableWidget::item { padding: 4px 6px; border: none; }
    QStatusBar { background: #232b3b; color: #9aa7bd; }
    QMenu {
      background: #232b3b; color: #e8eaf0;
      border: 1px solid #333c4e; border-radius: 6px; padding: 4px;
    }
    QMenu::item { padding: 6px 20px; border-radius: 4px; }
    QMenu::item:selected { background: #2c456e; color: #ffffff; }
    QMenu::separator { background: #333c4e; height: 1px; margin: 4px 8px; }
    QToolTip { background: #232b3b; color: #e8eaf0; border: 1px solid #333c4e; }
  )");
}

}  // namespace

class MainWindow : public QMainWindow {
  Q_OBJECT

 public:
  MainWindow() {
    setWindowTitle("Lsearch");
    setWindowIcon(makeAppIcon());
    resize(900, 620);

    input_ = new QLineEdit(this);
    input_->setPlaceholderText("输入关键词…（仅匹配文件名，大小写不敏感；* ? 为通配符）");

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
    table_->verticalHeader()->setVisible(false);
    table_->setColumnWidth(1, 60);
    table_->setColumnWidth(2, 80);
    table_->setColumnWidth(3, 150);

    auto* central = new QWidget(this);
    auto* lay = new QVBoxLayout(central);
    lay->setContentsMargins(8, 8, 8, 4);
    lay->setSpacing(6);
    lay->addWidget(input_);
    lay->addWidget(table_);
    setCentralWidget(central);

    statusBar()->showMessage("连接 lsearchd …");

    connect(input_, &QLineEdit::textChanged, this, &MainWindow::onQueryChanged);
    connect(table_, &QTableWidget::cellDoubleClicked, this, &MainWindow::onOpen);

    timer_ = new QTimer(this);
    timer_->setSingleShot(true);
    timer_->setInterval(150);
    connect(timer_, &QTimer::timeout, this, &MainWindow::runSearch);

    worker_ = std::thread([this] { searchLoop(); });

    setupTray();
  }

  ~MainWindow() override {
    running_ = false;
    cv_.notify_all();
    if (worker_.joinable()) worker_.join();
    if (tray_) tray_->hide();
  }

 protected:
  // 点关闭：有托盘环境则隐藏到托盘，否则照常退出
  void closeEvent(QCloseEvent* e) override {
    if (tray_ && !quitting_) {
      e->ignore();
      hide();
      tray_->showMessage("Lsearch", "已最小化到托盘，双击托盘图标可恢复。", QSystemTrayIcon::Information, 2000);
    } else {
      e->accept();
    }
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
      std::lock_guard<std::mutex> lk(qmut_);
      query_ = std::move(q);
      pending_ = true;
    }
    cv_.notify_one();
  }

 private:
  void setupTray() {
    if (!QSystemTrayIcon::isSystemTrayAvailable()) {
      // WSLg 等无托盘环境：关闭即退出，避免窗口"消失找不回"
      tray_ = nullptr;
      return;
    }
    tray_ = new QSystemTrayIcon(makeAppIcon(), this);
    tray_->setToolTip("Lsearch");

    auto* menu = new QMenu(this);
    QAction* toggleAct = menu->addAction("显示 / 隐藏");
    menu->addSeparator();
    QAction* rebuildAct = menu->addAction("重建索引");
    menu->addSeparator();
    QAction* quitAct = menu->addAction("退出");

    connect(toggleAct, &QAction::triggered, this, [this] {
      if (isVisible()) { hide(); } else { showNormal(); raise(); activateWindow(); }
    });
    connect(rebuildAct, &QAction::triggered, this, [this] { triggerRebuild(); });
    connect(quitAct, &QAction::triggered, this, [this] {
      quitting_ = true;
      tray_->hide();
      QApplication::quit();
    });
    connect(tray_, &QSystemTrayIcon::activated, this, [this](QSystemTrayIcon::ActivationReason r) {
      if (r == QSystemTrayIcon::Trigger || r == QSystemTrayIcon::DoubleClick) {
        if (isVisible()) { hide(); } else { showNormal(); raise(); activateWindow(); }
      }
    });

    tray_->setContextMenu(menu);
    tray_->show();
    statusBar()->showMessage("已驻留系统托盘：关闭窗口将最小化到托盘");
  }

  void triggerRebuild() {
    statusBar()->showMessage("已触发索引重建…");
    std::thread([] {
      lsearch::Client c;
      std::string err;
      if (lsearch::Client::connectOrSpawn(lsearch::Config::load("").sock_path, true, c, err))
        c.command("rebuild", err);
    }).detach();
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
      {
        std::unique_lock<std::mutex> lk(qmut_);
        cv_.wait(lk, [&] { return pending_ || !running_; });
        if (!running_) break;
        pending_ = false;
        q = query_;
      }
      if (q.empty()) {
        fillTable({}, 0);
        continue;
      }
      std::vector<lsearch::SearchResult> res;
      size_t total = 0;
      if (c.search(q, lsearch::SortKey::Name, kMaxRows, false, false, res, &total, err)) {
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

  QLineEdit* input_ = nullptr;
  QTableWidget* table_ = nullptr;
  QTimer* timer_ = nullptr;
  QSystemTrayIcon* tray_ = nullptr;
  bool quitting_ = false;
  std::thread worker_;
  std::atomic<bool> running_{true};
  std::atomic<bool> pending_{false};
  std::mutex qmut_;
  std::condition_variable cv_;
  std::string query_;
};

int main(int argc, char** argv) {
  QApplication app(argc, argv);
  applyModernTheme(app);
  // 关闭窗口可隐藏到托盘，故不因"最后一个窗口关闭"而退出
  QApplication::setQuitOnLastWindowClosed(false);

  MainWindow w;
  w.show();
  return QApplication::exec();
}

#include "main.moc"