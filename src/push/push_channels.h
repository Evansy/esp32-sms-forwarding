#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>
#include "config/config.h"

// 两次 HTTP 请求之间的最小冷却时间，防止密集请求导致 ESP32 TLS/TCP 栈资源
// （mbedtls 堆、lwIP socket）尚未充分释放就开始下一次 TLS 握手。
constexpr unsigned long HTTP_COOLDOWN_MS = 500;

// 消息体类型：DEFAULT = 原始短信内容，CUSTOM = 用户自定义渲染后的 body。
// content 均已经过 sanitizeText 净化。
enum PushBodyType { PUSH_BODY_DEFAULT, PUSH_BODY_CUSTOM };
struct PushBody {
  PushBodyType type;
  String       content;
};

// 各推送渠道的具体发送实现。
// 约定：message.type == PUSH_BODY_CUSTOM 时 message.content 是用户自定义请求体，
//   否则 message.content 是原始短信文本；两者均已净化。
// 所有方法都是同步阻塞的；返回 true 表示 HTTP 状态码符合该渠道的成功语义。
//
// 重要：每个发送函数内部都使用**栈上**的 WiFiClientSecure / HTTPClient，
// 严禁共享，否则在并发推送或与 OTA 同时下载时会导致 TLS 状态损坏。
class PushChannels {
public:
#if FEATURE_PUSH_POST_JSON
  static bool sendPostJson  (const PushChannel& ch, const String& sender, const PushBody& message, const String& timestamp);
#endif
#if FEATURE_PUSH_BARK
  static bool sendBark      (const PushChannel& ch, const String& sender, const PushBody& message, const String& timestamp);
#endif
#if FEATURE_PUSH_GET
  static bool sendGet       (const PushChannel& ch, const String& sender, const PushBody& message, const String& timestamp);
#endif
#if FEATURE_PUSH_DINGTALK
  static bool sendDingtalk  (const PushChannel& ch, const String& sender, const PushBody& message, const String& timestamp);
#endif
#if FEATURE_PUSH_PUSHPLUS
  static bool sendPushPlus  (const PushChannel& ch, const String& sender, const PushBody& message, const String& timestamp);
#endif
#if FEATURE_PUSH_SERVERCHAN
  static bool sendServerChan(const PushChannel& ch, const String& sender, const PushBody& message, const String& timestamp);
#endif
#if FEATURE_PUSH_CUSTOM
  static bool sendCustom    (const PushChannel& ch, const String& sender, const PushBody& message, const String& timestamp);
#endif
#if FEATURE_PUSH_FEISHU
  static bool sendFeishu    (const PushChannel& ch, const String& sender, const PushBody& message, const String& timestamp);
#endif
#if FEATURE_PUSH_GOTIFY
  static bool sendGotify    (const PushChannel& ch, const String& sender, const PushBody& message, const String& timestamp);
#endif
#if FEATURE_PUSH_TELEGRAM
  static bool sendTelegram  (const PushChannel& ch, const String& sender, const PushBody& message, const String& timestamp);
#endif
#if FEATURE_PUSH_WECHAT_WORK
  static bool sendWechatWork(const PushChannel& ch, const String& sender, const PushBody& message, const String& timestamp);
#endif
#if FEATURE_PUSH_SMS
  static bool sendSmsPush   (const PushChannel& ch, const String& sender, const PushBody& message, const String& timestamp);
#endif
};