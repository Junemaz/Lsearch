// Lsearch GUI (Qt5)：Everything 风格的桌面搜索界面（V2 首版）
// 复用 ipc/client 访问 lsearchd（自动拉起守护进程），不直接触碰 core。
#include "core/config.h"
#include "core/entry.h"
#include "core/util.h"
#include "ipc/client.h"

#include <QApplication>
#include <QHeaderView>
#include <QLineEdit>
#include <QMainWindow>
#include <QProcess>
#include <QStatusBar>
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

}  // namespace

class MainWindow : public QMainWindow {
  Q_OBJECT

 public:
  MainWindow() {
    setWindowTitle("Lsearch");
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
    table_->setColumnWidth(1, 60);
    table_->setColumnWidth(2, 80);
    table_->setColumnWidth(3, 150);

    auto* central = new QWidget(this);
    auto* lay = new QVBoxLayout(central);
    lay->setContentsMargins(6, 6, 6, 6);
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
  }

  ~MainWindow() override {
    running_ = false;
    cv_.notify_all();
    if (worker_.joinable()) worker_.join();
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
        if (r.is_dir) type->setForeground(QBrush(QColor(0, 120, 0)));
        table_->setItem(row, kColType, type);
        auto* sz = new QTableWidgetItem(r.is_dir ? "-" : humanSize(r.size));
        sz->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
        sz->setData(Qt::UserRole + 1, static_cast<qulonglong>(r.size));
        table_->setItem(row, kColSize, sz);
        auto* mt = new QTableWidgetItem(isoTime(r.mtime));
        mt->setData(Qt::UserRole + 2, static_cast<qlonglong>(r.mtime));
        table_->setItem(row, kColMtime, mt);
      }
      table_->setSortingEnabled(true);
      statusBar()->showMessage(QString("命中 %1 条").arg(static_cast<qulonglong>(total)));
    });
  }

  QLineEdit* input_ = nullptr;
  QTableWidget* table_ = nullptr;
  QTimer* timer_ = nullptr;
  std::thread worker_;
  std::atomic<bool> running_{true};
  std::atomic<bool> pending_{false};
  std::mutex qmut_;
  std::condition_variable cv_;
  std::string query_;
};

int main(int argc, char** argv) {
  QApplication app(argc, argv);
  MainWindow w;
  w.show();
  return QApplication::exec();
}

#include "main.moc"