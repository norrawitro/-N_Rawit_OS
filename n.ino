ได้ครับ ผมจะวางโค้ดทั้งหมดให้คุณก๊อปปี้ไปใส่ได้เลย (แนะนำให้สร้างโฟลเดอร์ชื่อ `N_Rawit_Box_Arduino` แล้วสร้างไฟล์ตามชื่อด้านล่าง)

## 1) `N_Rawit_Box_Arduino.ino`
```cpp
/*
  N_Rawit_Box (MVP)
  - ESP32-S3 (Arduino)
  - WiFi AP + Web Server (GUI + API)
  - DataBox storage on SD (2,048 bytes per box)
  - ESP-NOW skeleton (for future mesh metadata exchange)
*/

#include <WiFi.h>
#include <WebServer.h>
#include <SD_MMC.h>
#include <FS.h>

#include "databox.h"
#include "web_pages.h"
#include "mesh_espnow.h"

// -----------------------------
// Config
// -----------------------------
static const char* AP_SSID = "N_Rawit_Box";
static const char* AP_PASS = "12345678";  // อย่างน้อย 8 ตัว
static const uint8_t WIFI_CHANNEL = 1;    // AP channel (ผูกกับ ESP-NOW ด้วย)

static const char* ADMIN_USER = "admin";
static const char* ADMIN_PASS = "admin";

// โฟลเดอร์บน SD สำหรับเก็บ DataBox
static const char* DATABOX_DIR = "/databox";

WebServer server(80);

static bool ensureAuth() {
  if (server.authenticate(ADMIN_USER, ADMIN_PASS)) return true;
  server.requestAuthentication();
  return false;
}

static void handleRoot() {
  if (!ensureAuth()) return;
  server.send(200, "text/html; charset=utf-8", WEB_INDEX_HTML);
}

static void handleApiList() {
  if (!ensureAuth()) return;
  String json = listDataBoxesAsJson(SD_MMC, DATABOX_DIR);
  server.send(200, "application/json; charset=utf-8", json);
}

static void handleApiGet() {
  if (!ensureAuth()) return;

  String id = server.arg("id");
  if (id.isEmpty()) {
    server.send(400, "application/json; charset=utf-8", "{\"error\":\"missing id\"}");
    return;
  }

  DataBoxMeta meta;
  if (!findDataBoxById(SD_MMC, DATABOX_DIR, id.c_str(), meta)) {
    server.send(404, "application/json; charset=utf-8", "{\"error\":\"not found\"}");
    return;
  }

  String json = metaToJson(meta);
  server.send(200, "application/json; charset=utf-8", json);
}

static void handleApiDownload() {
  if (!ensureAuth()) return;

  String id = server.arg("id");
  if (id.isEmpty()) {
    server.send(400, "application/json; charset=utf-8", "{\"error\":\"missing id\"}");
    return;
  }

  DataBoxMeta meta;
  if (!findDataBoxById(SD_MMC, DATABOX_DIR, id.c_str(), meta)) {
    server.send(404, "application/json; charset=utf-8", "{\"error\":\"not found\"}");
    return;
  }

  File f = SD_MMC.open(meta.filePath.c_str(), FILE_READ);
  if (!f) {
    server.send(500, "application/json; charset=utf-8", "{\"error\":\"failed to open file\"}");
    return;
  }

  server.streamFile(f, "application/octet-stream");
  f.close();
}

static void handleApiCreate() {
  if (!ensureAuth()) return;

  uint16_t type = (uint16_t)server.arg("type").toInt();
  uint32_t price = (uint32_t)server.arg("price").toInt();
  uint8_t visibility = (uint8_t)server.arg("visibility").toInt();
  String text = server.arg("text");

  String id = server.arg("id");
  if (id.isEmpty()) {
    id = String("DB-") + String((uint32_t)millis(), HEX);
  }

  String path = String(DATABOX_DIR) + "/" + id + ".dbx";

  DataBox box;
  databoxInit(box);
  databoxSetId(box, id.c_str());
  databoxSetOwner(box, "owner"); // TODO: ผูกกับระบบผู้ใช้จริง
  box.header.type = type;
  box.header.ver = 1;
  box.header.price = price;
  box.header.visibility = visibility;
  databoxSetUnixTime(box, (uint64_t)(time(nullptr))); // ถ้าไม่มี SNTP อาจไม่ตรง

  databoxSetPayloadText(box, text.c_str());

  // TODO: ใส่ hash/sign/chain ใน box.security เมื่อทำ Security จริง

  if (!writeDataBoxToFile(SD_MMC, path.c_str(), box)) {
    server.send(500, "application/json; charset=utf-8", "{\"error\":\"write failed\"}");
    return;
  }

  // (โครง) ประกาศ metadata ไป mesh (ESP-NOW) ในอนาคต
  meshBroadcastMetaFromBox(box);

  server.send(200, "application/json; charset=utf-8", "{\"ok\":true}");
}

static void handleNotFound() {
  server.send(404, "text/plain; charset=utf-8", "Not found");
}

static bool initSd() {
  // บางบอร์ดต้องใช้ 1-bit mode: begin("/sdcard", true)
  if (!SD_MMC.begin("/sdcard", true /*1-bit*/)) {
    return false;
  }
  if (SD_MMC.cardType() == CARD_NONE) return false;

  if (!SD_MMC.exists(DATABOX_DIR)) {
    SD_MMC.mkdir(DATABOX_DIR);
  }
  return true;
}

static void initWiFiAp() {
  // ใช้ AP+STA เพื่อให้ ESP-NOW ทำงานผ่าน STA interface ได้ง่ายขึ้น
  WiFi.mode(WIFI_AP_STA);

  WiFi.softAP(AP_SSID, AP_PASS, WIFI_CHANNEL /*channel*/, 0 /*hidden*/, 4 /*max conn*/);

  IPAddress ip = WiFi.softAPIP();
  Serial.print("AP IP: ");
  Serial.println(ip);
}

static void initWebServer() {
  server.on("/", HTTP_GET, handleRoot);
  server.on("/api/databox/list", HTTP_GET, handleApiList);
  server.on("/api/databox/get", HTTP_GET, handleApiGet);
  server.on("/api/databox/download", HTTP_GET, handleApiDownload);
  server.on("/api/databox/create", HTTP_POST, handleApiCreate);
  server.onNotFound(handleNotFound);
  server.begin();
}

void setup() {
  Serial.begin(115200);
  delay(300);

  initWiFiAp();

  if (!initSd()) {
    Serial.println("SD init failed (SD_MMC).");
  } else {
    Serial.println("SD init OK.");
  }

  // ESP-NOW (โครง)
  meshInitEspNow(WIFI_CHANNEL);

  initWebServer();
  Serial.println("Server started.");
}

void loop() {
  server.handleClient();
  meshLoop();
}
```

## 2) `databox.h`
```cpp
#pragma once

#include <Arduino.h>
#include <FS.h>

static constexpr size_t DATABOX_TOTAL_SIZE = 2048;
static constexpr size_t DATABOX_HEADER_SIZE = 256;
static constexpr size_t DATABOX_SECURITY_SIZE = 256;
static constexpr size_t DATABOX_PAYLOAD_SIZE = 1536;

#pragma pack(push, 1)

struct DataBoxHeader {
  char magic[4];        // "NRWT"
  char id[16];          // id
  char owner[8];        // owner
  uint16_t type;        // type
  uint64_t time;        // unix time
  uint16_t ver;         // version
  uint32_t price;       // token price
  char link[64];        // link/path
  uint8_t gps[16];      // placeholder
  uint8_t visibility;   // 0=Public,1=Private,2=Followers
  uint8_t pad[132 - 1]; // รวมให้ครบ 256
};
static_assert(sizeof(DataBoxHeader) == DATABOX_HEADER_SIZE, "DataBoxHeader must be 256 bytes");

struct DataBoxSecurity {
  uint8_t hash[32];
  uint8_t sign[64];
  uint8_t cableId[8];
  uint8_t mask[4];
  uint8_t chain[32];
  uint8_t pad[116];
};
static_assert(sizeof(DataBoxSecurity) == DATABOX_SECURITY_SIZE, "DataBoxSecurity must be 256 bytes");

struct DataBox {
  DataBoxHeader header;
  DataBoxSecurity security;
  uint8_t payload[DATABOX_PAYLOAD_SIZE];
};
static_assert(sizeof(DataBox) == DATABOX_TOTAL_SIZE, "DataBox must be 2048 bytes");

#pragma pack(pop)

struct DataBoxMeta {
  String id;
  uint16_t type = 0;
  uint32_t price = 0;
  uint64_t time = 0;
  uint8_t visibility = 0;
  String filePath;
};

void databoxInit(DataBox& box);
void databoxSetId(DataBox& box, const char* id);
void databoxSetOwner(DataBox& box, const char* owner);
void databoxSetUnixTime(DataBox& box, uint64_t unixTime);
void databoxSetPayloadText(DataBox& box, const char* textUtf8);

bool readDataBoxFromFile(fs::FS& fs, const char* path, DataBox& out);
bool writeDataBoxToFile(fs::FS& fs, const char* path, const DataBox& box);

bool parseMetaFromBox(const DataBox& box, const String& filePath, DataBoxMeta& out);
String metaToJson(const DataBoxMeta& meta);
String listDataBoxesAsJson(fs::FS& fs, const char* dirPath);
bool findDataBoxById(fs::FS& fs, const char* dirPath, const char* id, DataBoxMeta& out);
```

## 3) `databox.cpp`
```cpp
#include "databox.h"

static void safeCopy(char* dst, size_t dstSize, const char* src) {
  if (!dst || dstSize == 0) return;
  memset(dst, 0, dstSize);
  if (!src) return;
  strncpy(dst, src, dstSize - 1);
}

void databoxInit(DataBox& box) {
  memset(&box, 0, sizeof(DataBox));
  box.header.magic[0] = 'N';
  box.header.magic[1] = 'R';
  box.header.magic[2] = 'W';
  box.header.magic[3] = 'T';
  box.header.ver = 1;
  box.header.visibility = 0;
}

void databoxSetId(DataBox& box, const char* id) {
  safeCopy(box.header.id, sizeof(box.header.id), id);
}

void databoxSetOwner(DataBox& box, const char* owner) {
  safeCopy(box.header.owner, sizeof(box.header.owner), owner);
}

void databoxSetUnixTime(DataBox& box, uint64_t unixTime) {
  box.header.time = unixTime;
}

void databoxSetPayloadText(DataBox& box, const char* textUtf8) {
  memset(box.payload, 0, sizeof(box.payload));
  if (!textUtf8) return;
  size_t n = strnlen(textUtf8, DATABOX_PAYLOAD_SIZE - 1);
  memcpy(box.payload, textUtf8, n);
}

bool readDataBoxFromFile(fs::FS& fs, const char* path, DataBox& out) {
  File f = fs.open(path, FILE_READ);
  if (!f) return false;
  if (f.size() != (int)sizeof(DataBox)) {
    f.close();
    return false;
  }
  size_t r = f.readBytes((char*)&out, sizeof(DataBox));
  f.close();
  if (r != sizeof(DataBox)) return false;
  if (memcmp(out.header.magic, "NRWT", 4) != 0) return false;
  return true;
}

bool writeDataBoxToFile(fs::FS& fs, const char* path, const DataBox& box) {
  File f = fs.open(path, FILE_WRITE);
  if (!f) return false;
  size_t w = f.write((const uint8_t*)&box, sizeof(DataBox));
  f.flush();
  f.close();
  return w == sizeof(DataBox);
}

bool parseMetaFromBox(const DataBox& box, const String& filePath, DataBoxMeta& out) {
  if (memcmp(box.header.magic, "NRWT", 4) != 0) return false;
  out.id = String(box.header.id);
  out.type = box.header.type;
  out.price = box.header.price;
  out.time = box.header.time;
  out.visibility = box.header.visibility;
  out.filePath = filePath;
  return true;
}

static String jsonEscape(const String& s) {
  String o;
  o.reserve(s.length() + 8);
  for (size_t i = 0; i < s.length(); i++) {
    char c = s[i];
    switch (c) {
      case '\\': o += "\\\\"; break;
      case '"': o += "\\\""; break;
      case '\n': o += "\\n"; break;
      case '\r': o += "\\r"; break;
      case '\t': o += "\\t"; break;
      default: o += c; break;
    }
  }
  return o;
}

String metaToJson(const DataBoxMeta& meta) {
  String j = "{";
  j += "\"id\":\"" + jsonEscape(meta.id) + "\"";
  j += ",\"type\":" + String(meta.type);
  j += ",\"price\":" + String(meta.price);
  j += ",\"time\":" + String((uint32_t)meta.time);
  j += ",\"visibility\":" + String(meta.visibility);
  j += ",\"filePath\":\"" + jsonEscape(meta.filePath) + "\"";
  j += "}";
  return j;
}

String listDataBoxesAsJson(fs::FS& fs, const char* dirPath) {
  File dir = fs.open(dirPath);
  if (!dir || !dir.isDirectory()) {
    return "{\"error\":\"databox dir not found\"}";
  }

  String out = "[";
  bool first = true;

  File f = dir.openNextFile();
  while (f) {
    if (!f.isDirectory()) {
      String name = String(f.name());
      if (f.size() == (int)sizeof(DataBox) && name.endsWith(".dbx")) {
        DataBox box;
        if (readDataBoxFromFile(fs, f.name(), box)) {
          DataBoxMeta meta;
          if (parseMetaFromBox(box, String(f.name()), meta)) {
            if (!first) out += ",";
            first = false;
            out += metaToJson(meta);
          }
        }
      }
    }
    f = dir.openNextFile();
  }

  out += "]";
  return out;
}

bool findDataBoxById(fs::FS& fs, const char* dirPath, const char* id, DataBoxMeta& out) {
  File dir = fs.open(dirPath);
  if (!dir || !dir.isDirectory()) return false;

  File f = dir.openNextFile();
  while (f) {
    if (!f.isDirectory()) {
      String name = String(f.name());
      if (f.size() == (int)sizeof(DataBox) && name.endsWith(".dbx")) {
        DataBox box;
        if (readDataBoxFromFile(fs, f.name(), box)) {
          DataBoxMeta meta;
          if (parseMetaFromBox(box, String(f.name()), meta)) {
            if (meta.id == String(id)) {
              out = meta;
              return true;
            }
          }
        }
      }
    }
    f = dir.openNextFile();
  }
  return false;
}
```

## 4) `web_pages.h`
```cpp
#pragma once

static const char WEB_INDEX_HTML[] PROGMEM = R"HTML(
<!doctype html>
<html lang="th">
<head>
  <meta charset="utf-8" />
  <meta name="viewport" content="width=device-width,initial-scale=1" />
  <title>N_Rawit Box (MVP)</title>
  <style>
    body { font-family: system-ui, -apple-system, Segoe UI, Roboto, sans-serif; margin: 16px; }
    .row { display: flex; gap: 12px; flex-wrap: wrap; }
    .card { border: 1px solid #ddd; border-radius: 10px; padding: 12px; max-width: 820px; }
    input, select, textarea, button { padding: 8px; font-size: 14px; }
    textarea { width: 100%; min-height: 80px; }
    table { border-collapse: collapse; width: 100%; }
    th, td { border-bottom: 1px solid #eee; text-align: left; padding: 8px; }
    .muted { color: #666; font-size: 13px; }
  </style>
</head>
<body>
  <h2>N_Rawit Box (MVP)</h2>
  <p class="muted">Web GUI ตัวอย่างสำหรับสร้าง/ดู DataBox บน SD Card</p>

  <div class="row">
    <div class="card" style="flex: 1 1 420px;">
      <h3>สร้าง DataBox</h3>
      <div class="row">
        <div>
          <label>ID (ไม่ใส่ได้)</label><br/>
          <input id="id" placeholder="DB-xxxx" />
        </div>
        <div>
          <label>Type</label><br/>
          <input id="type" type="number" value="1" />
        </div>
        <div>
          <label>Price</label><br/>
          <input id="price" type="number" value="0" />
        </div>
        <div>
          <label>Visibility</label><br/>
          <select id="vis">
            <option value="0">Public</option>
            <option value="1">Private</option>
            <option value="2">Followers</option>
          </select>
        </div>
      </div>
      <div style="margin-top:10px;">
        <label>Text payload</label><br/>
        <textarea id="text" placeholder="พิมพ์ข้อความ..."></textarea>
      </div>
      <div style="margin-top:10px;">
        <button onclick="createBox()">Create</button>
        <span id="createStatus" class="muted"></span>
      </div>
    </div>

    <div class="card" style="flex: 1 1 360px;">
      <h3>รายการ DataBox</h3>
      <button onclick="refresh()">Refresh</button>
      <div id="status" class="muted"></div>
      <div style="margin-top:10px; overflow:auto;">
        <table>
          <thead><tr><th>ID</th><th>Type</th><th>Price</th><th>Actions</th></tr></thead>
          <tbody id="rows"></tbody>
        </table>
      </div>
    </div>
  </div>

<script>
async function refresh() {
  const status = document.getElementById('status');
  status.textContent = 'กำลังโหลด...';
  const res = await fetch('/api/databox/list');
  const data = await res.json();
  const rows = document.getElementById('rows');
  rows.innerHTML = '';
  if (!Array.isArray(data)) {
    status.textContent = 'โหลดไม่สำเร็จ: ' + JSON.stringify(data);
    return;
  }
  status.textContent = 'ทั้งหมด: ' + data.length + ' กล่อง';
  for (const item of data) {
    const tr = document.createElement('tr');
    tr.innerHTML = `
      <td>${item.id}</td>
      <td>${item.type}</td>
      <td>${item.price}</td>
      <td><a href="/api/databox/download?id=${encodeURIComponent(item.id)}">download</a></td>
    `;
    rows.appendChild(tr);
  }
}

async function createBox() {
  const s = document.getElementById('createStatus');
  s.textContent = 'กำลังสร้าง...';
  const form = new URLSearchParams();
  const id = document.getElementById('id').value.trim();
  if (id) form.set('id', id);
  form.set('type', document.getElementById('type').value);
  form.set('price', document.getElementById('price').value);
  form.set('visibility', document.getElementById('vis').value);
  form.set('text', document.getElementById('text').value);

  const res = await fetch('/api/databox/create', {
    method: 'POST',
    headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
    body: form.toString()
  });
  const data = await res.json().catch(() => ({}));
  if (res.ok) {
    s.textContent = 'สำเร็จ';
    await refresh();
  } else {
    s.textContent = 'ไม่สำเร็จ: ' + JSON.stringify(data);
  }
}

refresh();
</script>
</body>
</html>
)HTML";
```

## 5) `mesh_espnow.h`
```cpp
#pragma once

#include <Arduino.h>
#include <esp_now.h>
#include <WiFi.h>
#include "databox.h"

struct MeshMetaMsg {
  char magic[4];   // "NRWM"
  char id[16];     // DataBox id
  uint16_t type;
  uint32_t price;
  uint8_t visibility;
  uint8_t reserved[5];
};
static_assert(sizeof(MeshMetaMsg) <= 250, "ESP-NOW payload should be <= 250 bytes");

bool meshInitEspNow(uint8_t wifiChannel);
void meshLoop();
void meshBroadcastMetaFromBox(const DataBox& box);
```

## 6) `mesh_espnow.cpp`
```cpp
#include "mesh_espnow.h"

static uint32_t lastBeaconMs = 0;

static void onEspNowRecv(const esp_now_recv_info_t* info, const uint8_t* data, int len) {
  if (!data || len <= 0) return;
  if (len < (int)sizeof(MeshMetaMsg)) return;

  MeshMetaMsg msg;
  memcpy(&msg, data, sizeof(MeshMetaMsg));
  if (memcmp(msg.magic, "NRWM", 4) != 0) return;

  Serial.print("[ESP-NOW] recv meta id=");
  Serial.print(String(msg.id));
  Serial.print(" type=");
  Serial.print(msg.type);
  Serial.print(" price=");
  Serial.println(msg.price);

  // TODO: ทำ dedup / เก็บ cache metadata / routing
}

bool meshInitEspNow(uint8_t wifiChannel) {
  (void)wifiChannel;

  if (esp_now_init() != ESP_OK) {
    Serial.println("[ESP-NOW] init failed");
    return false;
  }

  esp_now_register_recv_cb(onEspNowRecv);

  // broadcast peer
  esp_now_peer_info_t peerInfo = {};
  for (int i = 0; i < 6; i++) peerInfo.peer_addr[i] = 0xFF;
  peerInfo.channel = 0;   // 0=ใช้ช่องปัจจุบัน
  peerInfo.encrypt = false;

  esp_now_add_peer(&peerInfo);

  Serial.println("[ESP-NOW] init OK");
  return true;
}

void meshBroadcastMetaFromBox(const DataBox& box) {
  MeshMetaMsg msg = {};
  msg.magic[0] = 'N'; msg.magic[1] = 'R'; msg.magic[2] = 'W'; msg.magic[3] = 'M';
  memcpy(msg.id, box.header.id, sizeof(msg.id));
  msg.type = box.header.type;
  msg.price = box.header.price;
  msg.visibility = box.header.visibility;

  uint8_t bcast[6];
  for (int i = 0; i < 6; i++) bcast[i] = 0xFF;

  esp_err_t r = esp_now_send(bcast, (uint8_t*)&msg, sizeof(msg));
  if (r != ESP_OK) {
    Serial.print("[ESP-NOW] send failed: ");
    Serial.println((int)r);
  }
}

void meshLoop() {
  uint32_t now = millis();
  if (now - lastBeaconMs > 15000) {
    lastBeaconMs = now;
    // TODO: beacon สถานะโหนด
  }
}
```

ถ้าคุณบอก “บอร์ดที่ใช้จริง” (เช่น ESP32-S3-CAM รุ่นไหน) และ SD ต่อแบบ 1-bit/4-bit ผมจะปรับโค้ดส่วน `SD_MMC.begin(...)` และการตั้งค่าให้เข้ากับบอร์ดคุณแบบตรงรุ่นได้ทันทีครับ

ได้ครับ ผมจะวางโค้ดทั้งหมดให้คุณก๊อปปี้ไปใส่ได้เลย (แนะนำให้สร้างโฟลเดอร์ชื่อ `N_Rawit_Box_Arduino` แล้วสร้างไฟล์ตามชื่อด้านล่าง)

## 1) `N_Rawit_Box_Arduino.ino`
```cpp
/*
  N_Rawit_Box (MVP)
  - ESP32-S3 (Arduino)
  - WiFi AP + Web Server (GUI + API)
  - DataBox storage on SD (2,048 bytes per box)
  - ESP-NOW skeleton (for future mesh metadata exchange)
*/

#include <WiFi.h>
#include <WebServer.h>
#include <SD_MMC.h>
#include <FS.h>

#include "databox.h"
#include "web_pages.h"
#include "mesh_espnow.h"

// -----------------------------
// Config
// -----------------------------
static const char* AP_SSID = "N_Rawit_Box";
static const char* AP_PASS = "12345678";  // อย่างน้อย 8 ตัว
static const uint8_t WIFI_CHANNEL = 1;    // AP channel (ผูกกับ ESP-NOW ด้วย)

static const char* ADMIN_USER = "admin";
static const char* ADMIN_PASS = "admin";

// โฟลเดอร์บน SD สำหรับเก็บ DataBox
static const char* DATABOX_DIR = "/databox";

WebServer server(80);

static bool ensureAuth() {
  if (server.authenticate(ADMIN_USER, ADMIN_PASS)) return true;
  server.requestAuthentication();
  return false;
}

static void handleRoot() {
  if (!ensureAuth()) return;
  server.send(200, "text/html; charset=utf-8", WEB_INDEX_HTML);
}

static void handleApiList() {
  if (!ensureAuth()) return;
  String json = listDataBoxesAsJson(SD_MMC, DATABOX_DIR);
  server.send(200, "application/json; charset=utf-8", json);
}

static void handleApiGet() {
  if (!ensureAuth()) return;

  String id = server.arg("id");
  if (id.isEmpty()) {
    server.send(400, "application/json; charset=utf-8", "{\"error\":\"missing id\"}");
    return;
  }

  DataBoxMeta meta;
  if (!findDataBoxById(SD_MMC, DATABOX_DIR, id.c_str(), meta)) {
    s