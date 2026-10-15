
/*
  AMB82-MINI - WiFi Manager + RTSP + Live Status
  Realtek AmebaPro2 Arduino SDK 4.1.0
  FEATURES:
  - AP + STA concurrent WiFi
  - Default WiFi connection
  - Browser WiFi configuration
  - Multiple saved networks
  - New network gets priority 1
  - FlashMemory persistent credentials
  - Status and provisioning on http://192.168.1.1/ only
  - Requested vs actual connected SSID
  - Connection failure and fallback reporting
  - Automatic WiFi reconnection
  - H264 RTSP streaming
  MAIN WIFI:
    SSID: Ram
    Password: qwertyuiop
  CAMERA AP:
    SSID: seaport cam3
    Password: ChangeMe2026!
  AP IP: Printed in Serial Monitor
  HTTP: port 80
  RTSP: port 554
  IMPORTANT:
  - AP and STA must share the same WiFi channel.
  - Initial AP channel 153 matches previous router log.
  - STA association/DHCP run in a background task; HTTP uses cached status.
  - Change AP password before production use.
  - Config uses one sector at FLASH_MEMORY_APP_BASE (0xFD0000).
  - Single-sector saves are read-back checked, but not power-loss atomic.
*/
#include <Arduino.h>
#include <WiFi.h>
#include <FlashMemory.h>
#include "StreamIO.h"
#include "VideoStream.h"
#include "RTSP.h"
#include <stddef.h>
#include <string.h>
#include <stdlib.h>
#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include "platform_stdlib.h"
extern "C" {
#include "lwip/sockets.h"
#include "module_rtsp2.h"
#include "rtsp_drv.h"
#include "sys_api.h"
}
#undef read
#undef write
#include <errno.h>
#include <ctype.h>
// ===================================================
// 1. USER CONFIGURATION
// ===================================================
char DEFAULT_SSID[] = "Ram";
char DEFAULT_PASS[] = "qwertyuiop";
char RAM_SSID[] = "Ram";
char RAM_PASS[] = "qwertyuiop";
char AP_SSID[33] = "seaport cam3";
char AP_PASS[65] = "ChangeMe2026!";
// From your earlier log: main router channel 153.
// Change this if the router channel changes.
char AP_CHANNEL[4] = "153";
// Official AmebaPro2 FlashMemory application region.
// Confirm availability for your exact board/firmware.
#define FLASH_ADDR FLASH_MEMORY_APP_BASE
#define FLASH_SIZE 4096
#define MAX_NETWORKS 3
#define WIFI_TIMEOUT_MS 10000UL
#define RETRY_INTERVAL_MS 15000UL
#define CONFIG_MAGIC 0x53435033UL
#define CONFIG_VERSION 5UL
#define VIDEO_CHANNEL 0

// Device settings have their own reserved flash sector. Profiles remain compatible.
struct DeviceSettings {
  uint32_t magic, version;
  uint8_t dhcp, ip[4], gateway[4], mask[4], dns[4];
  uint16_t httpPort, rtspPort;
  char hostname[33], apSSID[33], apPassword[65];
  uint8_t apChannel, resolution, fps;
  uint32_t bitrate, retryMs, settleMs;
  uint32_t checksum;
};
DeviceSettings device, runningDevice;
bool settingsPending = false;
unsigned long rebootAt = 0;
String settingsError = "";
struct NetworkConfig;
class HttpClient;
uint32_t settingsHash(const DeviceSettings &d);
bool validateSettings(const DeviceSettings &d);
bool saveDeviceSettings();
void defaultSettings() {
  memset(&device, 0, sizeof(device));
  device.magic = 0x414D4433; device.version = 1; device.dhcp = 1;
  uint8_t ip[4] = {10,10,10,8}, gw[4] = {10,10,10,1}, mask[4] = {255,255,255,0};
  memcpy(device.ip, ip, 4); memcpy(device.gateway, gw, 4);
  memcpy(device.mask, mask, 4); memcpy(device.dns, gw, 4);
  device.httpPort = 80; device.rtspPort = 554;
  strcpy(device.hostname, "seaport-cam3"); strcpy(device.apSSID, "seaport cam3");
  strcpy(device.apPassword, "ChangeMe2026!"); device.apChannel = 153;
  device.resolution = VIDEO_HD; device.fps = CAM_FPS;
  device.bitrate = 2 * 1024 * 1024; device.retryMs = 15000; device.settleMs = 10000;
}
uint32_t settingsHash(const DeviceSettings &d) {
  const uint8_t *p = reinterpret_cast<const uint8_t *>(&d);
  uint32_t hash = 2166136261UL;
  for (size_t i=0; i<offsetof(DeviceSettings, checksum); ++i) { hash ^= p[i]; hash *= 16777619UL; }
  return hash;
}
bool validChannel(unsigned int n) {
  const uint8_t list[] = {1,2,3,4,5,6,7,8,9,10,11,12,13,14,36,40,44,48,52,56,60,64,100,104,108,112,116,120,124,128,132,136,140,144,149,153,157,161,165};
  for (size_t i=0;i<sizeof(list);++i) if (n==list[i]) return true;
  return false;
}
uint32_t ipv4Value(const uint8_t *p) { return (uint32_t(p[0])<<24)|(uint32_t(p[1])<<16)|(uint32_t(p[2])<<8)|p[3]; }
bool validateSettings(const DeviceSettings &d) {
  if (d.dhcp>1 || !d.httpPort || !d.rtspPort || d.httpPort==d.rtspPort || d.rtspPort==80) return false;
  if (!memchr(d.hostname,0,sizeof(d.hostname)) || !d.hostname[0] ||
      !memchr(d.apSSID,0,sizeof(d.apSSID)) || !d.apSSID[0] ||
      !memchr(d.apPassword,0,sizeof(d.apPassword)) || strlen(d.apPassword)<8 || strlen(d.apPassword)>63) return false;
  for (size_t i=0;i<strlen(d.hostname);++i) {
    char c=d.hostname[i]; if (!(isalnum(static_cast<unsigned char>(c)) || c=='-')) return false;
  }
  if (d.hostname[0]=='-' || d.hostname[strlen(d.hostname)-1]=='-') return false;
  if (!validChannel(d.apChannel) || d.fps<1 || d.fps>30 || d.bitrate<128000 || d.bitrate>8000000 ||
      d.retryMs<5000 || d.retryMs>300000 || d.settleMs<1000 || d.settleMs>60000) return false;
  if (d.resolution!=VIDEO_VGA && d.resolution!=VIDEO_HD && d.resolution!=VIDEO_FHD) return false;
  if (!d.dhcp) {
    uint32_t ip=ipv4Value(d.ip), gw=ipv4Value(d.gateway), mask=ipv4Value(d.mask), host=~mask;
    if (!mask || mask==0xFFFFFFFFUL || (host & (host+1)) || host<3 ||
        !d.ip[0] || d.ip[0]==127 || d.ip[0]>=224 || !d.gateway[0] || d.gateway[0]>=224 ||
        (ip & mask)!=(gw & mask) || ip==gw || (ip & host)==0 || (ip & host)==host ||
        (gw & host)==0 || (gw & host)==host || !ipv4Value(d.dns) || d.dns[0]>=224) return false;
    // Keep the recovery AP subnet separate from the station subnet.
    uint32_t network=ip & mask, broadcast=network | host;
    if (network<=0xC0A801FFUL && broadcast>=0xC0A80100UL) return false;
  }
  return true;
}
bool saveDeviceSettings() {
  device.magic=0x414D4433; device.version=1; device.checksum=settingsHash(device);
  FlashMemory.begin(FLASH_MEMORY_APP_BASE+FLASH_SIZE, FLASH_SIZE);
  if (!FlashMemory.buf) { FlashMemory.end(); return false; }
  FlashMemory.eraseSector(0);
  const uint8_t *bytes=reinterpret_cast<const uint8_t *>(&device);
  for (size_t i=0;i<sizeof(device);i+=4) {
    uint32_t word=0xFFFFFFFFUL; size_t n=sizeof(device)-i; if(n>4)n=4;
    memcpy(&word,bytes+i,n); FlashMemory.writeWord(i,word);
  }
  memset(FlashMemory.buf,0xFF,FLASH_SIZE); FlashMemory.read(0);
  bool ok=memcmp(FlashMemory.buf,&device,sizeof(device))==0;
  FlashMemory.end(); return ok;
}
void loadDeviceSettings() {
  defaultSettings();
  DeviceSettings saved = {};
  FlashMemory.begin(FLASH_MEMORY_APP_BASE+FLASH_SIZE,FLASH_SIZE);
  if(FlashMemory.buf) { memset(FlashMemory.buf,0xFF,FLASH_SIZE); FlashMemory.read(0); memcpy(&saved,FlashMemory.buf,sizeof(saved)); }
  FlashMemory.end();
  if(saved.magic==0x414D4433 && saved.version==1 && saved.checksum==settingsHash(saved) && validateSettings(saved)) device=saved;
  runningDevice=device;
  strcpy(AP_SSID, runningDevice.apSSID); // Replaced below with correctly sized arrays.
  strcpy(AP_PASS, runningDevice.apPassword);
  snprintf(AP_CHANNEL,sizeof(AP_CHANNEL),"%u",runningDevice.apChannel);
}
// ===================================================
// 2. NETWORK CONFIGURATION STRUCTURES
// ===================================================
struct NetworkProfile {
  char ssid[33];
  char password[65];
};
struct NetworkConfig {
  uint32_t magic;
  uint32_t version;
  uint8_t count;
  NetworkProfile profiles[MAX_NETWORKS];
  uint32_t checksum;
};
NetworkConfig config;
bool ramSeedNeeded = true;
int activeNetwork = -1;
bool reconnectRequested = false;
unsigned long applyAfter = 0;
uint32_t connectionGeneration = 0;
bool rtspStarted = false;
int apStartResult = WL_IDLE_STATUS; // Initialization result, not a beacon-health check.
unsigned long lastRetry = 0;
unsigned long lastConnectionAttempt = 0;
// ===================================================
// 3. CONNECTION MONITOR
// ===================================================
String requestedSSID = "";
String connectedSSID = "";
String connectionState = "IDLE";
String lastConnectionError = "";
String connectionIP = "";
int connectionRSSI = 0;
// ===================================================
// 4. CAMERA AND SERVER OBJECTS
// ===================================================
VideoSetting videoConfig(
    VIDEO_HD,
    CAM_FPS,
    VIDEO_H264,
    0
);

class HttpClient : public Stream {
  int fd;
  bool alive;
public:
  explicit HttpClient(int socket) : fd(socket), alive(socket>=0) {}
  int available() override { int n=0; if(!alive)return 0; if(lwip_ioctl(fd,FIONREAD,&n)<0)return 0; return n; }
  int read() override {
    uint8_t b; int n=lwip_recv(fd,&b,1,MSG_DONTWAIT);
    if(n==1)return b;
    if(n==0 || (n<0 && errno!=EAGAIN && errno!=EWOULDBLOCK && errno!=EINTR))alive=false;
    delay(1); return -1;
  }
  int peek() override { uint8_t b; int n=lwip_recv(fd,&b,1,MSG_PEEK|MSG_DONTWAIT); return n==1?b:-1; }
  void flush() override {}
  size_t write(uint8_t b) override { return write(&b,1); }
  size_t write(const uint8_t *data,size_t length) override {
    int n=lwip_send(fd,data,length,MSG_DONTWAIT);
    if(n>0)return n;
    if(n==0 || (errno!=EAGAIN && errno!=EWOULDBLOCK && errno!=EINTR))alive=false;
    return 0;
  }
  using Print::write;
  bool connected() { return alive; }
  explicit operator bool() const { return fd>=0; }
  void stop() { if(fd>=0)lwip_close(fd);fd=-1;alive=false; }
};
int recoveryListener=-1, configuredListener=-1;
int listenHttp(uint16_t port) {
  int fd=lwip_socket(AF_INET,SOCK_STREAM,0); if(fd<0)return -1;
  int yes=1; lwip_setsockopt(fd,SOL_SOCKET,SO_REUSEADDR,&yes,sizeof(yes));
  sockaddr_in address={}; address.sin_family=AF_INET; address.sin_port=htons(port); address.sin_addr.s_addr=htonl(INADDR_ANY);
  if(lwip_bind(fd,reinterpret_cast<sockaddr *>(&address),sizeof(address))<0 || lwip_listen(fd,2)<0) { lwip_close(fd);return -1; }
  unsigned long nonblocking=1; if(lwip_ioctl(fd,FIONBIO,&nonblocking)<0) { lwip_close(fd);return -1; }
  return fd;
}
int acceptHttp() {
  int fd=recoveryListener>=0?lwip_accept(recoveryListener,nullptr,nullptr):-1;
  if(fd<0 && configuredListener>=0)fd=lwip_accept(configuredListener,nullptr,nullptr);
  return fd;
}
void beginHttp() {
  recoveryListener=listenHttp(80);
  if(runningDevice.httpPort!=80)configuredListener=listenHttp(runningDevice.httpPort);
  Serial.print("[HTTP] Recovery port 80: ");Serial.println(recoveryListener>=0?"ready":"FAILED");
  if(runningDevice.httpPort!=80) { Serial.print("[HTTP] Configured port: ");Serial.println(configuredListener>=0?"ready":"FAILED"); }
}
class ConfigurableRTSP : public RTSP {
public:
  bool preparePort(uint16_t port) {
    if(!_p_mmf_context)_p_mmf_context=RTSPInit();
    if(!_p_mmf_context)return false;
    rtsp2_control(_p_mmf_context->priv,CMD_RTSP2_SET_PORT,port);
    return getPort()==port;
  }
};

ConfigurableRTSP rtsp;
StreamIO videoStreamer(1, 1);

// ===================================================
// 5. CHECKSUM
// ===================================================
uint32_t configChecksum(
    const NetworkConfig &value
) {
  const uint8_t *data =
      (const uint8_t *)&value;
  size_t length =
      offsetof(NetworkConfig, checksum);
  uint32_t hash = 2166136261UL;
  for (size_t i = 0; i < length; i++) {
    hash ^= data[i];
    hash *= 16777619UL;
  }
  return hash;
}
// ===================================================
// 6. FLASH SAVE
// ===================================================
bool saveConfig() {
  config.magic = CONFIG_MAGIC;
  config.version = CONFIG_VERSION;
  config.checksum = configChecksum(config);
  FlashMemory.begin(
      FLASH_ADDR,
      FLASH_SIZE
  );
  if (!FlashMemory.buf) { FlashMemory.end(); return false; }
  // SDK 4.1.0 write() erases the entire application region, even
  // with a 4 KB buffer. Erase exactly our sector, then write words.
  FlashMemory.eraseSector(0);
  const uint8_t *bytes = reinterpret_cast<const uint8_t *>(&config);
  for (size_t offset = 0; offset < sizeof(config); offset += 4) {
    uint32_t word = 0xFFFFFFFFUL;
    size_t n = sizeof(config) - offset;
    if (n > 4) n = 4;
    memcpy(&word, bytes + offset, n);
    FlashMemory.writeWord(offset, word);
  }
  FlashMemory.read(0);
  bool ok = memcmp(FlashMemory.buf, &config, sizeof(config)) == 0;
  FlashMemory.end();
  Serial.println(ok ? "[FLASH] Configuration verified" : "[FLASH] Read-back failed");
  return ok;
}
// ===================================================
// 7. FLASH LOAD
// ===================================================
void loadConfig() {
  memset(&config, 0, sizeof(config));
  FlashMemory.begin(
      FLASH_ADDR,
      FLASH_SIZE
  );
  if (FlashMemory.buf) {
    memset(FlashMemory.buf, 0xFF, FLASH_SIZE);
    FlashMemory.read(0);
  }
  if (FlashMemory.buf) memcpy(
      &config,
      FlashMemory.buf,
      sizeof(config)
  );
  FlashMemory.end();
  bool valid =
      config.magic == CONFIG_MAGIC &&
      (config.version == CONFIG_VERSION || config.version == 4UL) &&
      config.count <= MAX_NETWORKS &&
      config.checksum == configChecksum(config);
  if (valid) {
    for (int i = 0; i < config.count; ++i) {
      if (!memchr(config.profiles[i].ssid, 0, 33) ||
          !memchr(config.profiles[i].password, 0, 65) ||
          !config.profiles[i].ssid[0] ||
          (config.profiles[i].password[0] && strlen(config.profiles[i].password) < 8) ||
          strlen(config.profiles[i].password) > 63) valid = false;
    }
  }
  ramSeedNeeded = !valid || config.version == 4UL;
  if (!valid) {
    Serial.println(
        "[FLASH] Initializing default WiFi"
    );
    memset(&config, 0, sizeof(config));
    config.magic = CONFIG_MAGIC;
    config.version = CONFIG_VERSION;
    config.count = 1;
    strncpy(
        config.profiles[0].ssid,
        DEFAULT_SSID,
        sizeof(config.profiles[0].ssid) - 1
    );
    strncpy(
        config.profiles[0].password,
        DEFAULT_PASS,
        sizeof(config.profiles[0].password) - 1
    );
    saveConfig();
  } else {
    Serial.println("[FLASH] Saved profiles loaded");
    for (int i = 0; i < config.count; i++) {
      config.profiles[i].ssid[32] = '\0';
      config.profiles[i].password[64] = '\0';
    }
  }
  if (config.count > 0) {
    requestedSSID = String(config.profiles[0].ssid);
  }
  Serial.print("[FLASH] Network count: ");
  Serial.println(config.count);
  for (int i = 0; i < config.count; i++) {
    Serial.print("[FLASH] ");
    Serial.println(config.profiles[i].ssid);
  }
}
// ===================================================
// 8. ADD OR UPDATE NETWORK
// ===================================================
void ensureRamProfile() {
  // One-time migration: later deletion of Ram remains saved across reboots.
  if (!ramSeedNeeded) return;
  int found = -1;
  for (int i = 0; i < config.count; ++i)
    if (strcmp(config.profiles[i].ssid, RAM_SSID) == 0) found = i;
  if (found < 0) {
    int count = config.count < MAX_NETWORKS ? config.count + 1 : MAX_NETWORKS;
    for (int i = count - 1; i > 0; --i) config.profiles[i] = config.profiles[i - 1];
    memset(&config.profiles[0], 0, sizeof(NetworkProfile));
    strncpy(config.profiles[0].ssid, RAM_SSID, sizeof(config.profiles[0].ssid) - 1);
    strncpy(config.profiles[0].password, RAM_PASS, sizeof(config.profiles[0].password) - 1);
    config.count = count;
  } else {
    strncpy(config.profiles[found].password, RAM_PASS, sizeof(config.profiles[found].password) - 1);
  }
  if (!saveConfig()) Serial.println("[FLASH] Ram available for this boot; persistence failed.");
  requestedSSID = config.count ? String(config.profiles[0].ssid) : String("");
}
bool addNetwork(
    String ssid,
    String password
) {
  if (strlen(ssid.c_str()) != ssid.length() || strlen(password.c_str()) != password.length()) return false;
  if (ssid.length() == 0 ||
      ssid.length() > 32) {
    return false;
  }
  if ((password.length() && password.length() < 8) ||
      password.length() > 63) {
    return false;
  }
  NetworkConfig previous = config;
  // New network becomes highest priority.
  NetworkProfile selected;
  memset(&selected, 0, sizeof(selected));
  ssid.toCharArray(
      selected.ssid,
      sizeof(selected.ssid)
  );
  password.toCharArray(
      selected.password,
      sizeof(selected.password)
  );
  NetworkProfile reordered[MAX_NETWORKS];
  memset(reordered, 0, sizeof(reordered));
  reordered[0] = selected;
  int count = 1;
  for (int i = 0; i < config.count; i++) {
    if (ssid == String(config.profiles[i].ssid)) {
      continue;
    }
    if (count >= MAX_NETWORKS) {
      break;
    }
    reordered[count++] = config.profiles[i];
  }
  memcpy(
      config.profiles,
      reordered,
      sizeof(reordered)
  );
  config.count = count;
  if (!saveConfig()) { config = previous; return false; }
  requestedSSID = ssid;
  connectionState = "CONNECTING";
  lastConnectionError = "";
  lastConnectionAttempt = millis();
  reconnectRequested = true;
  applyAfter = millis() + 1200UL;
  Serial.print("[WIFI] Requested SSID: ");
  Serial.println(requestedSSID);
  return true;
}
// ===================================================
// 9. DELETE SAVED NETWORK
// ===================================================
bool connectSavedProfile(int index) {
  if(index<0 || index>=config.count)return false;
  if(index>0) {
    NetworkProfile selected=config.profiles[index];
    return addNetwork(String(selected.ssid),String(selected.password));
  }
  requestedSSID=config.profiles[0].ssid;
  connectionState="CONNECTING";lastConnectionError="";
  lastConnectionAttempt=millis();reconnectRequested=true;
  applyAfter=millis()+1200UL;
  return true;
}
bool removeNetwork(int index) {
  if (index < 0 ||
      index >= config.count) {
    return false;
  }
  NetworkConfig previous = config;
  for (int i = index; i < config.count - 1; i++) {
    config.profiles[i] = config.profiles[i + 1];
  }
  config.count--;
  memset(
      &config.profiles[config.count],
      0,
      sizeof(NetworkProfile)
  );
  if (!saveConfig()) { config = previous; return false; }
  requestedSSID = config.count ? String(config.profiles[0].ssid) : String("");
  connectionState = "CONNECTING";
  lastConnectionError = "";
  reconnectRequested = true;
  applyAfter = millis() + 1200UL;
  return true;
}
// ===================================================
// 10. CONNECTION STATUS HELPERS
// ===================================================
String ipToString(IPAddress ip) {
  char buffer[16];
  snprintf(
      buffer,
      sizeof(buffer),
      "%u.%u.%u.%u",
      (unsigned int)ip[0],
      (unsigned int)ip[1],
      (unsigned int)ip[2],
      (unsigned int)ip[3]
  );
  return String(buffer);
}
// ===================================================
// 11. CONNECT TO SAVED NETWORKS
// ===================================================
// Only this worker calls station WiFi APIs. Queue items contain no Strings.
struct ConnectionJob { uint32_t generation; uint8_t count; NetworkProfile profiles[MAX_NETWORKS]; };
struct ConnectionReport {
  uint32_t generation;
  char state[16], requested[33], actual[33], ip[16], error[128];
  int rssi, index;
  unsigned long attemptedAt;
};
void publishConnection(ConnectionReport &report); // Prevent Arduino's early auto-prototype.
QueueHandle_t connectionJobs = nullptr;
QueueHandle_t connectionReports = nullptr;
#define MAX_SCAN_NETWORKS 16
struct NearbyNetwork { char ssid[33]; int32_t rssi; bool secured; };
struct ScanReport {
  bool scanning, failed;
  int count;
  unsigned long completedAt;
  NearbyNetwork networks[MAX_SCAN_NETWORKS];
};
QueueHandle_t scanRequests=nullptr,scanReports=nullptr;
ScanReport nearby={};
void scanNearby(ScanReport &result);
void scanNearby(ScanReport &result) {
  result.scanning=true;result.failed=false;
  xQueueOverwrite(scanReports,&result);
  int count=WiFi.scanNetworks();
  result.count=0;result.failed=count<0;
  for(int i=0;i<count;++i) {
    const char *ssid=WiFi.SSID(static_cast<uint8_t>(i));
    if(!ssid || !ssid[0] || strcmp(ssid,runningDevice.apSSID)==0)continue;
    int found=-1;for(int j=0;j<result.count;++j)if(strcmp(result.networks[j].ssid,ssid)==0)found=j;
    int32_t signal=WiFi.RSSI(static_cast<uint8_t>(i));
    if(found<0) { if(result.count>=MAX_SCAN_NETWORKS)continue;found=result.count++;memset(&result.networks[found],0,sizeof(NearbyNetwork)); }
    else if(signal<=result.networks[found].rssi)continue;
    strncpy(result.networks[found].ssid,ssid,32);result.networks[found].rssi=signal;
    result.networks[found].secured=WiFi.encryptionType(static_cast<uint8_t>(i))!=ENC_TYPE_NONE;
  }
  for(int i=0;i<result.count;++i)for(int j=i+1;j<result.count;++j)
    if(result.networks[j].rssi>result.networks[i].rssi) { NearbyNetwork temp=result.networks[i];result.networks[i]=result.networks[j];result.networks[j]=temp; }
  result.scanning=false;result.completedAt=millis();
  xQueueOverwrite(scanReports,&result);
}
bool workerReady = false;
bool stationReady = false;

char pageNotice[128] = "";
void publishConnection(ConnectionReport &report) {
  xQueueOverwrite(connectionReports, &report);
}
void connectionWorker(void *) {
  ConnectionJob job = {};
  ConnectionReport report = {};
  bool haveJob = false;
  unsigned long retryAt = 0;
  ScanReport scan={};
  for (;;) {
    uint8_t scanNow=0;
    if(scanRequests && scanReports && !uxQueueMessagesWaiting(connectionJobs) &&
       (xQueueReceive(scanRequests,&scanNow,0)==pdPASS || !scan.completedAt || millis()-scan.completedAt>=60000UL))
      scanNearby(scan); // Same worker owns scanning and connection APIs.
    if (xQueueReceive(connectionJobs, &job, pdMS_TO_TICKS(200)) == pdPASS) {
      haveJob = true;
      retryAt = 0;
    }
    if (!haveJob) continue;
    if (retryAt && (int32_t)(millis() - retryAt) < 0) {
      // Monitor link loss, without competing with HTTP for WiFi API calls.
      if (report.actual[0] && WiFi.status() != WL_CONNECTED) {
        memset(report.actual, 0, sizeof(report.actual));
        memset(report.ip, 0, sizeof(report.ip));
        report.rssi = 0;
        strcpy(report.state, "DISCONNECTED");
        publishConnection(report);
      }
      continue;
    }
    // Keep a working fallback. Profile edits arrive via the job queue.
    if (retryAt && report.actual[0] && WiFi.status() == WL_CONNECTED) {
      report.rssi = WiFi.RSSI();
      IPAddress ip = WiFi.localIP(0);
      snprintf(report.ip, sizeof(report.ip), "%u.%u.%u.%u", ip[0], ip[1], ip[2], ip[3]);
      publishConnection(report);
      retryAt = millis() + 1000UL;
      continue;
    }
    memset(&report, 0, sizeof(report));
    report.generation = job.generation;
    report.index = -1;
    if (job.count) strncpy(report.requested, job.profiles[0].ssid, 32);
    strcpy(report.state, job.count ? "CONNECTING" : "IDLE");
    report.attemptedAt = millis();
    if (!job.count) {
      WiFi.disconnect();
      strcpy(report.error, "No saved WiFi networks. Add one below.");
      publishConnection(report);
      haveJob = false;
      continue;
    }
    bool replaced = false;
    for (int i = 0; i < job.count; ++i) {
      if (uxQueueMessagesWaiting(connectionJobs)) { replaced = true; break; }
      snprintf(report.error, sizeof(report.error), "Trying saved network %d of %d: %s", i + 1, job.count, job.profiles[i].ssid);
      publishConnection(report);
      Serial.print("[STA] Trying: "); Serial.println(job.profiles[i].ssid);
      WiFi.disconnect();
      // Blocking SDK association/DHCP is isolated from the HTTP loop.
      int result = job.profiles[i].password[0] ? WiFi.begin(job.profiles[i].ssid, job.profiles[i].password) : WiFi.begin(job.profiles[i].ssid);
      unsigned long started = millis();
      bool connected = false;
      do {
        if (uxQueueMessagesWaiting(connectionJobs)) { replaced = true; break; }
        IPAddress ip = WiFi.localIP(0);
        const char *actual = WiFi.SSID();
        connected = WiFi.status() == WL_CONNECTED && (ip[0] || ip[1] || ip[2] || ip[3]) && actual && strcmp(actual, job.profiles[i].ssid) == 0;
        if (connected) {
          strncpy(report.actual, actual, 32);
          snprintf(report.ip, sizeof(report.ip), "%u.%u.%u.%u", ip[0], ip[1], ip[2], ip[3]);
          report.rssi = WiFi.RSSI(); report.index = i;
          strcpy(report.state, i == 0 ? "CONNECTED" : "FALLBACK");
          if (i) snprintf(report.error, sizeof(report.error), "Requested WiFi failed. Using saved network: %s", report.actual);
          else report.error[0] = 0;
          publishConnection(report);
          break;
        }
        if (result == WL_CONNECT_FAILED) break;
        vTaskDelay(pdMS_TO_TICKS(200));
      } while (millis() - started < runningDevice.settleMs);
      if (connected || replaced) break;
      Serial.print("[STA] Failed: "); Serial.println(job.profiles[i].ssid);
    }
    if (replaced) continue;
    if (!report.actual[0]) {
      WiFi.disconnect();
      strcpy(report.state, "FAILED");
      strcpy(report.error, "Unable to connect to any saved WiFi network; retrying later.");
      publishConnection(report);
    }
    retryAt = millis() + runningDevice.retryMs;
  }
}
bool hasStationIP() { return stationReady; }
bool connectSavedNetworks() {
  if (!workerReady) {
    connectionState = "FAILED";
    lastConnectionError = "Connection task unavailable; restart the board.";
    return false;
  }
  ConnectionJob job = {};
  job.generation = ++connectionGeneration;
  job.count = config.count;
  memcpy(job.profiles, config.profiles, sizeof(job.profiles));
  stationReady = false;
  connectedSSID = ""; connectionIP = ""; connectionRSSI = 0;
  activeNetwork = -1;
  requestedSSID = config.count ? String(config.profiles[0].ssid) : String("");
  connectionState = config.count ? "CONNECTING" : "IDLE";
  lastConnectionError = "";
  return xQueueOverwrite(connectionJobs, &job) == pdPASS;
}
void serviceConnection() {
  if(scanReports)xQueueReceive(scanReports,&nearby,0);
  ConnectionReport report;
  if (connectionReports && xQueueReceive(connectionReports, &report, 0) == pdPASS) {
    if (reconnectRequested || report.generation != connectionGeneration) return;
    connectionState = report.state; requestedSSID = report.requested;
    connectedSSID = report.actual; connectionIP = report.ip;
    connectionRSSI = report.rssi; activeNetwork = report.index;
    lastConnectionAttempt = report.attemptedAt;
    lastConnectionError = report.error;
    stationReady = report.actual[0] && report.ip[0];
  }
}

// ===================================================
// 12. START CONCURRENT ACCESS POINT
// ===================================================
void startAccessPoint() {
  Serial.println("[AP] Enabling concurrent mode");
  WiFi.enableConcurrent();
  Serial.print("[AP] SSID: ");
  Serial.println(AP_SSID);
  Serial.print("[AP] Channel: ");
  Serial.println(AP_CHANNEL);
  int status = WiFi.apbegin(
      AP_SSID,
      AP_PASS,
      AP_CHANNEL,
      0
  );
  apStartResult = status;
  Serial.print("[AP] Start result: ");
  Serial.println(status);
  delay(10000);
  Serial.print("[AP] IP: ");
  Serial.println(WiFi.localIP(1));
  Serial.print("[AP] Gateway: ");
  Serial.println(WiFi.gatewayIP(1));
  // Initialization status and IP do not guarantee
  // AP remains broadcast after STA changes channel.
}
// ===================================================
// 13. RTSP VIDEO SERVER
// ===================================================
void printRTSP() {
  if (stationReady) {
    Serial.print("[RTSP] rtsp://");
    Serial.print(connectionIP);
    Serial.print(":");
    Serial.println(rtsp.getPort());
  }
}
void startRTSP() {
  if (rtspStarted) {
    return;
  }
  Serial.println("[CAMERA] Starting H264 encoder");
  videoConfig=VideoSetting(runningDevice.resolution,runningDevice.fps,VIDEO_H264,0);
  videoConfig.setBitrate(runningDevice.bitrate);
  Camera.configVideoChannel(
      VIDEO_CHANNEL,
      videoConfig
  );
  Camera.videoInit();
  if(!rtsp.preparePort(runningDevice.rtspPort)) { Serial.println("[RTSP] Port setup failed"); return; }
  rtsp.configVideo(videoConfig);
  rtsp.begin();
  videoStreamer.registerInput(
      Camera.getStream(VIDEO_CHANNEL)
  );
  videoStreamer.registerOutput(rtsp);
  if (videoStreamer.begin() != 0) {
    Serial.println("[RTSP] StreamIO failed");
    return;
  }
  Camera.channelBegin(VIDEO_CHANNEL);
  rtspStarted = true;
  Serial.println("[RTSP] Initialized");
  printRTSP();
}
// ===================================================
// 14. HTML / URL / JSON HELPERS
// ===================================================
String escapeHTML(String value) {
  value.replace("&", "&amp;");
  value.replace("<", "&lt;");
  value.replace(">", "&gt;");
  value.replace("\"", "&quot;");
  value.replace("'", "&#39;");
  return value;
}
String escapeJSON(const String &input) {
  String output = "";
  for (unsigned int i = 0; i < input.length(); i++) {
    char c = input[i];
    if (c == '"' || c == '\\') {
      output += '\\';
      output += c;
    }
    else if ((unsigned char)c < 0x20) {
      output += ' ';
    }
    else {
      output += c;
    }
  }
  return output;
}
String urlDecode(const String &input) {
  String result = "";
  for (unsigned int i = 0; i < input.length(); i++) {
    if (input[i] == '+') {
      result += ' ';
    } else if (
        input[i] == '%' &&
        i + 2 < input.length()
    ) {
      char hex[3] = {
        input[i + 1],
        input[i + 2],
        '\0'
      };
      char *end = nullptr;
      long decoded = strtol(hex, &end, 16);
      if (end != hex + 2 || decoded <= 0 || decoded > 255) return "";
      result += (char)decoded;
      i += 2;
    } else if (input[i] == '%') {
      return "";
    } else {
      result += input[i];
    }
  }
  return result;
}
String formValue(
    const String &body,
    const String &name
) {
  const char *data=body.c_str();size_t length=body.length(),keyLength=name.length();
  for(size_t start=0;start<length;) {
    size_t end=start;while(end<length && data[end]!='&')++end;
    size_t equals=start;while(equals<end && data[equals]!='=')++equals;
    if(equals<end && equals-start==keyLength && memcmp(data+start,name.c_str(),keyLength)==0)
      return urlDecode(body.substring(equals+1,end));
    start=end+1;
  }
  return "";
}
// ===================================================
// 15. SINGLE-PAGE STATUS
// ===================================================
// ===================================================
// 16. CONFIGURATION WEBPAGE
// ===================================================
void field(Print &out,const char *label,const char *name,const String &value,const char *type="text") {
  out.print("<label>");out.print(label);out.print("<input name='");out.print(name);
  out.print("' type='");out.print(type);out.print("' value='");out.print(escapeHTML(value));out.println("'></label>");
}
void renderPage(Print &out,const String &message="") {
  out.print("<!doctype html><html><head><meta name='viewport' content='width=device-width,initial-scale=1'><title>AMB82 Configuration</title><style>body{font:15px Arial;margin:20px;background:#fafafa;color:#223}main{max-width:680px;margin:auto}section{background:white;border:1px solid #ddd;padding:16px;margin:14px 0;border-radius:6px}label{display:grid;grid-template-columns:150px 1fr;align-items:center;gap:12px;margin:10px 0}input,select{padding:8px;border:1px solid #ccc;border-radius:4px;min-width:0}button{padding:9px 16px;margin:6px;background:#1769df;color:white;border:0;border-radius:4px}p{overflow-wrap:anywhere}.notice{background:#eef4ff;padding:12px}@media(max-width:480px){label{grid-template-columns:1fr;gap:4px}}</style></head><body><main><h2>Seaport Camera 3 Configuration</h2>");
  out.print("<div id='actionNotice' role='status' aria-live='polite' style='position:sticky;top:0;z-index:10;background:#fff4cc;border:1px solid #d9b43b;padding:12px;margin-bottom:12px'>Page ready. Click a button to see its progress here.</div>");
  if(message.length()) { out.print("<p id='boardReply' class='notice'>");out.print(escapeHTML(message));out.print("</p>"); }
  out.print("<section><b>Status: ");out.print(escapeHTML(connectionState));
  out.print("</b><p>Requested: ");out.print(escapeHTML(requestedSSID));
  out.print("<br>Connected: ");out.print(escapeHTML(connectedSSID));
  out.print("<br>Camera IP: ");out.print(escapeHTML(connectionIP));
  out.print("<br>Signal: ");out.print(connectionRSSI);out.print(" dBm<br>");out.print(escapeHTML(lastConnectionError));
  if(rtspStarted && stationReady) { out.print("<br>RTSP: rtsp://");out.print(connectionIP);out.print(":");out.print(rtsp.getPort()); }
  out.print("</p><a href='/'>Refresh status</a> &middot; Setup: 192.168.1.1:80</section>");
  out.print("<section><h3>Wi-Fi networks</h3>");
  out.print("<p id='scanState'>");
  if(!workerReady || !scanRequests || !scanReports)out.print("Wi-Fi scan service unavailable.");
  else if(nearby.scanning)out.print("Wi-Fi scan queued or in progress. Refresh status shortly to see results.");
  else if(!nearby.completedAt)out.print("Automatic scan queued. Refresh status shortly to see results.");
  else if(nearby.failed)out.print("Scan failed. Try Scan now.");
  else { out.print("Nearby networks: ");out.print(nearby.count);out.print(". Results age: ");out.print((millis()-nearby.completedAt)/1000);out.print(" seconds."); }
  out.print("</p><label>Nearby router SSID<select id='nearbyNetworks'><option value=''>Choose a nearby network</option>");
  for(int i=0;i<nearby.count;++i) {
    out.print("<option value='");out.print(escapeHTML(String(nearby.networks[i].ssid)));out.print("'>");
    out.print(escapeHTML(String(nearby.networks[i].ssid)));out.print(" (");out.print(nearby.networks[i].rssi);
    out.print(nearby.networks[i].secured?" dBm, secured)":" dBm, open)");out.print("</option>");
  }
  out.print("</select></label><form method='POST' action='/'><input type='hidden' name='operation' value='scan'><button>Scan now</button></form><p>Scans run automatically every 60 seconds in the background. Refresh status to display the latest list. Hidden SSIDs can be entered manually.</p><form method='POST' action='/'><input type='hidden' name='operation' value='save'>");
  field(out,"SSID","ssid","");field(out,"Password","pass","","password");
  out.print("<label style='display:block'><input type='checkbox' id='showWifiPassword'> Show Wi-Fi password</label><p>For an open network, leave the Wi-Fi password blank. Secured networks require their password.</p>");
  out.print("<button>Save &amp; Connect</button></form><p>Saved networks: ");out.print(config.count);out.print("</p>");
  for(int i=0;i<config.count;++i) {
    out.print("<form method='POST' action='/'><input type='hidden' name='id' value='");out.print(i);out.print("'>");
    out.print(i+1);out.print(". ");out.print(escapeHTML(String(config.profiles[i].ssid)));out.print(" <button type='submit' name='operation' value='connect'>Connect</button> <button type='submit' name='operation' value='delete'>Delete</button></form>");
  }
  out.print("<p>Connect uses the saved password and makes that network Priority 1. Other saved networks remain available for fallback.</p>");
  out.print("</section><form method='POST' action='/'><input type='hidden' name='operation' value='settings'><section><h3>Camera IP on the router network (STA)</h3><label>Address mode<select name='dhcp'>");
  out.print(device.dhcp?"<option value='1' selected>DHCP / Automatic</option><option value='0'>Static IP</option>":"<option value='1'>DHCP / Automatic</option><option value='0' selected>Static IP</option>");
  out.print("</select></label>");
  out.print(device.dhcp?"<fieldset id='staticFields' hidden disabled style='border:0;padding:0;margin:0'>":"<fieldset id='staticFields' style='border:0;padding:0;margin:0'>");
  field(out,"IP Address","ip",ipToString(IPAddress(device.ip)));
  field(out,"Gateway","gateway",ipToString(IPAddress(device.gateway)));
  field(out,"Netmask","mask",ipToString(IPAddress(device.mask)));
  field(out,"DNS","dns",ipToString(IPAddress(device.dns)));
  out.print("</fieldset>");
  field(out,"Camera hostname","hostname",device.hostname);
  out.print("<p>Hostname is the camera name on your router, not a Wi-Fi SSID. DHCP assigns IP/gateway/netmask/DNS automatically. Manual fields appear only in Static IP mode. Setup AP stays at 192.168.1.1. Use a separate station subnet.</p></section><section><h3>Server ports</h3>");
  field(out,"HTTP port","http",String(device.httpPort),"number");
  out.print("<label>HTTPS port<input value='Unavailable: no TLS server' disabled></label>");
  field(out,"RTSP port","rtsp",String(device.rtspPort),"number");
  out.print("<p>Port 80 remains available for recovery. HTTP and RTSP ports must differ.</p></section><section><h3>Setup access point</h3>");
  field(out,"AP SSID","ap_ssid",device.apSSID);
  field(out,"AP password","ap_pass","","password");
  out.print("<label style='display:block'><input type='checkbox' id='showApPassword'> Show new AP password</label>");
  out.print("<p>Leave AP password blank to keep it.</p>");
  field(out,"AP channel","channel",String(device.apChannel),"number");
  out.print("</section><section><h3>H.264 video</h3><label>Resolution<select name='resolution'>");
  const int sizes[]={VIDEO_VGA,VIDEO_HD,VIDEO_FHD};const char *labels[]={"640 x 480","1280 x 720","1920 x 1080"};
  for(int i=0;i<3;++i) { out.print("<option value='");out.print(sizes[i]);out.print("'");if(device.resolution==sizes[i])out.print(" selected");out.print(">");out.print(labels[i]);out.print("</option>"); }
  out.print("</select></label>");
  field(out,"Frame rate (1-30)","fps",String(device.fps),"number");
  field(out,"Bitrate (kbps)","bitrate",String(device.bitrate/1000),"number");
  out.print("</section><section><h3>Connection timing</h3>");
  field(out,"Retry (seconds)","retry",String(device.retryMs/1000),"number");
  field(out,"Settle (seconds)","settle",String(device.settleMs/1000),"number");
  out.print("<p>SDK association/DHCP may take longer than the settling window.</p><button>Save device settings</button></section></form>");
  if(settingsPending)out.print("<p class='notice'>Saved settings are pending. Restart to apply.</p>");
  out.print("<form method='POST' action='/'><input type='hidden' name='operation' value='restart'><button>Restart camera</button></form><p>Save settings first. Status refresh is manual to reduce traffic. Restart disconnects Wi-Fi and video.</p></main>");
  out.print(R"ACTIONJS(<script>
(function(){
  var notice=document.getElementById('actionNotice'),timer;
  var names={save:'Save & Connect',connect:'Connect saved network',delete:'Delete network',settings:'Save device settings',restart:'Restart camera',refresh:'Refresh status',scan:'Scan nearby Wi-Fi'};
  function action(form,button){if(button&&button.name==='operation')return button.value;var op=form.querySelector('input[name="operation"]');return op?op.value:'';}
  function passwordToggle(id,name){document.getElementById(id).addEventListener('change',function(){var input=document.querySelector('input[name="'+name+'"]');input.type=this.checked?'text':'password';});}
  passwordToggle('showWifiPassword','pass');passwordToggle('showApPassword','ap_pass');
  document.getElementById('nearbyNetworks').addEventListener('change',function(){if(this.value)document.querySelector('input[name="ssid"]').value=this.value;});
  var addressMode=document.querySelector('select[name="dhcp"]'),staticFields=document.getElementById('staticFields');
  function updateAddressMode(){var automatic=addressMode.value==='1';staticFields.hidden=automatic;staticFields.disabled=automatic;}
  addressMode.addEventListener('change',updateAddressMode);updateAddressMode();
  function show(text){notice.textContent=text;notice.scrollIntoView({block:'nearest'});}
  function remember(label){try{sessionStorage.setItem('amb82Action',label);}catch(e){}}
  function sending(label){
    clearTimeout(timer);remember(label);show(label+': click registered. Sending request to the board...');
    timer=setTimeout(function(){show(label+': no new page response yet. The click worked; board processing is not confirmed. Check Wi-Fi and Serial Monitor.');},8000);
  }
  var reply=document.getElementById('boardReply'),previous='';
  try{previous=sessionStorage.getItem('amb82Action')||'';sessionStorage.removeItem('amb82Action');}catch(e){}
  if(reply)show((previous?previous+' - ':'')+'Board response: '+reply.textContent);
  else if(previous)show(previous+': page loaded, but no processing result was supplied.');
  document.addEventListener('click',function(e){
    var button=e.target.closest('button');
    if(button&&button.form)show((names[action(button.form,button)]||button.textContent)+': click registered. Checking the form...');
    var link=e.target.closest('a');if(link&&link.getAttribute('href')==='/')sending(names.refresh);
  },true);
  document.addEventListener('submit',function(e){
    sending(names[action(e.target,e.submitter)]||'Submit');
  },true);
  document.addEventListener('invalid',function(){clearTimeout(timer);show('Click registered, but a form field is invalid. Correct the highlighted field; nothing was sent.');},true);
})();
</script></body></html>)ACTIONJS");
}
class PageBuffer : public Print {
public:
  String text;
  PageBuffer() { text.reserve(6144); }
  size_t write(uint8_t byte) override { text += char(byte); return 1; }
};
bool sendAll(HttpClient &client, const String &data) {
  size_t offset=0; unsigned long started=millis();
  while(offset<data.length() && millis()-started<5000UL && client.connected()) {
    size_t n=data.length()-offset;if(n>1024)n=1024;
    size_t sent=client.write(reinterpret_cast<const uint8_t *>(data.c_str())+offset,n);
    offset+=sent;
    if(!sent)delay(1);
  }
  return offset==data.length();
}
void sendPage(HttpClient &client, const String &message = "") {
  PageBuffer page;
  renderPage(page, message);
  String header = "HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\nCache-Control: no-store\r\nConnection: close\r\nContent-Length: ";
  header += String(page.text.length()); header += "\r\n\r\n";
  if (sendAll(client, header)) sendAll(client, page.text);
}
// ===================================================
// 17. HTTP REQUEST HANDLER
// ===================================================
bool decimal(const String &s,uint32_t &n,uint32_t min,uint32_t max) {
  if(!s.length() || s.length()>10)return false;
  uint64_t v=0;for(size_t i=0;i<s.length();++i) { if(s[i]<'0'||s[i]>'9')return false;v=v*10+s[i]-'0';if(v>max)return false; }
  if(v<min)return false;n=v;return true;
}
bool parseIPv4(const String &s,uint8_t *ip) {
  int start=0;for(int i=0;i<4;++i) {
    int end=s.indexOf('.',start);if(i==3) { if(end>=0)return false;end=s.length(); } else if(end<0)return false;
    uint32_t n;if(!decimal(s.substring(start,end),n,0,255))return false;ip[i]=n;start=end+1;
  }return true;
}
bool updateDeviceSettings(const String &body) {
  DeviceSettings next=device;uint32_t n;
  if(!decimal(formValue(body,"dhcp"),n,0,1))return false;next.dhcp=n;
  if(!next.dhcp && (!parseIPv4(formValue(body,"ip"),next.ip)||!parseIPv4(formValue(body,"gateway"),next.gateway)||
     !parseIPv4(formValue(body,"mask"),next.mask)||!parseIPv4(formValue(body,"dns"),next.dns)))return false;
  if(!decimal(formValue(body,"http"),n,1,65535))return false;next.httpPort=n;
  if(!decimal(formValue(body,"rtsp"),n,1,65535))return false;next.rtspPort=n;
  if(!decimal(formValue(body,"channel"),n,1,165))return false;next.apChannel=n;
  if(!decimal(formValue(body,"resolution"),n,0,10))return false;next.resolution=n;
  if(!decimal(formValue(body,"fps"),n,1,30))return false;next.fps=n;
  if(!decimal(formValue(body,"bitrate"),n,128,8000))return false;next.bitrate=n*1000;
  if(!decimal(formValue(body,"retry"),n,5,300))return false;next.retryMs=n*1000;
  if(!decimal(formValue(body,"settle"),n,1,60))return false;next.settleMs=n*1000;
  String hostname=formValue(body,"hostname"),ap=formValue(body,"ap_ssid"),pass=formValue(body,"ap_pass");
  if(!hostname.length()||hostname.length()>32||!ap.length()||ap.length()>32||pass.length()>63||
     (pass.length() && pass.length()<8))return false;
  hostname.toCharArray(next.hostname,sizeof(next.hostname));ap.toCharArray(next.apSSID,sizeof(next.apSSID));
  if(pass.length())pass.toCharArray(next.apPassword,sizeof(next.apPassword));
  if(!validateSettings(next))return false;
  DeviceSettings previous=device;device=next;
  if(!saveDeviceSettings()) { device=previous;settingsError="Flash verification failed. Settings were not accepted.";return false; }
  settingsPending=true;return true;
}
bool readHttpLine(HttpClient &client,String &line,size_t limit,unsigned long timeout) {
  line="";unsigned long started=millis();
  while(client.connected() && millis()-started<timeout) {
    int value=client.read();
    if(value<0)continue;
    if(value=='\n')return true;
    if(value=='\r')continue;
    if(line.length()>=limit)return false;
    line+=char(value);
  }
  return false;
}
void handleHTTP() {
  HttpClient client(acceptHttp());
  if (!client) {
    return;
  }
  String request;
  if(!readHttpLine(client,request,512,5000UL)) {
    sendAll(client,"HTTP/1.1 408 Request Timeout\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
    Serial.println("[HTTP] Incomplete request line; no settings changed.");
    client.stop();return;
  }
  request.trim();
  int end=0;
  while(end<(int)request.length() && !isspace(static_cast<unsigned char>(request[end])))++end;
  String method=request.substring(0,end);method.toUpperCase();
  bool isGet=method=="GET",isPost=method=="POST";
  if(!isGet && !isPost) {
    sendAll(client,"HTTP/1.1 405 Method Not Allowed\r\nAllow: GET, POST\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
    Serial.println("[HTTP] Unsupported request method.");
    client.stop();return;
  }
  // Serve the same page for GET URL variants, including query strings and
  // absolute-form requests. The form's POST operation chooses the action.
  Serial.println(isPost?"[HTTP] POST configuration":"[HTTP] GET configuration");
  // ---------- Read request headers ----------
  int contentLength = 0;
  bool lengthSeen=false;
  unsigned long headerStarted = millis();
  unsigned int headerBytes = 0;
  while (client.connected()) {
    if (millis() - headerStarted > 6000UL || headerBytes > 3072) { client.stop(); return; }
    String header;
    if(!readHttpLine(client,header,1024,3000UL)) {
      sendAll(client,"HTTP/1.1 408 Request Timeout\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
      Serial.println("[HTTP] Incomplete headers; no settings changed.");
      client.stop();return;
    }
    headerBytes += header.length();
    header.trim();
    if (header.length() == 0) {
      break;
    }
    const char *raw=header.c_str(),*colon=strchr(raw,':');
    if(colon) {
      size_t keyLength=colon-raw;
      while(keyLength && isspace(static_cast<unsigned char>(raw[keyLength-1])))--keyLength;
      if(keyLength==17 && strncasecmp(raw,"Transfer-Encoding",17)==0) {
        sendPage(client,"Unsupported request encoding. Reload this page and use its form buttons.");
        client.stop();return;
      }
      if(keyLength==14 && strncasecmp(raw,"Content-Length",14)==0) {
        const char *value=colon+1;while(*value==' '||*value=='\t')++value;
        char *tail=nullptr;unsigned long count=strtoul(value,&tail,10);
        while(tail && (*tail==' '||*tail=='\t'))++tail;
        if(lengthSeen || value==tail || !isdigit(static_cast<unsigned char>(*value)) || !tail || *tail || count>1536) {
          sendPage(client,"Invalid request length. Nothing was changed.");client.stop();return;
        }
        contentLength=static_cast<int>(count);lengthSeen=true;
      }
    }
  }
  if(isGet && request.indexOf("/favicon.ico")>=0) {
    sendAll(client,"HTTP/1.1 204 No Content\r\nConnection: close\r\n\r\n");
    client.stop();return;
  }
  if (contentLength < 0 ||
      contentLength > 1536) {
    client.println(
        "HTTP/1.1 413 Payload Too Large"
    );
    client.println("Connection: close");
    client.println();
    client.stop();
    return;
  }
  // ---------- Read POST body ----------
  if(isPost && (!lengthSeen || contentLength==0)) {
    Serial.println("[HTTP] POST has no form body length; no action executed.");
    sendPage(client,"POST reached the board, but the form body is missing (Content-Length absent or zero). No action was executed. Reload the latest page and try again.");
    client.stop();return;
  }
  String body = "";
  if(contentLength && !body.reserve(contentLength)) {
    sendPage(client,"Not enough memory to receive the form. No action was executed.");client.stop();return;
  }
  unsigned long started = millis();
  while (
      (int)body.length() < contentLength &&
      millis() - started < 5000UL
  ) {
    int value=client.read();
    if(value>=0)body+=static_cast<char>(value);
    else if(!client.connected())break;
  }
  if ((int)body.length() != contentLength) {
    client.println("HTTP/1.1 400 Bad Request\r\nConnection: close\r\n\r\nIncomplete body");
    client.stop();
    return;
  }
  String message = "";
  String operation=formValue(body,"operation");
  Serial.print("[HTTP] Form bytes expected/received: ");Serial.print(contentLength);Serial.print('/');Serial.println(body.length());
  Serial.print("[HTTP] Recognized action: ");
  bool knownOperation=operation=="save"||operation=="connect"||operation=="delete"||operation=="settings"||operation=="restart"||operation=="scan";
  Serial.println(knownOperation?operation.c_str():"MISSING OR INVALID");
  bool changed = false;
  if(isPost && operation=="scan") {
    uint8_t command=1;
    if(workerReady && scanRequests && scanReports) {
      xQueueOverwrite(scanRequests,&command);nearby.scanning=true;
      message="Scan queued after any active connection attempt. Refresh status shortly to see the nearby Wi-Fi list.";
    } else message="Scan service unavailable. Restart the camera.";
  } else if(isPost && operation=="settings") {
    settingsError="";
    if(updateDeviceSettings(body)) message="Device settings saved. Restart to apply them.";
    else message=settingsError.length()?settingsError:String("Invalid settings. Check IP/subnet/gateway, ports and field ranges. Nothing was saved.");
  } else if(isPost && operation=="restart") {
    message="Restart scheduled. Rejoin the setup AP and reopen 192.168.1.1 after startup.";
    rebootAt=millis()+4000UL;
  } else if (isPost && operation == "save") {
    String ssid = formValue(body, "ssid");
    String pass = formValue(body, "pass");
    if (addNetwork(ssid, pass)) {
      changed = true;
      message =
          "Credentials saved. Connecting to new WiFi.";
    } else {
      message =
          "Invalid credentials or flash save failed.";
    }
  // ---------- Delete network ----------
  } else if(isPost && operation=="connect") {
    uint32_t index;
    if(decimal(formValue(body,"id"),index,0,MAX_NETWORKS-1) && connectSavedProfile(static_cast<int>(index))) {
      changed=true;
      message="Switch queued for saved network: ";message+=requestedSSID;message+=". Refresh status to check the connection.";
    } else message="Connect failed: invalid saved network or profile save failed. Nothing was switched.";
  } else if (
      isPost && operation == "delete"
  ) {
    String id = formValue(body, "id");
    int index = -1;
    if (
        id.length() == 1 &&
        id[0] >= '0' &&
        id[0] <= '2'
    ) {
      index = id.toInt();
    }
    if (removeNetwork(index)) {
      changed = true;
      message = "Network deleted.";
    } else {
      message = "Invalid network ID.";
    }
  }
  if (changed) {
    strncpy(pageNotice, message.c_str(), sizeof(pageNotice) - 1);
    // Return the updated list in this response, without a redirect.
    sendPage(client, String(pageNotice));
    Serial.print("[HTTP] Saved profiles displayed: "); Serial.println(config.count);
  } else {
    if(isGet)sendPage(client,"Page request received. Connection status refreshed.");
    else {
      if(!message.length()) {
        message="Form received but its action field is missing or invalid. Expected bytes: ";
        message+=String(contentLength);message+=", received: ";message+=String(body.length());
        message+=". No action was executed. Firmware: form-parser-2.";
      }
      sendPage(client,message);
    }
  }
  delay(50);
  client.stop();
}
// ===================================================
// 18. MAIN SETUP
// ===================================================
void setup() {
  Serial.begin(115200);
  delay(1500);
  Serial.println();
  Serial.println("================================");
  Serial.println(" AMB82 WIFI MANAGER + SINGLE PAGE STATUS");
  Serial.println("================================");
  // Step 1: Load saved WiFi profiles.
  loadDeviceSettings();
  loadConfig();
  ensureRamProfile();
  WiFi.setHostname(runningDevice.hostname);
  if(!runningDevice.dhcp) WiFi.config(IPAddress(runningDevice.ip),IPAddress(runningDevice.dns),IPAddress(runningDevice.gateway),IPAddress(runningDevice.mask));
  // Step 2: Enable concurrent AP + STA.
  startAccessPoint();
  connectionJobs = xQueueCreate(1, sizeof(ConnectionJob));
  connectionReports = xQueueCreate(1, sizeof(ConnectionReport));
  scanRequests=xQueueCreate(1,sizeof(uint8_t));
  scanReports=xQueueCreate(1,sizeof(ScanReport));
  if (connectionJobs && connectionReports)
    workerReady = xTaskCreate(connectionWorker, "wifi-connect", 4096, nullptr, 1, nullptr) == pdPASS;
  if (!workerReady) {
    connectionState = "FAILED";
    lastConnectionError = "Unable to create connection task.";
  }
  // Queue only after HTTP and camera setup have completed.
  // Step 4: Start HTTP server.
  beginHttp();
  Serial.println("[HTTP] Server started");
  Serial.print("[HTTP] AP URL: http://");
  Serial.println(WiFi.localIP(1));
  // Step 5: Start RTSP stream.
  startRTSP();
  lastRetry = millis();
  connectSavedNetworks();
  Serial.println("[SYSTEM] Ready");
}
// ===================================================
// 19. MAIN LOOP
// ===================================================
void loop() {
  serviceConnection();
  handleHTTP();
  if (reconnectRequested && (int32_t)(millis() - applyAfter) >= 0) {
    reconnectRequested = false;
    // Discard an older report before queuing the latest profile snapshot.
    ConnectionReport old;
    if (connectionReports) xQueueReceive(connectionReports, &old, 0);
    connectSavedNetworks();
  }
  if(rebootAt && (int32_t)(millis()-rebootAt)>=0)sys_reset();
  delay(2);
}
