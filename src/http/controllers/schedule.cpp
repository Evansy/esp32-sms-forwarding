#include "schedule.h"
#include "schedule/schedule.h"
#include "http/body_accumulator.h"
#include "http/json_response.h"
#include "common/nvs_helper.h"
#include "time/time_sync.h"
#include "../../logger/logger.h"
#include <ArduinoJson.h>

// ── GET /api/schedule ──
// 返回所有定时短信任务列表
void scheduleGetController(AsyncWebServerRequest* request) {
  JsonResp::build(request, [](JsonObject root) {
    JsonArray tasksArr = root["tasks"].to<JsonArray>();
    for (int i = 0; i < smsScheduleCount; i++) {
      const auto& task = smsSchedules[i];
      JsonObject obj = tasksArr.add<JsonObject>();
      obj["index"]       = i;
      obj["enabled"]     = task.enabled;
      obj["mode"]        = (int)task.mode;
      obj["modeLabel"]   = ScheduleStore::modeLabel(task.mode);
      obj["phone"]       = task.phone;
      obj["message"]     = task.message;
      obj["hour"]        = task.hour;
      obj["minute"]      = task.minute;
      obj["weekday"]     = task.weekday;
      obj["monthDay"]    = task.monthDay;
      obj["intervalMin"] = task.intervalMin;
      obj["nextFire"]    = (long long)task.nextFire;  // time_t 可能为 64 位
    }
    root["count"]     = smsScheduleCount;
    root["maxCount"]  = MAX_SCHEDULED_SMS;
    root["timeSynced"] = TimeSync::isSynced();
  });
}

// ── POST /api/schedule ──
// JSON body: { "action": "create"|"update"|"delete"|"enable"|"disable", "index": N, "task": { ... } }
void schedulePostController(AsyncWebServerRequest* request, uint8_t* data,
                            size_t len, size_t index, size_t total) {
  const char* requestBody = nullptr;
  if (!httpAccumulateBody(request, data, len, index, total, HTTP_JSON_BODY_MAX_BYTES, &requestBody)) return;
  if (requestBody == nullptr) return;

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, requestBody);
  httpReleaseAccumulatedBody(request);
  if (err) {
    JsonResp::err(request, 400, "JSON 解析失败");
    return;
  }

  String action = doc["action"] | "";
  if (action.length() == 0) {
    JsonResp::err(request, 400, "缺少 action 字段");
    return;
  }

  if (action == "create") {
    if (!doc["task"].is<JsonObject>()) {
      JsonResp::err(request, 400, "缺少 task 对象");
      return;
    }
    JsonObject t = doc["task"].as<JsonObject>();

    SmsSchedule task = {};
    task.enabled      = t["enabled"] | true;
    task.mode         = (SmsScheduleMode)(t["mode"] | 1);  // 默认每天
    task.phone        = t["phone"] | String("");
    task.message      = t["message"] | String("");
    task.hour         = t["hour"] | 0;
    task.minute       = t["minute"] | 0;
    task.weekday      = t["weekday"] | 0;
    task.monthDay     = t["monthDay"] | 1;
    task.intervalMin  = t["intervalMin"] | 60;
    task.nextFire     = 0;

    // 单次模式可指定 nextFire（epoch）
    if (task.mode == SCHEDULE_ONCE && t["nextFire"].is<long long>()) {
      task.nextFire = (time_t)(t["nextFire"].as<long long>());
    }

    // 校验
    String phoneTrim = task.phone;
    phoneTrim.trim();
    task.phone = phoneTrim;
    if (task.phone.length() == 0) {
      JsonResp::err(request, 400, "号码不能为空");
      return;
    }
    if (task.message.length() == 0) {
      JsonResp::err(request, 400, "内容不能为空");
      return;
    }
    if (task.message.length() > 160) {
      JsonResp::err(request, 400, "内容不能超过160字符");
      return;
    }

    int idx = ScheduleStore::addTask(task);
    if (idx < 0) {
      JsonResp::err(request, 400, "任务列表已满（最多" + String(MAX_SCHEDULED_SMS) + "个）");
      return;
    }

    LOG("SCHED", "API 创建任务 %d: %s -> %s", idx, ScheduleStore::modeLabel(task.mode), task.phone.c_str());
    JsonResp::ok(request, "已创建定时短信任务");

  } else if (action == "update") {
    int idx = doc["index"] | -1;
    if (idx < 0 || idx >= smsScheduleCount) {
      JsonResp::err(request, 400, "无效的任务索引");
      return;
    }
    if (!doc["task"].is<JsonObject>()) {
      JsonResp::err(request, 400, "缺少 task 对象");
      return;
    }
    JsonObject t = doc["task"].as<JsonObject>();

    SmsSchedule task = smsSchedules[idx];  // 保留现有值作为默认
    if (t["enabled"].is<bool>())    task.enabled     = t["enabled"].as<bool>();
    if (t["mode"].is<int>())        task.mode         = (SmsScheduleMode)(t["mode"].as<int>());
    if (t["phone"].is<const char*>()) task.phone     = t["phone"].as<String>();
    if (t["message"].is<const char*>()) task.message  = t["message"].as<String>();
    if (t["hour"].is<int>())        task.hour         = constrain(t["hour"].as<int>(), 0, 23);
    if (t["minute"].is<int>())      task.minute       = constrain(t["minute"].as<int>(), 0, 59);
    if (t["weekday"].is<int>())     task.weekday      = constrain(t["weekday"].as<int>(), 0, 6);
    if (t["monthDay"].is<int>())    task.monthDay     = constrain(t["monthDay"].as<int>(), 1, 31);
    if (t["intervalMin"].is<int>()) task.intervalMin  = constrain(t["intervalMin"].as<int>(), 1, 65535);

    String phoneTrim = task.phone;
    phoneTrim.trim();
    task.phone = phoneTrim;
    task.nextFire = 0;  // 更新后重新计算

    // 单次模式可指定 nextFire
    if (task.mode == SCHEDULE_ONCE && t["nextFire"].is<long long>()) {
      task.nextFire = (time_t)(t["nextFire"].as<long long>());
    }

    if (!ScheduleStore::updateTask(idx, task)) {
      JsonResp::err(request, 400, "更新失败，请检查参数");
      return;
    }

    LOG("SCHED", "API 更新任务 %d", idx);
    JsonResp::ok(request, "已更新定时短信任务");

  } else if (action == "delete") {
    int idx = doc["index"] | -1;
    if (idx < 0 || idx >= smsScheduleCount) {
      JsonResp::err(request, 400, "无效的任务索引");
      return;
    }

    if (!ScheduleStore::deleteTask(idx)) {
      JsonResp::err(request, 500, "删除失败");
      return;
    }

    LOG("SCHED", "API 删除任务 %d", idx);
    JsonResp::ok(request, "已删除定时短信任务");

  } else if (action == "enable") {
    int idx = doc["index"] | -1;
    if (idx < 0 || idx >= smsScheduleCount) {
      JsonResp::err(request, 400, "无效的任务索引");
      return;
    }
    auto& task = smsSchedules[idx];
    if (task.phone.length() == 0 || task.message.length() == 0) {
      JsonResp::err(request, 400, "任务缺少号码或内容，无法启用");
      return;
    }
    if (task.mode == SCHEDULE_ONCE && task.nextFire == 0) {
      // 尝试从 NVS 读取 nf 看是否有存储的触发时间
      char nfKey[12];
      snprintf(nfKey, sizeof(nfKey), "s%d_nf", idx);
      uint32_t storedNf = Nvs::getUInt("sms_sched", nfKey, 0);
      if (storedNf == 0) {
        JsonResp::err(request, 400, "单次任务未设置触发时间，无法启用");
        return;
      }
    }
    task.enabled = true;
    task.nextFire = 0;  // 重新计算
    ScheduleStore::save();
    LOG("SCHED", "API 启用任务 %d", idx);
    JsonResp::ok(request, "已启用定时短信任务");

  } else if (action == "disable") {
    int idx = doc["index"] | -1;
    if (idx < 0 || idx >= smsScheduleCount) {
      JsonResp::err(request, 400, "无效的任务索引");
      return;
    }
    smsSchedules[idx].enabled = false;
    smsSchedules[idx].nextFire = 0;
    ScheduleStore::save();
    LOG("SCHED", "API 禁用任务 %d", idx);
    JsonResp::ok(request, "已禁用定时短信任务");

  } else {
    JsonResp::err(request, 400, "未知的 action: " + action);
  }
}