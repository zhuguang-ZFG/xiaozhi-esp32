---
name: "Grobot 创作工作台"
description: "A warm, precise creative workbench for safe paper drawing."
colors:
  ink-navy: "#10161C"
  ink-navy-chat: "#141C22"
  paper-surface: "#1A242B"
  paper-light: "#F4F1EA"
  paper-card: "#FFFDF8"
  cyan-brand: "#32D6CB"
  cyan-deep: "#1B7773"
  text-light: "#F2F0E8"
  text-muted: "#A7B1B8"
  border-muted: "#34434A"
  success: "#5ECB9A"
  warning: "#E4B15D"
  danger: "#E06A70"
typography:
  display:
    fontFamily: "Noto Sans, system sans-serif"
    fontSize: "30px"
    fontWeight: 500
    lineHeight: 1.2
  headline:
    fontFamily: "Noto Sans, system sans-serif"
    fontSize: "20px"
    fontWeight: 500
    lineHeight: 1.3
  body:
    fontFamily: "Noto Sans, system sans-serif"
    fontSize: "20px"
    fontWeight: 400
    lineHeight: 1.4
  label:
    fontFamily: "Noto Sans, system sans-serif"
    fontSize: "20px"
    fontWeight: 500
    lineHeight: 1.2
rounded:
  pill: "18px"
  card: "24px"
  stage: "32px"
spacing:
  unit: "2px"
  control: "56px"
  card: "8px"
components:
  button-primary:
    backgroundColor: "{colors.cyan-brand}"
    textColor: "#071316"
    rounded: "{rounded.pill}"
    height: "56px"
  button-secondary:
    backgroundColor: "{colors.paper-surface}"
    textColor: "{colors.text-light}"
    rounded: "{rounded.pill}"
    height: "56px"
  status-pill:
    backgroundColor: "{colors.paper-surface}"
    textColor: "{colors.text-light}"
    rounded: "{rounded.pill}"
    height: "36px"
  preview-card:
    backgroundColor: "{colors.paper-surface}"
    textColor: "{colors.text-light}"
    rounded: "{rounded.card}"
    padding: "10px"
---

# Design System: Grobot 创作工作台

## Overview

**Creative North Star: "The Quiet Creative Desk"**

Grobot 创作工作台把设备变成一张安静的创作桌：深色、低饱和的工作底承托 Grobot 的表情，青蓝只在安全且需要注意的动作上发光；纸感卡片把预览、状态和控制从背景中托起。它既要让孩子一眼读懂下一步，也要让调试操作者在需要时找到完整工具，而不是把机器控制铺成工程控制台。

视觉系统偏温暖、精确、可触摸。圆角、边框和柔和阴影表达“这是一个可拿起的物件”，而不是装饰性浮夸；状态始终有文字或控件状态配合颜色，危险动作保持明确的红色实心按钮和直接标签。

**Key Characteristics:**
- Grobot 是主角，预览和安全动作优先于调试细节。
- 深墨蓝底 + 青蓝品牌色 + 纸感表面形成安静层次。
- 所有主要触摸目标至少 56px 高；高级机器工具默认折叠。

## Colors

低饱和墨蓝作为工作底，纸感表面作为内容层，青蓝品牌色只标记安全注意与主动作。

### Primary
- **Grobot 青蓝** (`#32D6CB`): 主确认动作、主入口和安全注意焦点；在屏幕上保持稀缺。
- **深青辅助** (`#1B7773`): 用户消息和次级强调，承担品牌色的暗阶。

### Neutral
- **深墨蓝工作底** (`#10161C`): 主屏背景，降低长时间观看的疲劳。
- **墨蓝聊天底** (`#141C22`): 对话内容所在的次级层。
- **纸感深表面** (`#1A242B`): 控制面板、状态胶囊和纸感舞台。
- **暖纸浅底** (`#F4F1EA`): 浅色主题的工作底。
- **纸卡白** (`#FFFDF8`): 预览卡和浅色主题的内容容器。
- **浅色文字** (`#F2F0E8`): 深色主题的正文与动作标签。
- **静音文字** (`#A7B1B8`): 次要说明和不抢焦点的状态信息。
- **低对比边框** (`#34434A`): 卡片、胶囊和面板的结构边界。
- **成功绿** (`#5ECB9A`): 成功状态，与文字状态共同出现。
- **警示琥珀** (`#E4B15D`): 原点等需要谨慎确认的动作。
- **危险红** (`#E06A70`): 停止、复位等危险动作。

### Named Rules
**The Cyan Scarcity Rule.** 青蓝只负责主动作和安全注意，不把整个界面染成霓虹。

## Typography

**Display Font:** Noto Sans (with the firmware's built-in text font fallback)
**Body Font:** Noto Sans (with the firmware's built-in text font fallback)
**Label/Mono Font:** Material Symbols only for existing icon affordances; labels remain direct text.

**Character:** 字体中性、清楚、适合 480×320 的近距离触摸屏；层级靠尺寸、位置和表面差异建立，不靠细体或全大写装饰。

### Hierarchy
- **Display** (500, 30px, 1.2): Grobot 相关的主视觉或设备级标题。
- **Headline** (500, 20px, 1.3): 预览、状态和控制面板标题。
- **Body** (400, 20px, 1.4): 对话、说明和状态详情。
- **Label** (500, 20px, 1.2): 触摸按钮和状态胶囊，保持直接可读。

## Layout

Waveshare 主屏使用 480×320 横向画布。顶部图标区保持透明，状态信息居中放在约 56% 宽的软胶囊中；顶部右图标组最左有 Grbl 链路三级圆点（绿=在线就绪、琥珀=已连未就绪、灰=离线），作为文字状态之外的余光层；内容区使用 8px 基础间距和 32px 内边距，Grobot 背后是 468×308 的圆角纸感舞台。预览和机器控制采用全屏半透遮罩 + 居中卡片，避免同时看到背景动作与当前确认动作。

控制抽屉分两页互斥切换：主操作区放暂停/继续、重复、试笔和停止，「点动·手动」页（2026-08-20 由「高级调试」更名）放点动十字、设原点、解锁、电机关闭和复位，页头开关切页、开机默认停在主操作区、所选页跨抽屉开合保持。抽屉面板固定高且**不滚动**——2026-08-20 连续四轮 HIL 实测靠滚动到达满铺按钮之外的内容在 480x320 上不可交付（按钮依次丢 X/Y 十字、Y+、整段工具键，用户明确「也不能滑动」），故改为每页都塞进视口。进入串流/换纸等活动态时强制切回主操作区，保证停止键始终可达。点动 fail-closed：先发 `?` 取新鲜有限 MPos，再按 X≤190/Y≤190mm 包线判定（Y 上限 2026-08-21 由 277 收紧，与云端 §5 同源），取不到新鲜坐标或越界即拒。所有主按钮保持至少 56px 高。

## Elevation & Depth

系统采用“色调分层 + 低强度环境阴影”的混合深度。背景不使用重边框；舞台、预览卡和控制面板用低对比边框定义边界，再用短而柔和的黑色阴影与背景分离。遮罩负责暂时压低背景，不制造新的装饰层。

状态层恒在最上：顶栏与状态胶囊必须叠在整屏 Grobot 表情之上——表情是内容层，任何时候不得压盖状态层。

### Shadow Vocabulary
- **卡片环境阴影** (`shadow width 24`, `opacity 30%`): 预览卡与控制面板相对背景的结构性抬升。
- **舞台环境阴影** (`shadow width 18`, `opacity 20%`): Grobot 纸感舞台的轻微分层。

## Shapes

圆角是系统的触觉语言：状态胶囊使用 18px，预览与控制卡使用 24px，Grobot 舞台使用 32px。边框为 1px 低对比线；内容容器与全屏遮罩不保留多余圆角。图片预览额外使用 16px 圆角，形成纸张内页感。

## Components

### Buttons
- **Shape:** 可触摸的软胶囊（18px radius）；主确认和控制动作高度至少 56px。
- **Primary:** 青蓝背景、深色文字，作为唯一主动作或明确安全动作。
- **Secondary / Ghost:** 深表面或辅助气泡色背景、浅色文字；用于取消、折叠和非主动作。
- **Danger:** 红色实心、白字、独占停止行；不与普通操作混淆。
- **Disabled:** 实心灰底 + 浅灰字，并保留 `LV_STATE_DISABLED`，不只降低文字透明度。

### Chips
- **Style:** 状态和通知使用居中软胶囊，深表面背景、低对比边框和正文色；状态颜色同时通过文字表达。聆听中胶囊切成功绿、连接中切静音色，颜色始终与文字同现。

### Cards / Containers
- **Corner Style:** 预览/控制卡 24px，Grobot 舞台 32px。
- **Background:** 纸感表面色；预览图片内部使用暖纸浅色。
- **Shadow Strategy:** 使用低强度环境阴影，不使用发光阴影。
- **Border:** 1px 低对比边框。
- **Internal Padding:** 4–10 个 spacing 单位，按钮之间保留至少 8px 间距。

### Navigation
- **Style:** 不引入额外导航栏；顶部仅保留状态、通知、Wi-Fi 和电池等设备信息。

### Grobot Stage
Grobot 表情居中、尺寸由设备脸部实现决定，舞台只提供纸感承托；副标题在底部居中胶囊中显示，不能遮挡表情，也不能与预览确认卡竞争。聆听时表情带 3 秒一周期的连续扫光（介于说话 1.5s 与空闲 6s 之间），配合瞳孔放大与说话主钮变绿给出「在听」反馈。

## Do's and Don'ts

### Do:
- **Do** keep primary touch targets at or above 56px and use direct localized labels.
- **Do** preserve state-aware enable/disable behavior and pair status colors with text or control state.
- **Do** use the semantic theme tokens instead of scattering new palette literals through layout code.
- **Do** keep advanced machine controls available behind an explicit collapsed section.

### Don't:
- **Don't** make Grobot compete with the drawing preview or confirmation action.
- **Don't** turn the primary surface into a dense debug console.
- **Don't** rely on color alone to communicate machine state.
- **Don't** use full-screen opaque bars or neon-accented surfaces as default decoration.
