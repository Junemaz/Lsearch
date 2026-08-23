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
#include <QCheckBox>
#include <QCloseEvent>
#include <QColor>
#include <QCursor>
#include <QDesktopServices>
#include <QDialog>
#include <QDir>
#include <QEvent>
#include <QFileDialog>
#include <QFrame>
#include <QGroupBox>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QHideEvent>
#include <QIcon>
#include <QKeyEvent>
#include <QKeySequence>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QLocale>
#include <QMainWindow>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPainter>
#include <QPalette>
#include <QPixmap>
#include <QProcess>
#include <QPushButton>
#include <QStatusBar>
#include <QStyle>
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

  // 关掉菜单退隐/动画：显隐即时完成，避免快速切换时新旧菜单短暂重叠
  app.setEffectEnabled(Qt::UI_FadeMenu, false);
  app.setEffectEnabled(Qt::UI_AnimateMenu, false);

  // ---- 浅色（亮色）主题 ----
  QPalette pal;
  const QColor window(244, 246, 248), base(255, 255, 255), alt(248, 250, 252);
  const QColor text(31, 36, 48), faint(107, 118, 134), accent(37, 99, 235);
  const QColor border(211, 217, 224), toolbg(255, 255, 255), sel(219, 234, 254);
  pal.setColor(QPalette::Window, window);
  pal.setColor(QPalette::WindowText, text);
  pal.setColor(QPalette::Base, base);
  pal.setColor(QPalette::AlternateBase, alt);
  pal.setColor(QPalette::Text, text);
  pal.setColor(QPalette::Button, toolbg);
  pal.setColor(QPalette::ButtonText, text);
  pal.setColor(QPalette::BrightText, Qt::white);
  pal.setColor(QPalette::Highlight, accent);
  pal.setColor(QPalette::HighlightedText, Qt::white);
  pal.setColor(QPalette::ToolTipBase, Qt::white);
  pal.setColor(QPalette::ToolTipText, text);
  pal.setColor(QPalette::Light, QColor(230, 234, 240));
  pal.setColor(QPalette::Midlight, QColor(224, 229, 236));
  pal.setColor(QPalette::Mid, QColor(214, 220, 228));
  pal.setColor(QPalette::Dark, QColor(196, 203, 212));
  pal.setColor(QPalette::Shadow, QColor(140, 149, 161));
#if QT_VERSION >= QT_VERSION_CHECK(5, 12, 0)
  pal.setColor(QPalette::PlaceholderText, faint);
#endif
  pal.setColor(QPalette::Link, accent);
  app.setPalette(pal);

  app.setStyleSheet(R"(
    QMainWindow, QDialog { background: #f4f6f8; }
    #titleBar { background: #ffffff; border-bottom: 1px solid #e3e6ec; }
    #titleText { font-weight: 600; font-size: 13px; color: #1f2430; }
    #btnMin, #btnClose {
      background: transparent; color: #6b7686;
      border: none; border-radius: 4px; font-size: 13px;
    }
    #btnMin:hover { background: #eef1f5; color: #1f2430; }
    #btnClose:hover { background: #e05561; color: #ffffff; }
    #menuBtn {
      background: transparent; color: #333c4e;
      border: none; border-radius: 4px; padding: 4px 10px;
    }
    #menuBtn:hover, #menuBtn[hover="true"] { background: #eef1f5; color: #1f2430; }
    #menuBtn:pressed { background: #e2e9f3; }
    #menuBtn[active="true"] { background: #dbeafe; color: #1f2430; }
    QLineEdit {
      background: #ffffff; color: #1f2430;
      border: 1px solid #d3d9e0; border-radius: 8px;
      padding: 8px 12px; font-size: 14px;
      selection-background-color: #2563eb; selection-color: #ffffff;
    }
    QLineEdit:focus { border: 1px solid #2563eb; }
    QHeaderView::section {
      background: #f1f3f6; color: #6b7686; border: none;
      border-bottom: 1px solid #e3e6ec;
      padding: 6px 8px; font-weight: 600;
    }
    QTableWidget, QAbstractScrollArea {
      background: #ffffff; alternate-background-color: #f8fafc;
      color: #1f2430; border: none; gridline-color: transparent;
      selection-background-color: #dbeafe; selection-color: #1f2430;
      outline: none;
    }
    QTableWidget::item { padding: 4px 6px; border: none; }
    QToolBar {
      background: #ffffff; border: none;
      border-top: 1px solid #e3e6ec; border-bottom: 1px solid #e3e6ec;
      spacing: 4px; padding: 4px;
    }
    QToolButton { background: transparent; color: #333c4e; border: none; border-radius: 5px; padding: 5px 12px; }
    QToolButton:hover { background: #eef1f5; color: #1f2430; }
    QToolButton:checked, QToolButton:pressed { background: #dbeafe; color: #1f2430; }
    QToolBar::separator { background: #e3e6ec; width: 1px; margin: 4px 6px; }
    QStatusBar { background: #f1f3f6; color: #6b7686; }
    QMenu { background: #ffffff; color: #1f2430;
      border: 1px solid #d3d9e0; border-radius: 6px; padding: 4px; }
    QMenu::item { padding: 6px 20px; border-radius: 4px; }
    QMenu::item:selected { background: #dbeafe; color: #1f2430; }
    QMenu::item:disabled { color: #a8b0bc; }
    QMenu::separator { background: #e3e6ec; height: 1px; margin: 4px 8px; }
    #dropdown { background: #ffffff; border: 1px solid #d3d9e0; border-radius: 6px; }
    #menuItem {
      background: transparent; color: #1f2430;
      border: none; border-radius: 4px; text-align: left;
      padding: 6px 18px;
    }
    #menuItem:hover { background: #dbeafe; color: #1f2430; }
    #menuItem:disabled { color: #a8b0bc; }
    #menuSep { background: #e3e6ec; height: 1px; border: none; margin: 3px 8px; }
    QToolTip { background: #ffffff; color: #1f2430; border: 1px solid #d3d9e0; }
    QLabel { color: #1f2430; }
    QGroupBox {
      border: 1px solid #e3e6ec; border-radius: 8px; margin-top: 10px;
      padding-top: 6px; color: #333c4e;
    }
    QGroupBox::title { subcontrol-origin: margin; left: 10px; padding: 0 4px; }
    QPushButton {
      background: #ffffff; color: #1f2430;
      border: 1px solid #d3d9e0; border-radius: 6px; padding: 6px 14px;
    }
    QPushButton:hover { background: #eef1f5; }
    QPushButton:pressed { background: #e2e9f3; }
    QCheckBox { color: #1f2430; spacing: 6px; }
    QListWidget {
      background: #ffffff; color: #1f2430; border: 1px solid #d3d9e0;
      border-radius: 6px; outline: none;
    }
    QListWidget::item { padding: 4px 6px; border-radius: 4px; }
    QListWidget::item:selected { background: #dbeafe; color: #1f2430; }
  )");
}

}  // namespace

// 索引管理对话框：增删根路径 / 排除前缀 / 选项，直接走守护进程 IPC 生效并重建
class IndexManageDialog : public QDialog {
  Q_OBJECT

 public:
  explicit IndexManageDialog(QWidget* parent = nullptr) : QDialog(parent) {
    setWindowTitle("索引管理");
    setWindowFlags(Qt::Dialog | Qt::FramelessWindowHint);
    setFixedSize(580, 480);
    setWindowIcon(makeAppIcon());

    // 自绘小标题栏（可拖动）
    header_ = new QWidget(this);
    header_->setObjectName("titleBar");
    header_->setFixedHeight(36);
    auto* hl = new QHBoxLayout(header_);
    hl->setContentsMargins(12, 0, 6, 0);
    auto* ht = new QLabel("索引管理", header_);
    ht->setObjectName("titleText");
    auto* hclose = new QToolButton(header_);
    hclose->setObjectName("btnClose");
    hclose->setText("✕");
    hclose->setFixedSize(30, 22);
    connect(hclose, &QToolButton::clicked, this, &QDialog::reject);
    hl->addWidget(ht);
    hl->addStretch(1);
    hl->addWidget(hclose);
    header_->installEventFilter(this);

    // 根路径
    auto* pathGroup = new QGroupBox("索引根路径", this);
    pathsList_ = new QListWidget(pathGroup);
    auto* pathBtns = new QVBoxLayout;
    addPathBtn_ = new QPushButton("添加目录…", pathGroup);
    delPathBtn_ = new QPushButton("删除", pathGroup);
    pathBtns->addWidget(addPathBtn_);
    pathBtns->addWidget(delPathBtn_);
    pathBtns->addStretch(1);
    auto* pathRow = new QHBoxLayout(pathGroup);
    pathRow->addWidget(pathsList_);
    pathRow->addLayout(pathBtns);

    // 排除前缀
    auto* exclGroup = new QGroupBox("排除路径（前缀匹配，含其下所有内容）", this);
    exclList_ = new QListWidget(exclGroup);
    exclEdit_ = new QLineEdit(exclGroup);
    exclEdit_->setPlaceholderText("输入要排除的路径前缀，回车添加");
    auto* exclBtns = new QVBoxLayout;
    addExclBtn_ = new QPushButton("添加", exclGroup);
    delExclBtn_ = new QPushButton("删除", exclGroup);
    exclBtns->addWidget(addExclBtn_);
    exclBtns->addWidget(delExclBtn_);
    exclBtns->addStretch(1);
    auto* exclRow = new QHBoxLayout(exclGroup);
    auto* exclCol = new QVBoxLayout;
    exclCol->addWidget(exclEdit_);
    exclCol->addWidget(exclList_);
    exclRow->addLayout(exclCol);
    exclRow->addLayout(exclBtns);

    // 选项
    auto* optGroup = new QGroupBox("选项", this);
    hiddenChk_ = new QCheckBox("索引隐藏文件/目录（以 . 开头）", optGroup);
    followChk_ = new QCheckBox("跟随符号链接", optGroup);
    auto* ol = new QVBoxLayout(optGroup);
    ol->addWidget(hiddenChk_);
    ol->addWidget(followChk_);
    ol->addWidget(new QLabel("修改后点「应用并重建」生效，会全量重扫。", optGroup));

    // 底部按钮
    applyBtn_ = new QPushButton("应用并重建", this);
    rebuildBtn_ = new QPushButton("立即重建", this);
    closeBtn_ = new QPushButton("关闭", this);
    auto* foot = new QHBoxLayout;
    foot->addStretch(1);
    foot->addWidget(applyBtn_);
    foot->addWidget(rebuildBtn_);
    foot->addWidget(closeBtn_);

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);
    auto* body = new QVBoxLayout;
    body->setContentsMargins(12, 8, 12, 8);
    body->setSpacing(8);
    body->addWidget(pathGroup);
    body->addWidget(exclGroup);
    body->addWidget(optGroup);
    body->addLayout(foot);
    root->addWidget(header_);
    root->addLayout(body);

    connect(addPathBtn_, &QPushButton::clicked, this, [this] {
      QString dir = QFileDialog::getExistingDirectory(this, "选择要索引的目录");
      if (!dir.isEmpty() && pathsList_->findItems(dir, Qt::MatchExactly).isEmpty())
        pathsList_->addItem(dir);
    });
    connect(delPathBtn_, &QPushButton::clicked, this, [this] { deleteSelected(pathsList_); });
    connect(addExclBtn_, &QPushButton::clicked, this, [this] { addExclFromEdit(); });
    connect(exclEdit_, &QLineEdit::returnPressed, this, [this] { addExclFromEdit(); });
    connect(delExclBtn_, &QPushButton::clicked, this, [this] { deleteSelected(exclList_); });
    connect(applyBtn_, &QPushButton::clicked, this, [this] { apply(); });
    connect(rebuildBtn_, &QPushButton::clicked, this, [this] { triggerRebuild(); });
    connect(closeBtn_, &QPushButton::clicked, this, &QDialog::reject);

    loadConfig();
  }

  bool eventFilter(QObject* o, QEvent* e) override {
    if (o == header_ && e->type() == QEvent::MouseButtonPress) {
      auto* m = static_cast<QMouseEvent*>(e);
      if (m->button() == Qt::LeftButton) {
        dragging_ = true;
        dragOffset_ = m->globalPos() - frameGeometry().topLeft();
        return true;
      }
    } else if (o == header_ && e->type() == QEvent::MouseMove) {
      auto* m = static_cast<QMouseEvent*>(e);
      if (dragging_ && (m->buttons() & Qt::LeftButton)) {
        move(m->globalPos() - dragOffset_);
        return true;
      }
    } else if (o == header_ && e->type() == QEvent::MouseButtonRelease) {
      if (dragging_) { dragging_ = false; return true; }
    }
    return QDialog::eventFilter(o, e);
  }

 private:
  void loadConfig() {
    lsearch::Client c;
    std::string err;
    if (!lsearch::Client::connectOrSpawn(lsearch::Config::load("").sock_path, true, c, err)) {
      QMessageBox::warning(this, "索引管理", "无法连接 lsearchd: " + QString::fromStdString(err));
      return;
    }
    std::vector<std::pair<std::string, std::string>> kv;
    if (!c.getConfig(kv, err)) {
      if (err.find("unknown command") != std::string::npos)
        QMessageBox::warning(this, "索引管理",
                             "守护进程版本过旧，还不支持「索引管理」。\n"
                             "请关闭本窗口重新打开（会自动用新版本重启守护进程），再试一次。");
      else
        QMessageBox::warning(this, "索引管理", "读取配置失败: " + QString::fromStdString(err));
      return;
    }
    QString paths, excludes, hidden, follow;
    for (auto& [k, v] : kv) {
      if (k == "paths") paths = QString::fromStdString(v);
      else if (k == "excludes") excludes = QString::fromStdString(v);
      else if (k == "hidden") hidden = QString::fromStdString(v);
      else if (k == "follow") follow = QString::fromStdString(v);
    }
    for (const auto& p : paths.split(',', Qt::SkipEmptyParts)) pathsList_->addItem(p.trimmed());
    for (const auto& p : excludes.split(',', Qt::SkipEmptyParts)) exclList_->addItem(p.trimmed());
    hiddenChk_->setChecked(hidden == "1");
    followChk_->setChecked(follow == "1");
  }

  QStringList collect(QListWidget* w) const {
    QStringList out;
    for (int i = 0; i < w->count(); ++i) out << w->item(i)->text().trimmed();
    return out;
  }

  void addExclFromEdit() {
    QString t = exclEdit_->text().trimmed();
    if (!t.isEmpty() && exclList_->findItems(t, Qt::MatchExactly).isEmpty()) exclList_->addItem(t);
    exclEdit_->clear();
  }

  void deleteSelected(QListWidget* w) {
    for (auto* it : w->selectedItems()) delete it;
  }

  void apply() {
    QString pathsCsv = collect(pathsList_).join(',');
    QString exclCsv = collect(exclList_).join(',');
    if (exclCsv.isEmpty()) exclCsv = "_";
    lsearch::Client c;
    std::string err;
    if (!lsearch::Client::connectOrSpawn(lsearch::Config::load("").sock_path, true, c, err)) {
      QMessageBox::warning(this, "索引管理", "无法连接 lsearchd: " + QString::fromStdString(err));
      return;
    }
    if (!c.setPaths(pathsCsv.toStdString(), err) || !c.setExcludes(exclCsv.toStdString(), err) ||
        !c.setOpts(hiddenChk_->isChecked() ? "1" : "0", followChk_->isChecked() ? "1" : "0", err)) {
      if (err.find("unknown command") != std::string::npos)
        QMessageBox::warning(this, "索引管理",
                             "守护进程版本过旧，还不支持「索引管理」。\n"
                             "请关闭本窗口重新打开（会自动用新版本重启守护进程），再试一次。");
      else
        QMessageBox::warning(this, "索引管理", "保存失败: " + QString::fromStdString(err));
      return;
    }
    QMessageBox::information(this, "索引管理", "配置已保存，正在后台重建索引…");
  }

  void triggerRebuild() {
    std::thread([] {
      lsearch::Client c;
      std::string err;
      if (lsearch::Client::connectOrSpawn(lsearch::Config::load("").sock_path, true, c, err))
        c.command("rebuild", err);
    }).detach();
    QMessageBox::information(this, "索引管理", "已触发重建，正在后台执行…");
  }

  QWidget* header_ = nullptr;
  QListWidget* pathsList_ = nullptr;
  QListWidget* exclList_ = nullptr;
  QLineEdit* exclEdit_ = nullptr;
  QPushButton* addPathBtn_ = nullptr;
  QPushButton* delPathBtn_ = nullptr;
  QPushButton* addExclBtn_ = nullptr;
  QPushButton* delExclBtn_ = nullptr;
  QPushButton* applyBtn_ = nullptr;
  QPushButton* rebuildBtn_ = nullptr;
  QPushButton* closeBtn_ = nullptr;
  QCheckBox* hiddenChk_ = nullptr;
  QCheckBox* followChk_ = nullptr;
  bool dragging_ = false;
  QPoint dragOffset_;
};

// 自定义下拉面板：完全不用 QMenu 弹窗机制（那套在部分合成器上会残留双菜单）。
// 一个普通 QWidget(Qt::Popup)，自己管理显隐；切换 = 同一窗口换内容+挪位置，
// 物理上不存在“第二个弹窗”，也就谈不上叠加/残影。
class Dropdown : public QWidget {
  Q_OBJECT
 public:
  explicit Dropdown(QWidget* parent = nullptr) : QWidget(parent, Qt::Popup) {
    setObjectName("dropdown");
    setAttribute(Qt::WA_StyledBackground, true);
    lay_ = new QVBoxLayout(this);
    lay_->setContentsMargins(4, 4, 4, 4);
    lay_->setSpacing(1);
  }

  // 用一个锚点按钮 + 模板菜单(含分隔符/禁用项)填充并显示；若已显示则原地换内容
  void showFor(QToolButton* anchor, QMenu* model) {
    // 抑制中间帧：重建+挪位期间不重绘，避免残影
    setUpdatesEnabled(false);
    qDeleteAll(rows_);
    rows_.clear();
    for (QAction* a : model->actions()) {
      if (a->isSeparator()) {
        auto* sep = new QFrame(this);
        sep->setObjectName("menuSep");
        sep->setFrameShape(QFrame::HLine);
        lay_->addWidget(sep);
        rows_.append(sep);
      } else {
        auto* b = new QToolButton(this);
        b->setObjectName("menuItem");
        b->setText(a->text());
        b->setEnabled(a->isEnabled());
        b->setToolTip(a->toolTip());
        b->setCursor(Qt::PointingHandCursor);
        connect(b, &QToolButton::clicked, this, [this, a] {
          hide();  // 先收面板，再触发动作（动作可能弹模态框）
          a->trigger();
        });
        lay_->addWidget(b);
        rows_.append(b);
      }
    }
    adjustSize();
    move(anchor->mapToGlobal(QPoint(0, anchor->height() + 2)));
    if (!isVisible()) show();  // 已显示则仅挪位置，不重新映射窗口
    setUpdatesEnabled(true);
    raise();
    activateWindow();
    update();
  }

 protected:
  // 面板外点击 -> 收起（配合 MainWindow 的状态复位）
  void mousePressEvent(QMouseEvent* e) override {
    if (!rect().contains(e->pos())) hide();
    QWidget::mousePressEvent(e);
  }
  void keyPressEvent(QKeyEvent* e) override {
    if (e->key() == Qt::Key_Escape) hide();
    QWidget::keyPressEvent(e);
  }
  void hideEvent(QHideEvent*) override { emit hidden(); }

 signals:
  void hidden();

 private:
  QVBoxLayout* lay_;
  QList<QWidget*> rows_;
};

class MainWindow : public QMainWindow {
  Q_OBJECT

 public:
  MainWindow() {
    setWindowTitle("Lsearch [gui-panel]");  // ASCII 构建标记：便于确认当前运行版本
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
  // 点 ✕ = 真正退出进程（不再隐藏到托盘，避免“窗口没了进程还在”）
  void closeEvent(QCloseEvent* e) override {
    quitApp();
    e->accept();
  }

  // 无边框窗口：拖拽/缩放统一入口（过滤标题栏与主要子控件）
  bool eventFilter(QObject* obj, QEvent* ev) override {
    // 菜单按钮：**点击才打开**（不悬停误弹）；菜单打开后的“飘过去切换/高亮
    // 跟随”由 watchMenus 全局光标轮询完成（弹窗抓取会吞掉按钮悬停事件，
    // 所以高亮不依赖 Qt 的 hover，全由我们手动驱动）
    if (isMenuButton(obj)) {
      if (ev->type() == QEvent::MouseButtonPress) {
        openMenuAt(menuButtonIndex(obj));
        refreshButtonStates();
        return true;
      }
      if (ev->type() == QEvent::Enter || ev->type() == QEvent::Leave) {
        refreshButtonStates();  // 无菜单打开时的悬停高亮/离开熄灭
        return false;
      }
    }
    // 点击其它地方（搜索框/表格/空白）收摊并复位高亮
    if (ev->type() == QEvent::MouseButtonPress && activeMenu_ >= 0 &&
        !isMenuButton(obj)) {
      closeAllMenus();
      refreshButtonStates();
    }
    if (ev->type() == QEvent::MouseButtonPress || ev->type() == QEvent::MouseMove ||
        ev->type() == QEvent::MouseButtonRelease || ev->type() == QEvent::MouseButtonDblClick) {
      auto* m = static_cast<QMouseEvent*>(ev);
      QPoint winPos = (obj == this) ? m->pos() : static_cast<QWidget*>(obj)->mapTo(this, m->pos());
      if (handleTopLevelMouse(static_cast<QWidget*>(obj), m, winPos)) return true;
    }
    return QMainWindow::eventFilter(obj, ev);
  }

  // 菜单打开期间：全局光标轮询（60ms）——光标飘到别的标题上立即切换，
  // 并在每个 tick 刷新按钮高亮（弹窗抓取吞了鼠标事件，高亮必须我们自己刷）
  void watchMenus() {
    if (activeMenu_ < 0) {
      refreshButtonStates();
      return;
    }
    const QPoint g = QCursor::pos();
    for (size_t i = 0; i < menuBtns_.size(); ++i) {
      QRect r(menuBtns_[i]->mapToGlobal(QPoint(0, 0)), menuBtns_[i]->size());
      if (r.contains(g)) {
        if (activeMenu_ != static_cast<int>(i)) openMenuAt(static_cast<int>(i));
        refreshButtonStates();
        return;
      }
    }
    refreshButtonStates();
  }

  // 手动驱动菜单按钮高亮：
  //   active = 当前打开的菜单；hover = 鼠标正悬停的按钮（即使菜单弹窗抓取了鼠标）
  // 关闭 hover 事件依赖，保证高亮始终跟随光标、旧高亮必被清掉
  void refreshButtonStates() {
    QPoint g = QCursor::pos();
    int over = -1;
    for (size_t i = 0; i < menuBtns_.size(); ++i) {
      QRect r(menuBtns_[i]->mapToGlobal(QPoint(0, 0)), menuBtns_[i]->size());
      if (r.contains(g)) { over = static_cast<int>(i); break; }
    }
    for (size_t i = 0; i < menuBtns_.size(); ++i) {
      QToolButton* b = menuBtns_[i];
      const bool act = (activeMenu_ == static_cast<int>(i));
      const bool hov = (over == static_cast<int>(i));
      if (b->property("active").toBool() != act || b->property("hover").toBool() != hov) {
        b->setProperty("active", act);
        b->setProperty("hover", hov);
        b->style()->unpolish(b);
        b->style()->polish(b);
      }
    }
  }

  bool isMenuButton(QObject* o) const { return menuButtonIndex(o) >= 0; }
  int menuButtonIndex(QObject* o) const {
    for (size_t i = 0; i < menuBtns_.size(); ++i)
      if (o == menuBtns_[i]) return static_cast<int>(i);
    return -1;
  }

  // 打开第 i 个菜单：单例下拉面板复用——同一窗口换内容/位置，物理上无第二个弹窗
  void openMenuAt(int i) {
    if (i < 0 || i >= static_cast<int>(menuBtns_.size())) return;
    if (activeMenu_ == i && dropdown_->isVisible()) return;
    dropdown_->showFor(menuBtns_[i], menuModels_[i]);
    activeMenu_ = i;
  }

  void closeAllMenus() {
    dropdown_->hide();
    activeMenu_ = -1;
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
    connect(minBtn_, &QToolButton::clicked, this, [this] {
      // — = 最小化（托盘可用时最小化到托盘，否则普通最小化）
      if (tray_) {
        hide();
        tray_->showMessage("Lsearch", "已最小化到托盘，点击托盘图标可恢复。",
                           QSystemTrayIcon::Information, 2000);
      } else {
        showMinimized();
      }
    });
    connect(closeBtn_, &QToolButton::clicked, this, [this] { quitApp(); });  // ✕ = 退出进程
    tl->addWidget(icon);
    tl->addWidget(title);
    tl->addStretch(1);
    tl->addWidget(minBtn_);
    tl->addWidget(closeBtn_);

    // ---- 自绘菜单行 ----
    // 单例下拉面板 dropdown_：全程序只有这一扇“菜单窗口”，三个标题的条目
    // 存在三个模板 QMenu（从不显示），打开时把对应模板填进面板并挪到按钮下。
    // 结构上不可能出现两个菜单。
    dropdown_ = new Dropdown(this);
    connect(dropdown_, &Dropdown::hidden, this, [this] {
      activeMenu_ = -1;
      refreshButtonStates();
    });

    menuRow_ = new QWidget(this);
    auto* mr = new QHBoxLayout(menuRow_);
    mr->setContentsMargins(8, 2, 8, 2);
    mr->setSpacing(2);
    auto addMenuBtn = [&](const QString& text) {
      auto* b = new QToolButton(menuRow_);
      b->setObjectName("menuBtn");
      b->setText(text);
      b->setCursor(Qt::PointingHandCursor);
      b->setFocusPolicy(Qt::NoFocus);
      b->setMouseTracking(true);
      mr->addWidget(b);
      menuBtns_.push_back(b);
      return b;
    };

    auto* fileMenu = new QMenu(this);
    QAction* hideAct = fileMenu->addAction("隐藏到托盘");
    hideAct->setShortcut(QKeySequence("Ctrl+H"));
    hideAct->setEnabled(QSystemTrayIcon::isSystemTrayAvailable());
    connect(hideAct, &QAction::triggered, this, [this] {
      if (tray_) {
        hide();
        tray_->showMessage("Lsearch", "已最小化到托盘，点击托盘图标可恢复。",
                           QSystemTrayIcon::Information, 2000);
      }
    });
    fileMenu->addSeparator();
    QAction* quitAct = fileMenu->addAction("退出");
    quitAct->setShortcut(QKeySequence("Ctrl+Q"));
    connect(quitAct, &QAction::triggered, this, [this] { quitApp(); });
    menuModels_.push_back(fileMenu);
    addMenuBtn("文件");

    auto* toolsMenu = new QMenu(this);
    QAction* mRebuild = toolsMenu->addAction("重建索引");
    mRebuild->setShortcut(QKeySequence("Ctrl+R"));
    connect(mRebuild, &QAction::triggered, this, [this] { doRebuild(); });
    QAction* mManage = toolsMenu->addAction("索引管理");
    mManage->setShortcut(QKeySequence("Ctrl+M"));
    connect(mManage, &QAction::triggered, this, [this] { openManage(); });
    QAction* mStats = toolsMenu->addAction("索引统计");
    mStats->setShortcut(QKeySequence("Ctrl+I"));
    connect(mStats, &QAction::triggered, this, [this] { showStats(); });
    toolsMenu->addSeparator();
    QAction* mConfig = toolsMenu->addAction("打开配置文件目录");
    connect(mConfig, &QAction::triggered, this, [this] { openConfigDir(); });
    menuModels_.push_back(toolsMenu);
    addMenuBtn("工具");

    auto* helpMenu = new QMenu(this);
    connect(helpMenu->addAction("关于 Lsearch"), &QAction::triggered, this, [this] {
      QMessageBox::about(this, "关于 Lsearch",
                         "Lsearch 0.1.0\n\nEverything 风格的文件名搜索（麒麟/信创桌面）。\n"
                         "守护进程 lsearchd + CLI/TUI/GUI 多前端。");
    });
    menuModels_.push_back(helpMenu);
    addMenuBtn("帮助");
    mr->addStretch(1);
    menuRow_->setMouseTracking(true);

    // 菜单打开期间用全局光标轮询实现“飘过去切换”（绕过弹窗抓取吞悬停事件）
    menuWatchTimer_ = new QTimer(this);
    menuWatchTimer_->setInterval(60);
    connect(menuWatchTimer_, &QTimer::timeout, this, [this] { watchMenus(); });
    menuWatchTimer_->start();

    menuRow_->installEventFilter(this);
    for (QToolButton* b : menuBtns_) b->installEventFilter(this);

    // 注册菜单动作的快捷键到主窗口（否则藏在模板菜单里的快捷键不生效）
    for (QMenu* m : menuModels_)
      for (QAction* a : m->actions()) addAction(a);

    headerCont_ = new QWidget(this);
    headerCont_->setObjectName("titleBar");
    auto* hc = new QVBoxLayout(headerCont_);
    hc->setContentsMargins(0, 0, 0, 0);
    hc->setSpacing(0);
    hc->addWidget(titleBar_);
    hc->addWidget(menuRow_);
    setMenuWidget(headerCont_);  // 标题栏 + 菜单行整体位于中央区之上

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

    // ---- 工具栏（仅保留高频操作；其余进菜单）----
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

    connect(input_, &QLineEdit::textChanged, this, &MainWindow::onQueryChanged);
    connect(table_, &QTableWidget::cellDoubleClicked, this, &MainWindow::onOpen);

    timer_ = new QTimer(this);
    timer_->setSingleShot(true);
    timer_->setInterval(150);
    connect(timer_, &QTimer::timeout, this, &MainWindow::runSearch);

    // ---- 重建进度：状态栏右侧常驻标签 + 每秒轮询 ----
    rebuildLabel_ = new QLabel("", this);
    rebuildLabel_->setStyleSheet("color:#6b7686;padding:0 8px;");
    statusBar()->addPermanentWidget(rebuildLabel_);
    pollTimer_ = new QTimer(this);
    pollTimer_->setInterval(1000);
    connect(pollTimer_, &QTimer::timeout, this, &MainWindow::pollRebuild);
    pollTimer_->start();

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

  void quitApp() {
    quitting_ = true;
    if (tray_) tray_->hide();
    QApplication::quit();
  }

  void openManage() {
    IndexManageDialog dlg(this);
    dlg.exec();
    runSearch();  // 应用后刷新当前结果
  }

  void openConfigDir() {
    std::string f = lsearch::Config::load("").config_file;
    QDir dir(QString::fromStdString(lsearch::dirName(f)));
    QDesktopServices::openUrl(QUrl::fromLocalFile(dir.path()));
  }

  // 每 1 秒轮询 stats：重建时显示进度，完成时显示结果片刻后清除
  void pollRebuild() {
    std::string err;
    if (!pollClient_.connected() && !pollClient_.connect(lsearch::Config::load("").sock_path, err)) {
      QMetaObject::invokeMethod(this, [this] { rebuildLabel_->clear(); });
      return;
    }
    std::vector<std::pair<std::string, std::string>> kv;
    if (!pollClient_.stats(kv, err)) {
      pollClient_.close();
      QMetaObject::invokeMethod(this, [this] { rebuildLabel_->clear(); });
      return;
    }
    bool rebuilding = false;
    uint64_t sf = 0, files = 0;
    for (auto& [k, v] : kv) {
      if (k == "rebuilding") rebuilding = (v == "1");
      else if (k == "scan_files") sf = static_cast<uint64_t>(atoll(v.c_str()));
      else if (k == "files") files = static_cast<uint64_t>(atoll(v.c_str()));
    }
    QMetaObject::invokeMethod(this, [this, rebuilding, sf, files] {
      if (rebuilding) {
        wasRebuilding_ = true;
        rebuildLabel_->setText(QString("⏳ 正在重建索引… 已扫 %1 项")
                                   .arg(QLocale().toString(static_cast<qlonglong>(sf))));
      } else if (wasRebuilding_) {
        wasRebuilding_ = false;
        rebuildLabel_->setText(QString("✓ 重建完成，共 %1 条")
                                   .arg(QLocale().toString(static_cast<qlonglong>(files))));
        QTimer::singleShot(5000, this, [this] { rebuildLabel_->clear(); });
      } else {
        rebuildLabel_->clear();
      }
    });
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
        // 断线/旧守护进程：尝试重连（自动拉起新守护进程），下个查询重试
        c.close();
        std::string e2;
        if (lsearch::Client::connectOrSpawn(lsearch::Config::load("").sock_path, true, c, e2)) {
          showStatus("守护进程已重连");
        } else {
          showStatus("搜索出错: " + QString::fromStdString(err) + "（已断线：" + QString::fromStdString(e2) + "）");
        }
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
        if (r.is_dir) type->setForeground(QColor(21, 128, 61));
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
  QWidget* headerCont_ = nullptr;
  QWidget* menuRow_ = nullptr;
  std::vector<QToolButton*> menuBtns_;
  int activeMenu_ = -1;
  QTimer* menuWatchTimer_ = nullptr;
  // 单例下拉面板 + 三个“模板”菜单（只存菜单项，从不弹出/显示）
  Dropdown* dropdown_ = nullptr;
  std::vector<QMenu*> menuModels_;
  QToolButton* minBtn_ = nullptr;
  QToolButton* closeBtn_ = nullptr;
  QLineEdit* input_ = nullptr;
  QTableWidget* table_ = nullptr;
  QTimer* timer_ = nullptr;
  QTimer* pollTimer_ = nullptr;
  QLabel* rebuildLabel_ = nullptr;
  bool wasRebuilding_ = false;
  lsearch::Client pollClient_;
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