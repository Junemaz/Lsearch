# 本地化补丁说明（Lsearch 对 Yet-Zio/FramelessWindow 的改动）
- Qt5 兼容：`event->position().toPoint()` → `event->pos()`、
  `event->globalPosition().toPoint()` → `event->globalPos()`（原库面向 Qt6）
- 增加 `#include <QScreen>` / `<QGuiApplication>`（Qt5 需显式包含）
- 浅色主题：`src/framelesswindow.css` 由默认暗色改为浅色（匹配 Lsearch 亮色主题，
  含 closeBtn hover 红、tooltip 等）
