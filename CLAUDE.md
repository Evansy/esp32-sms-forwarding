# ESP32 短信转发器

## 构建
```bash
source .venv/bin/activate   # 必须先激活 venv
pio run                     # 编译
pio run -t upload           # 烧录固件 + 自动上传 gzip 压缩的 LittleFS
pio run -t uploadfs         # 仅上传 Web UI 文件系统
```

## 架构速览
- ESP32-C3 (Arduino/C++17) 通过 UART 连接 ML307R 4G 模组
- 所有模组 AT 通信经 SimDispatcher 串行化，不要直接操作 Serial1
- Web UI 是两个原生 HTML（data/index.html, data/tools.html），无构建步骤，烧录时自动 gzip
- 配置持久化到 NVS；推送队列串行执行防并发

## 注意
- 内存紧张（320KB RAM），HTTP body 有大小限制，避免大 JSON
- 长操作需喂狗 esp_task_wdt_reset()
- 重置/重启 API 需要 CSRF token（先 GET /api/tools/reset-token）
- Feature flags 在 platformio.ini 的 build_flags 中控制
- .gitignore 不要改成 `*`（venv 创建时会覆盖）

## 代码风格
- 命名：模块 PascalCase (Sim::, Push::, ConfigStore::)，变量 camelCase，常量 kCamelCase
- 日志：LOG("TAG", "msg", args...) 宏
- JSON 响应：JsonResp::ok() / JsonResp::err()
- 配置字段：ConfigStore 前缀 config.
