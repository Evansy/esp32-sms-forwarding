#include "schedule.h"
#include "common/nvs_helper.h"
#include "time/time_sync.h"
#include "sms/sms.h"
#include "../logger/logger.h"
#include <ctime>

static constexpr char kNvsSmsSched[] = "sms_sched";

SmsSchedule smsSchedules[MAX_SCHEDULED_SMS];
int         smsScheduleCount = 0;

// tick() 最低间隔 ms，避免 NVS 频繁读取
static constexpr unsigned long SCHED_TICK_INTERVAL_MS = 1000;
static unsigned long s_lastSchedTick = 0;

// ── 辅助：返回指定月份的天数 ──
static int daysInMonth(int year, int month) {
  // month: 1-12
  static const int kDim[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  if (month == 2 && (year % 4 == 0 && (year % 100 != 0 || year % 400 == 0))) {
    return 29;
  }
  return kDim[month - 1];
}

// ── 辅助：将 struct tm 规范化并转 epoch ──
static time_t mktimeAndNorm(struct tm& t) {
  return mktime(&t);
}

// ── load ──
void ScheduleStore::load() {
  NvsScope p(kNvsSmsSched, true);
  if (!p.ok()) {
    LOG("SCHED", "NVS 打开失败");
    return;
  }
  auto& prefs = p.p();

  smsScheduleCount = constrain((int)prefs.getUChar("count", 0), 0, MAX_SCHEDULED_SMS);

  for (int i = 0; i < smsScheduleCount; i++) {
    String prefix = "s" + String(i) + "_";
    auto& task = smsSchedules[i];
    task.enabled      = prefs.getBool((prefix + "en").c_str(), false);
    task.mode         = (SmsScheduleMode)prefs.getUChar((prefix + "mode").c_str(), SCHEDULE_ONCE);
    task.phone        = prefs.isKey((prefix + "phn").c_str()) ? prefs.getString((prefix + "phn").c_str(), "") : "";
    task.message      = prefs.isKey((prefix + "msg").c_str()) ? prefs.getString((prefix + "msg").c_str(), "") : "";
    task.hour         = prefs.getUChar((prefix + "hr").c_str(), 0);
    task.minute       = prefs.getUChar((prefix + "min").c_str(), 0);
    task.weekday      = prefs.getUChar((prefix + "wd").c_str(), 0);
    task.monthDay     = prefs.getUChar((prefix + "md").c_str(), 1);
    task.intervalMin  = prefs.getUShort((prefix + "ivl").c_str(), 60);
    task.nextFire     = 0;  // 运行时计算

    // 校验 mode 范围，防止 NVS 损坏导致越界
    if (task.mode > SCHEDULE_INTERVAL) {
      LOG("SCHED", "任务 %d mode 值异常 (%d)，已重置为单次", i, (int)task.mode);
      task.mode = SCHEDULE_ONCE;
      task.enabled = false;
    }
  }

  // 为所有启用的任务计算 nextFire
  for (int i = 0; i < smsScheduleCount; i++) {
    if (smsSchedules[i].enabled) {
      smsSchedules[i].nextFire = computeNextFire(i);
    }
  }

  LOG("SCHED", "已加载 %d 个定时短信任务", smsScheduleCount);
}

// ── save ──
void ScheduleStore::save() {
  NvsScope p(kNvsSmsSched, false);
  if (!p.ok()) {
    LOG("SCHED", "NVS 写入失败");
    return;
  }
  auto& prefs = p.p();

  prefs.putUChar("count", (uint8_t)smsScheduleCount);

  for (int i = 0; i < smsScheduleCount; i++) {
    String prefix = "s" + String(i) + "_";
    auto& task = smsSchedules[i];
    prefs.putBool((prefix + "en").c_str(),   task.enabled);
    prefs.putUChar((prefix + "mode").c_str(), (uint8_t)task.mode);
    prefs.putString((prefix + "phn").c_str(), task.phone);
    prefs.putString((prefix + "msg").c_str(), task.message);
    prefs.putUChar((prefix + "hr").c_str(),   task.hour);
    prefs.putUChar((prefix + "min").c_str(),  task.minute);
    prefs.putUChar((prefix + "wd").c_str(),   task.weekday);
    prefs.putUChar((prefix + "md").c_str(),   task.monthDay);
    prefs.putUShort((prefix + "ivl").c_str(),  task.intervalMin);
  }

  // 清除已删除任务残留的 key
  for (int i = smsScheduleCount; i < MAX_SCHEDULED_SMS; i++) {
    String prefix = "s" + String(i) + "_";
    prefs.remove((prefix + "en").c_str());
    prefs.remove((prefix + "mode").c_str());
    prefs.remove((prefix + "phn").c_str());
    prefs.remove((prefix + "msg").c_str());
    prefs.remove((prefix + "hr").c_str());
    prefs.remove((prefix + "min").c_str());
    prefs.remove((prefix + "wd").c_str());
    prefs.remove((prefix + "md").c_str());
    prefs.remove((prefix + "ivl").c_str());
    prefs.remove((prefix + "lf").c_str());
    prefs.remove((prefix + "nf").c_str());
  }

  LOG("SCHED", "已保存 %d 个定时短信任务", smsScheduleCount);
}

// ── computeNextFire ──
time_t ScheduleStore::computeNextFire(int index) {
  if (index < 0 || index >= smsScheduleCount) return 0;
  const SmsSchedule& task = smsSchedules[index];

  if (!TimeSync::isSynced()) return 0;
  time_t now = time(nullptr);
  if (now < 100000) return 0;

  struct tm t;
  localtime_r(&now, &t);

  switch (task.mode) {
    case SCHEDULE_ONCE: {
      // 单次模式：使用 NVS 中存储的触发时间
      char nfKey[12];
      snprintf(nfKey, sizeof(nfKey), "s%d_nf", index);
      uint32_t storedNf = Nvs::getUInt(kNvsSmsSched, nfKey, 0);
      if (storedNf > 0) {
        return (time_t)storedNf;
      }
      return 0;  // 未设置触发时间
    }

    case SCHEDULE_DAILY: {
      struct tm target = t;
      target.tm_hour = task.hour;
      target.tm_min  = task.minute;
      target.tm_sec  = 0;
      time_t fireEpoch = mktimeAndNorm(target);
      if (fireEpoch <= now) {
        fireEpoch += 86400;  // 明天（CST 无夏令时，86400 安全）
      }
      return fireEpoch;
    }

    case SCHEDULE_WEEKLY: {
      struct tm target = t;
      target.tm_hour = task.hour;
      target.tm_min  = task.minute;
      target.tm_sec  = 0;
      int daysAhead = ((int)task.weekday - t.tm_wday + 7) % 7;
      if (daysAhead == 0) {
        time_t candidate = mktimeAndNorm(target);
        if (candidate <= now) {
          daysAhead = 7;
        }
      }
      target.tm_mday += daysAhead;
      mktimeAndNorm(target);  // 规范化日溢出
      target.tm_hour = task.hour;
      target.tm_min  = task.minute;
      target.tm_sec  = 0;
      return mktimeAndNorm(target);
    }

    case SCHEDULE_MONTHLY: {
      struct tm target = t;
      int actualDay = min((int)task.monthDay, daysInMonth(t.tm_year + 1900, t.tm_mon + 1));
      target.tm_mday = actualDay;
      target.tm_hour = task.hour;
      target.tm_min  = task.minute;
      target.tm_sec  = 0;
      time_t fireEpoch = mktimeAndNorm(target);
      if (fireEpoch <= now) {
        // 下个月
        target.tm_mon++;
        mktimeAndNorm(target);  // 规范化月份溢出
        int actualDayNext = min((int)task.monthDay, daysInMonth(target.tm_year + 1900, target.tm_mon + 1));
        target.tm_mday = actualDayNext;
        target.tm_hour = task.hour;
        target.tm_min  = task.minute;
        target.tm_sec  = 0;
        fireEpoch = mktimeAndNorm(target);
      }
      return fireEpoch;
    }

    case SCHEDULE_INTERVAL: {
      char lfKey[12];
      snprintf(lfKey, sizeof(lfKey), "s%d_lf", index);
      uint32_t lastFired = Nvs::getUInt(kNvsSmsSched, lfKey, 0);
      if (lastFired == 0) {
        return now + (time_t)(task.intervalMin * 60);
      }
      time_t next = (time_t)lastFired + (time_t)(task.intervalMin * 60);
      // 追赶：如果 next 远在过去，跳到下一个间隔
      while (next + 60 <= now) {
        next += (time_t)(task.intervalMin * 60);
      }
      return next;
    }

    default:
      return 0;
  }
}

// ── validateTask ──
bool ScheduleStore::validateTask(const SmsSchedule& task) {
  if (task.phone.length() == 0) return false;
  if (task.message.length() == 0) return false;
  if (task.message.length() > 160) return false;
  if (task.hour > 23) return false;
  if (task.minute > 59) return false;
  if (task.mode == SCHEDULE_ONCE && task.nextFire == 0) return false;  // 单次模式必须指定触发时间
  if (task.mode == SCHEDULE_WEEKLY && task.weekday > 6) return false;
  if (task.mode == SCHEDULE_MONTHLY && (task.monthDay < 1 || task.monthDay > 31)) return false;
  if (task.mode == SCHEDULE_INTERVAL && task.intervalMin < 1) return false;
  return true;
}

// ── modeLabel ──
const char* ScheduleStore::modeLabel(SmsScheduleMode mode) {
  switch (mode) {
    case SCHEDULE_ONCE:    return "单次";
    case SCHEDULE_DAILY:   return "每天";
    case SCHEDULE_WEEKLY:  return "每周";
    case SCHEDULE_MONTHLY: return "每月";
    case SCHEDULE_INTERVAL:return "间隔";
    default:               return "未知";
  }
}

// ── tick（含 1 秒节流）──
void ScheduleStore::tick() {
  unsigned long nowMs = millis();
  if (nowMs - s_lastSchedTick < SCHED_TICK_INTERVAL_MS) return;
  s_lastSchedTick = nowMs;

  if (!TimeSync::isSynced()) return;

  time_t now = time(nullptr);
  if (now < 100000) return;

  for (int i = 0; i < smsScheduleCount; i++) {
    SmsSchedule& task = smsSchedules[i];
    if (!task.enabled) continue;

    // 跳过无效任务（空号码/内容），安全防护
    if (task.phone.length() == 0 || task.message.length() == 0) continue;

    // 延迟计算 nextFire
    if (task.nextFire == 0) {
      task.nextFire = computeNextFire(i);
      if (task.nextFire == 0) continue;
    }

    // 检查是否在触发窗口内（当前时间 >= nextFire 且 < nextFire + 60s）
    if (now < task.nextFire) continue;
    if (now >= task.nextFire + 60) {
      // 已过触发窗口
      if (task.mode == SCHEDULE_ONCE) {
        // 单次模式已过期，禁用该任务
        task.enabled = false;
        task.nextFire = 0;
        char nfKey[12];
        snprintf(nfKey, sizeof(nfKey), "s%d_nf", i);
        Nvs::putUInt(kNvsSmsSched, nfKey, 0);
        save();
        LOG("SCHED", "单次任务 %d 已过期，已禁用", i);
      } else {
        task.nextFire = computeNextFire(i);
      }
      continue;
    }

    // 在触发窗口内：检查是否已经触发过（防重复）
    char lfKey[12];
    snprintf(lfKey, sizeof(lfKey), "s%d_lf", i);
    uint32_t lastFired = Nvs::getUInt(kNvsSmsSched, lfKey, 0);
    if (lastFired >= (uint32_t)task.nextFire) {
      // 本次触发窗口已触发过（如重启后），重新计算下次
      task.nextFire = computeNextFire(i);
      continue;
    }

    // ─── 触发！───
    LOG("SCHED", "定时短信触发: 任务%d -> %s", i, task.phone.c_str());

    // 先记录触发时间到 NVS（防断电极重触）
    Nvs::putUInt(kNvsSmsSched, lfKey, (uint32_t)now);

    Sms::sendPDU(task.phone.c_str(), task.message.c_str());

    if (task.mode == SCHEDULE_ONCE) {
      task.enabled = false;
      task.nextFire = 0;
      char nfKey[12];
      snprintf(nfKey, sizeof(nfKey), "s%d_nf", i);
      Nvs::putUInt(kNvsSmsSched, nfKey, 0);
      save();
    } else {
      task.nextFire = computeNextFire(i);
    }

    // 每次只触发一条，避免 SMS 阻塞叠加
    return;
  }
}

// ── addTask ──
int ScheduleStore::addTask(const SmsSchedule& task) {
  if (smsScheduleCount >= MAX_SCHEDULED_SMS) return -1;
  if (!validateTask(task)) return -1;

  int idx = smsScheduleCount;
  smsSchedules[idx] = task;

  // 对于单次模式，将 nextFire 写入 NVS 的 nf key
  if (task.mode == SCHEDULE_ONCE && task.nextFire > 0) {
    char nfKey[12];
    snprintf(nfKey, sizeof(nfKey), "s%d_nf", idx);
    Nvs::putUInt(kNvsSmsSched, nfKey, (uint32_t)task.nextFire);
  }

  smsScheduleCount++;
  save();

  // 重新计算 nextFire
  smsSchedules[idx].nextFire = computeNextFire(idx);

  LOG("SCHED", "添加定时短信任务 %d: %s -> %s", idx, modeLabel(task.mode), task.phone.c_str());
  return idx;
}

// ── updateTask ──
bool ScheduleStore::updateTask(int index, const SmsSchedule& task) {
  if (index < 0 || index >= smsScheduleCount) return false;
  if (!validateTask(task)) return false;

  // 清除旧的 lastFired 和 nextFire NVS key
  char lfKey[12], nfKey[12];
  snprintf(lfKey, sizeof(lfKey), "s%d_lf", index);
  snprintf(nfKey, sizeof(nfKey), "s%d_nf", index);
  Nvs::putUInt(kNvsSmsSched, lfKey, 0);
  Nvs::putUInt(kNvsSmsSched, nfKey, 0);

  smsSchedules[index] = task;

  // 单次模式：将 nextFire 存入 NVS
  if (task.mode == SCHEDULE_ONCE && task.nextFire > 0) {
    Nvs::putUInt(kNvsSmsSched, nfKey, (uint32_t)task.nextFire);
  }

  smsSchedules[index].nextFire = computeNextFire(index);
  save();

  LOG("SCHED", "更新定时短信任务 %d", index);
  return true;
}

// ── deleteTask ──
bool ScheduleStore::deleteTask(int index) {
  if (index < 0 || index >= smsScheduleCount) return false;

  LOG("SCHED", "删除定时短信任务 %d: %s", index, smsSchedules[index].phone.c_str());

  // 清除 NVS 中该任务的 lf 和 nf key
  char lfKey[12], nfKey[12];
  snprintf(lfKey, sizeof(lfKey), "s%d_lf", index);
  snprintf(nfKey, sizeof(nfKey), "s%d_nf", index);
  Nvs::putUInt(kNvsSmsSched, lfKey, 0);
  Nvs::putUInt(kNvsSmsSched, nfKey, 0);

  // 前移后续任务
  for (int i = index; i < smsScheduleCount - 1; i++) {
    smsSchedules[i] = smsSchedules[i + 1];
  }
  smsScheduleCount--;

  // 清空最后一个槽位
  smsSchedules[smsScheduleCount] = SmsSchedule{};

  save();

  // 重新计算所有启用任务的 nextFire（索引已变）
  for (int i = 0; i < smsScheduleCount; i++) {
    if (smsSchedules[i].enabled) {
      smsSchedules[i].nextFire = computeNextFire(i);
    }
  }

  return true;
}