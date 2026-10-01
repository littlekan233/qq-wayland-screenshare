<div align="center">
  
# 🐧 qq-wayland-screenshare 🖥
在 Wayland 桌面环境下使用 QQ 屏幕共享。

</div>

> [!WARNING]
> niri 正式版目前因为 XShm 支持问题，暂不可用。
>
> 如需要在 niri 环境下使用，请考虑编译最新版本的 niri。Arch 用户可安装 [niri-git[AUR]](https://aur.archlinux.org/packages/niri-git)。
>
> **该项目目前额外依赖 libportal 库。**

## 📖 关于此项目
该项目受 [xuwd1/wemeet-wayland-screenshare](https://github.com/xuwd1/wemeet-wayland-screenshare) 启发而制作。

项目最初是基于原项目二改的，但是改着改着直接乱套了，索性推倒重来让 AI 再写一份得了（）

通过 `LD_PRELOAD` 选择要加载的库；两个版本启动 QQ 时都应显式设置
`XDG_SESSION_TYPE=x11`。无需额外的 `QWLSS_*` 功能开关。

## 🚀 编译
1. 安装依赖
   Ubuntu/Debian:
   ```sh
   sudo apt update
   sudo apt install \
     build-essential cmake ninja-build pkg-config \
     libglib2.0-dev \
     libxcb1-dev libxcb-randr0-dev libx11-dev libxext-dev \
     libportal-dev libpipewire-0.3-dev
   ```
   Fedora/RHEL:
   ```sh
   sudo dnf install \
     gcc cmake ninja-build pkgconf-pkg-config \
     glib2-devel \
     libxcb-devel libX11-devel libXext-devel \
     libportal-devel pipewire-devel
   ```
   Arch/Manjaro:
   ```sh
   sudo pacman -S --needed \
     gcc cmake ninja pkgconf \
     glib2 \
     libxcb libx11 libxext \
     libportal libpipewire
   ```
2. 克隆仓库
   ```sh
   git clone --depth=1 https://github.com/littlekan233/qq-wayland-screenshare.git
   ```
3. 编译
   ```sh
   cd qq-wayland-screenshare
   cmake -S . -B build -GNinja
   cmake --build build
   ```
   一次构建产出两个库：

   | 产物 | 用途 |
   | --- | --- |
   | `build/libqwlss.so` | 稳定版。X11 ozone 下的采集链路，行为与之前一致。 |
   | `build/libqwlss-exp-wlsanitizer.so` | 实验版。额外的 Wayland 支持，见下文。 |

   两个库都通过 `LD_PRELOAD` 加载，并需要 `XDG_SESSION_TYPE=x11` 的会话兼容设置。

## 📝 食用方法
在 Wayland 桌面启动 QQ 时，两个版本都应同时指定 `LD_PRELOAD` 和
`XDG_SESSION_TYPE=x11`。后者用于 QQ 的会话兼容判断，不会改变实际的 Ozone
渲染后端；渲染后端由 `--ozone-platform` 指定。

稳定版（X11 ozone）：

```sh
LD_PRELOAD=/path/to/libqwlss.so XDG_SESSION_TYPE=x11 /opt/QQ/qq --ozone-platform=x11
```

实验版（QQ 使用原生 Wayland ozone）：

```sh
LD_PRELOAD=/path/to/libqwlss-exp-wlsanitizer.so XDG_SESSION_TYPE=x11 /opt/QQ/qq --ozone-platform=wayland
```

实验版目前也会在主进程中设置 `XDG_SESSION_TYPE=x11`，作为兼容兜底；
使用方法仍统一显式设置它，不能把它写成仅稳定版需要的条件。实验版
另外尝试隐藏标题为 `屏幕共享` 的共享边框窗口。

当发起屏幕共享时，请在 QQ 的选择共享内容中选择 桌面1，然后在 XDG Portal 窗口中选择你要共享的内容。

### 实验版做了什么

Wayland ozone 下 QQ 的“屏幕共享”大边框是一个独立的窗口，会占满一层平铺布局。
实验版在共享期间把它 `.hide()`，共享本身不受影响；窗口对象保留，随时可以恢复。

判定条件（全部满足才处理）：

* 标题严格等于 `屏幕共享`；
* 窗口尺寸不小于 `600x400`：用于排除固定在 `87x40` 的工具栏和会缩到
  `202x148` 的预览窗口；
* 该尺寸持续至少 2 秒，且此时采集接收器已经建立。

接收器已建立只表示采集初始化成功，不保证首帧已经收到，也不证明用户已完成
选择共享源。该条件不参与捕获的启动判断；同标题预览窗口仍需实际验证是否会误匹配。

### 已知限制

* 只在 QQ 主进程、且 `--ozone-platform=wayland`（或 `WAYLAND_DISPLAY` 存在）时生效；
  X11 ozone 请使用稳定版。
* 入口改写依赖 QQ 的应用目录布局（`resources/app/package.json` 里的
  `application.asar/app_launcher/index.js`）。若 QQ 改版导致布局变化，
  实验版会打印一条说明并放弃处理边框，采集功能不受影响。
* 采集的启动/停止依靠 PPAPI 子进程的 `XShmGetImage` 活动信号；在 X11 ozone
  下由窗口监听线程负责，两条路径不会同时运行。
* 边框处理基于应用内部窗口 API，QQ 改版后可能需要重新适配。

## 🧪 测试

```sh
cmake -S . -B build -GNinja -DQWLSS_BUILD_TESTS=ON
cmake --build build
ctest --test-dir build --output-on-failure
```

覆盖内容：

* `shm_publish_read`：跨进程共享内存的发布与读取；
* `wlsanitizer`：实验版的主进程/Ozone 判定，以及“非主进程不得发布采集状态”；
* `hook_checker`：手动运行，检查 `XShmGetImage` hook 是否可用（需要真实 X11 环境）。

## 🤔 工作原理
```mermaid
flowchart TD
  A[启动 QQ，注入 Hook] --> B[Hook 在主进程启动窗口事件检测线程]
  A --> J[用户发起屏幕共享]
  B --> D[监听窗口事件]
  J --> E
  D --> E[有窗口触发 MapNotify 等事件]
  E --> F{类名是否为 qq\0QQ、窗口名是否为“屏幕共享”，以及窗口大小是否等于 X 服务器的显示大小？（是否为一个全屏的大蓝框窗口？）}
  F -->|是| G[执行 xcb_unmap_window_checked，不显示大蓝框窗口]
  F -->|否| H[忽略]
  H --> D
  G --> I[执行 watcher 的 do_screencast，准备传输画面]
  I --> B[启动 watchdog 线程，并等待共享正式开始]
  G --> K[拉起 XDG Portal 对话框，用户选择共享内容，启动 XDG Portal ScreenCast 会话]
  K --> L[选取后触发 on_portal_ready，拿到 PipeWire FD]
  L --> M[调用 pw_capture_start，接管 PipeWire FD，处理流数据中的图像并 hook_publish_bgrx]
  M --> M2[watchdog 开始 15s 倒计时来等待 XShmGetImage 被调用]
  B --> M2
  J --> N[QQ 启动 PPAPI 子进程并重复调用 XShmGetImage]
  N --> O[watchdog 检测到调用，开始监控请求]
  M2 -.-> O
  N --> P[Hook 从共享内存中获取 hook_publish_bgrx 获取到的帧，并转为 XImage 传给 QQ]
  J -->|共享一段时间后| J2[用户结束共享]
  J2 --> Q[PPAPI 进程被 QQ 关闭]
  N -.-> Q
  Q --> R[watchdog 发现 3 秒内没有任何 XShmGetImage 调用了，开始结束会话]
  O -.-> R
  R --> S[停止 watchdog、PipeWire FD、XDG Portal ScreenCast 会话]
  M -->|收到了 watchdog 的停止信号（pw_capture_stop）| S
  K -->|收到了 watchdog 的停止信号（portal_capture_stop）| S
  S --> T([清理残留帧，本轮会话结束])
```

Wayland ozone 下 QQ 不创建 X11 共享窗口，上面基于 X11 窗口的检测看不到它。
实验版因此在主进程里直接挂到 QQ 的窗口事件上，并按上文的判定条件隐藏边框；
共享开始/结束仍然由 PPAPI 的 `XShmGetImage` 活动信号驱动。

## 🙏 鸣谢
感谢 [xuwd1/wemeet-wayland-screenshare](https://github.com/xuwd1) 为本项目提供参考（旧版代码原本还是二改，~~被改成依托大分还不能用的旧版本~~在 legacy 分支上）

感谢 GPT 6 Astra 为该项目提供了大量代码 ~~（因为我几乎对这一块一窍不通.jpg 我边看边学的，流程图是真人推导的）~~
