# AGENTS.md

ESP32-C3 + ML307R/C/A 短信转发器项目指南。

## 项目概述

ESP32-C3 通过 UART 连接 ML307R 4G 模组，接收短信后通过 WiFi 推送到多种渠道（Bark、飞书、钉钉、Telegram 等）。支持 Web 管理界面、OTA 升级、定时短信、黑名单、飞行模式控制等。

- **硬件**：ESP32-C3 Super Mini + ML307R/C/A 4G 核心板
- **框架**：C++17, Arduino, PlatformIO
- **Flash 分区**：app0(1.75MB) + app1(1.75MB, OTA) + LittleFS(320KB) + coredump(64KB) + NVS

## 目录结构

```
src/
├── main.cpp              # 入口：setup() 初始化各模块，loop() 轮询状态机
├── config/               # 配置结构体 + NVS 读写
├── sim/                  # SIM 卡 AT 指令、状态机（SIM_INIT→SIM_WAIT→SIM_READY）、热插拔
│   └── sim_dispatcher    # AT 指令串行分发器（所有模组通信经过此层）
├── sms/                  # PDU 短信解析（pdulib）、长短信合并、发送
├── call/                 # 来电检测与通知
├── push/                 # 推送核心
│   ├── push.cpp          # Push::send() 入口，策略分发（广播/故障转移）
│   ├── push_channels.cpp # 12 种推送类型实现（每种可独立 feature flag 开关）
│   ├── push_queue.cpp    # 推送队列（串行执行，防止并发）
│   └── push_retry.cpp    # 失败重试逻辑
├── http/                 # Web 服务
│   ├── http_server.cpp   # 路由注册、Basic Auth 中间件
│   ├── json_response.h   # 统一 JSON 响应封装
│   ├── body_accumulator.h# POST body 分片累积（ESP32 内存受限）
│   └── controllers/      # 各 API 控制器（每个 .cpp 对应一组路由）
├── wifi/                 # 多 WiFi 有序连接、AP 模式、自动重连
├── time/                 # 时间同步（NTP + SIM NITZ）
├── schedule/             # 定时短信调度器（持久化到 NVS）
├── ota/                  # OTA 在线升级 + 手动上传
├── logger/               # 串口 + 内存环形缓冲 + 可选文件日志
├── coredump/             # ESP32 崩溃 core dump 捕获与导出
├── ble/                  # BluFi BLE 配网（可选，feature flag 默认关闭）
└── app_description.cpp   # 自定义固件版本描述（ESPConnect 可读）
data/
├── index.html            # 配置管理页面（账号、推送、WiFi、黑名单、定时重启）
└── tools.html            # 工具箱页面（状态、通信、诊断、调试、维护 5 个 tab）
script/
├── upload_littlefs.py    # pio run -t upload 的后置脚本：gzip 压缩 data/ 再上传
├── data_utils.py         # gzip 压缩工具函数
├── compress_data.py      # 独立压缩脚本（CI 用）
├── restore_data.py       # 恢复原始文件
├── inject_app_version.py # 编译时注入 git 版本号
└── generate_compilation_database.py
```

## 核心数据流

### 短信接收 → 推送

```
ML307R UART → SimDispatcher (串行读取)
  → Sms::onUrc() 解析 +CMT URC
  → PDU 解析 → 长短信合并（30s 超时）
  → Push::send() 策略分发
    → PushQueue 入队
    → PushRetry 失败重试
    → PushChannel::dispatch() 按类型调用 HTTP/MQTT
```

### Web API 请求

```
HTTP 请求 → Basic Auth 中间件（/api/health 免认证）
  → 路由分发到 controller
  → controller 调用 ConfigStore / Sim / Sms 等模块
  → JsonResp::ok() / JsonResp::err() 返回
```

### AT 指令通道

所有模组通信通过 `SimDispatcher` 串行化：
- `SimDispatcher::sendCommand()` — 同步发送 AT 指令，等待响应
- `SimDispatcher::pauseReader()` / `resumeReader()` — 暂停自动读取（用于 Ping 等异步多行响应）
- 模组事件（短信、来电）通过 URC 回调处理

## Web UI 架构

两个单页 HTML，纯原生 JS（无框架），gzip 后烧录到 LittleFS：

- **index.html**（配置页 `/`）：4 个 tab — 常规、推送、网络、高级
- **tools.html**（工具页 `/tools`）：5 个 tab — 状态、通信、诊断、调试、维护

样式统一：`#1a73e8` 主色、8px 圆角卡片、13px 正文、紧凑表单。

### 工具页 tab 分组

| Tab | 功能 |
|-----|------|
| 状态 | 设备仪表盘（IP/内存/运行时长/Flash）、快捷操作（Ping/重启）、SOC 详情 |
| 通信 | 发送短信、定时短信 CRUD、黑名单管理 |
| 诊断 | 模组查询（固件/信号/SIM/网络/WiFi）、Ping 测试、飞行模式 |
| 调试 | AT 指令控制台（直接发送到模组） |
| 维护 | OTA 升级（在线+手动）、日志、崩溃记录、配置导入导出、重置 |

## 构建与烧录

```bash
# 需要先激活 venv（项目根目录下）
source .venv/bin/activate

# 编译
pio run

# 烧录固件 + 自动上传 LittleFS（gzip 压缩后上传）
pio run -t upload

# 仅上传 Web UI 文件系统
pio run -t uploadfs
```

`pio run -t upload` 会自动触发 `script/upload_littlefs.py`，流程：
1. gzip 压缩 `data/` 下所有文件
2. 构建 LittleFS 镜像并上传
3. 恢复原始未压缩文件

## Feature Flags

在 `platformio.ini` 的 `build_flags` 中控制编译开关：

| Flag | 默认 | 说明 |
|------|------|------|
| `FEATURE_COREDUMP` | 开启 | 崩溃 core dump 捕获 |
| `FEATURE_BLUFI` | 关闭 | BluFi BLE 配网 |
| `FEATURE_CALL` | 关闭 | 来电通知 |
| `FEATURE_LOG_FILE` | 关闭 | 日志写入文件 |
| `FEATURE_PUSH_*` | 各不同 | 各推送通道类型（Bark/飞书/SMS 默认开启） |

## 注意事项

1. **串口独占**：模组 UART (Serial1) 由 `SimDispatcher` 管理，所有 AT 指令必须经过它，不能直接操作 Serial1
2. **内存紧张**：ESP32-C3 只有 320KB RAM，HTTP body 使用 `body_accumulator.h` 分片累积，有大小限制 (`HTTP_JSON_BODY_MAX_BYTES`)
3. **看门狗**：长时间操作（模组上电、LittleFS 格式化）需调用 `esp_task_wdt_reset()` 喂狗
4. **Web UI 无构建步骤**：直接编辑 `data/*.html`，烧录时自动 gzip。不需要 npm/node
5. **OTA 双分区**：app0/app1 交替写入，在线升级失败可回滚
6. **CSRF 保护**：重置配置和重启设备需要先请求 `/api/tools/reset-token` 获取一次性 token
7. **Basic Auth**：所有路由需要认证（除了 `/api/health`），修改密码后需重新登录
8. **定时短信**：依赖 NTP/SIM 时间同步，未同步时任务不会触发
9. **gzip 服务**：ESPAsyncWebServer 优先返回 `.gz` 文件 + `Content-Encoding: gzip`，所以 LittleFS 只需存 .gz 版本
