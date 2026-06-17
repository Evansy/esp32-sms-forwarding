#include "http_server.h"
#include "controllers/save.h"
#include "controllers/tools.h"
#include "controllers/config.h"
#include "controllers/status.h"
#include "controllers/health.h"
#include "controllers/soc.h"
#include "controllers/wifi.h"
#include "controllers/blacklist.h"
#include "controllers/schedule.h"
#include "controllers/ota.h"
#include "controllers/logs.h"
#include "config/config.h"
#include "../logger/logger.h"
#include <LittleFS.h>

// Auth whitelist: routes that require NO authentication
static const char* const AUTH_WHITELIST[] = {"/api/health", nullptr};

static AsyncAuthenticationMiddleware g_authMiddleware;

static AsyncMiddlewareFunction g_conditionalAuth([](AsyncWebServerRequest* req, ArMiddlewareNext next) {
  const char* url = req->url().c_str();
  for (int i = 0; AUTH_WHITELIST[i] != nullptr; ++i) {
    if (strcmp(url, AUTH_WHITELIST[i]) == 0) { next(); return; }
  }
  if (!g_authMiddleware.allowed(req)) {
    req->requestAuthentication(AsyncAuthType::AUTH_BASIC, "SMS Forwarding", "请输入账号密码");
    return;
  }
  next();
});

void HttpServer::refreshAuthCredentials() {
  g_authMiddleware.setUsername(config.webUser.c_str());
  g_authMiddleware.setPassword(config.webPass.c_str());
  g_authMiddleware.generateHash();
  LOG("HTTP", "认证凭证已更新");
}

void HttpServer::setup(AsyncWebServer& server) {
  g_authMiddleware.setUsername(config.webUser.c_str());
  g_authMiddleware.setPassword(config.webPass.c_str());
  g_authMiddleware.setAuthType(AsyncAuthType::AUTH_BASIC);
  g_authMiddleware.setRealm("SMS Forwarding");
  g_authMiddleware.generateHash();

  server.addMiddleware(&g_conditionalAuth);

  // API routes (new)
  server.on("/api/config/export", HTTP_GET, configExportController);
  server.on("/api/config/import", HTTP_POST,
    [](AsyncWebServerRequest* request) {},
    nullptr,
    configImportController);
  server.on("/api/config/detail",  HTTP_GET,  configController);
  server.on("/api/status",  HTTP_GET,  statusController);
  server.on("/api/health",  HTTP_GET,  healthController);
  server.on("/api/logs",    HTTP_GET,    logsController);
  server.on("/api/logs",    HTTP_DELETE, logsDeleteController);
  server.on("/api/soc",     HTTP_GET,  socController);

  // WiFi configuration API
  server.on("/api/wifi", HTTP_GET, wifiGetController);
  server.on("/api/wifi", HTTP_POST,
    [](AsyncWebServerRequest* request) {},
    nullptr,
    wifiPostController);

  // Blacklist API
  server.on("/api/blacklist", HTTP_GET, blacklistGetController);
  server.on("/api/blacklist", HTTP_POST,
    [](AsyncWebServerRequest* request) {},
    nullptr,
    blacklistPostController);

  // Schedule API
  server.on("/api/schedule", HTTP_GET, scheduleGetController);
  server.on("/api/schedule", HTTP_POST,
    [](AsyncWebServerRequest* request) {},
    nullptr,
    schedulePostController);

  // Configuration save
  server.on("/api/save", HTTP_POST, saveController);
  server.on("/api/reboot", HTTP_POST, saveRebootController);

  // Tool handlers
  server.on("/sendsms", HTTP_POST, sendSmsController);
  server.on("/ping",    HTTP_POST, pingController);
  server.on("/query",   HTTP_GET,  queryController);
  server.on("/flight",  HTTP_GET,  flightModeController);
  server.on("/at",      HTTP_GET,  atCommandController);

  // Config reset / reboot API
  server.on("/api/tools/reset-token", HTTP_GET, resetTokenController);
  server.on("/api/tools/reset", HTTP_POST,
    [](AsyncWebServerRequest* request) {},
    nullptr,
    resetConfigController);
  server.on("/api/tools/reboot", HTTP_POST,
    [](AsyncWebServerRequest* request) {},
    nullptr,
    rebootController);
  #if FEATURE_COREDUMP
  server.on("/api/tools/coredump/info",   HTTP_GET, coredumpInfoController);
  server.on("/api/tools/coredump/export", HTTP_GET, exportCoreDumpController);
#endif

  // OTA upgrade API
  server.on("/api/ota/status",  HTTP_GET,  otaStatusController);
  server.on("/api/ota/version", HTTP_GET,  otaVersionController);
  server.on("/api/ota/start",   HTTP_POST,
    [](AsyncWebServerRequest* request) {},
    nullptr,
    otaStartController);
  server.on("/api/ota/upload",  HTTP_POST,
    otaUploadCompleteController,
    otaUploadChunkController,
    nullptr);
  server.on("/api/ota/upload-fs", HTTP_POST,
    otaUploadFsCompleteController,
    otaUploadFsChunkController,
    nullptr);

  // Suppress LittleFS error logs for common browser auto-requests
  server.on("/favicon.ico", HTTP_GET, [](AsyncWebServerRequest* request) {
    request->send(204);
  });

  // Static pages — prefer .gz (compressed), fall back to uncompressed
  // This allows both `pio run -t upload` (auto-gzip) and `pio run -t uploadfs` (raw) to work
  auto serveHtml = [](AsyncWebServerRequest* request, const char* path) {
    String gzPath = String(path) + ".gz";
    if (LittleFS.exists(gzPath)) {
      AsyncWebServerResponse* resp = request->beginResponse(LittleFS, gzPath, "text/html");
      resp->addHeader("Content-Encoding", "gzip");
      request->send(resp);
    } else {
      request->send(LittleFS, path, "text/html");
    }
  };
  server.on("/tools", HTTP_GET, [serveHtml](AsyncWebServerRequest* request) {
    serveHtml(request, "/tools.html");
  });
  server.on("/", HTTP_GET, [serveHtml](AsyncWebServerRequest* request) {
    serveHtml(request, "/index.html");
  });

  // All unmatched routes → 404 (no LittleFS lookup, no VFS error logs)
  server.onNotFound([](AsyncWebServerRequest* request) {
    request->send(404, "text/plain", "Not Found");
  });

  server.begin();

  LOG("HTTP", "HTTP服务器已启动");
}
