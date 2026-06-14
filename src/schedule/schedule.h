#pragma once
#include <Arduino.h>
#include <ctime>

// 定时短信调度模式
enum SmsScheduleMode : uint8_t {
  SCHEDULE_ONCE    = 0,  // 单次，触发后自动禁用
  SCHEDULE_DAILY   = 1,  // 每天
  SCHEDULE_WEEKLY  = 2,  // 每周
  SCHEDULE_MONTHLY = 3,  // 每月
  SCHEDULE_INTERVAL= 4   // 间隔 N 分钟
};

constexpr int MAX_SCHEDULED_SMS = 10;

struct SmsSchedule {
  bool           enabled;
  SmsScheduleMode mode;
  String         phone;        // 目标号码
  String         message;      // 短信内容
  uint8_t        hour;         // 时 (0-23)，daily/weekly/monthly 用
  uint8_t        minute;       // 分 (0-59)，daily/weekly/monthly 用
  uint8_t        weekday;      // 星期 (0=Sun..6=Sat)，weekly 用
  uint8_t        monthDay;     // 月中第几天 (1-31)，monthly 用
  uint16_t       intervalMin;  // 间隔分钟数，interval 用
  time_t         nextFire;     // 下次触发 epoch（运行时计算，不持久化）
};

extern SmsSchedule smsSchedules[MAX_SCHEDULED_SMS];
extern int         smsScheduleCount;

// 定时短信调度引擎：加载、持久化、触发、CRUD。
class ScheduleStore {
public:
  // 从 NVS 加载所有定时短信任务（setup 中调用）
  static void load();

  // 将所有任务写入 NVS（增删改后调用）
  static void save();

  // 在主循环中周期调用，检查并触发到期任务。
  // 每次调用最多触发一条任务（避免多任务同时阻塞）。
  static void tick();

  // --- CRUD ---

  // 追加新任务，返回分配的索引；失败（列表已满或校验不通过）返回 -1。
  static int addTask(const SmsSchedule& task);

  // 更新指定索引的任务；失败（索引越界或校验不通过）返回 false。
  static bool updateTask(int index, const SmsSchedule& task);

  // 删除指定索引的任务并前移后续项；失败返回 false。
  static bool deleteTask(int index);

  // 返回指定模式的中文标签
  static const char* modeLabel(SmsScheduleMode mode);

private:
  // 根据调度模式计算下次触发时间
  static time_t computeNextFire(int index);

  // 校验任务字段合法性
  static bool validateTask(const SmsSchedule& task);
};