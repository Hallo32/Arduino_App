#include <Arduino.h>
#include <ArduinoJson.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <HTTPClient.h>
#include <HTTPUpdate.h>
#include <Preferences.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <math.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

constexpr char GITHUB_RELEASE_API[] =
    "https://api.github.com/repos/Hallo32/Arduino_App/releases/latest";
constexpr char FIRMWARE_ASSET_NAME[] = "blinky.ino.bin";
constexpr char DEVICE_HOSTNAME_PREFIX[] = "ESP32-C6-";

constexpr char BLE_UART_SERVICE_UUID[] =
    "6E400001-B5A3-F393-E0A9-E50E24DCCA9E";
constexpr char BLE_UART_RX_UUID[] =
    "6E400002-B5A3-F393-E0A9-E50E24DCCA9E";
constexpr char BLE_UART_TX_UUID[] =
    "6E400003-B5A3-F393-E0A9-E50E24DCCA9E";

constexpr char WIFI_NAMESPACE[] = "wifi";
constexpr char OTA_NAMESPACE[] = "ota";

constexpr uint32_t BLINK_INTERVAL_MS = 500;
constexpr uint32_t TIME_SYNC_TIMEOUT_MS = 20000;
constexpr uint32_t TEMPERATURE_INTERVAL_MS = 300000;
constexpr time_t VALID_TIME_THRESHOLD = 1700000000;

constexpr char GERMAN_TIME_ZONE[] =
    "CET-1CEST,M3.5.0/2,M10.5.0/3";

enum class ProvisioningState : uint8_t {
  Idle,
  WaitingForSsid,
  WaitingForPassword
};

bool ledState = false;
bool otaCheckComplete = false;
uint32_t lastLedToggle = 0;

String wifiSsid;
String wifiPassword;
String installedReleaseTag;
String deviceHostname;
String pendingSsid;
String serialLine;

ProvisioningState provisioningState = ProvisioningState::Idle;

BLEServer *bleServer = nullptr;
BLECharacteristic *bleTxCharacteristic = nullptr;
QueueHandle_t bleCommandQueue = nullptr;

uint32_t lastTemperatureSent = 0;

struct BleCommand {
  char text[65];
};


// -----------------------------------------------------------------------------
// BLE output
// -----------------------------------------------------------------------------

void sendBleText(const char *text, size_t length) {
  if (bleTxCharacteristic == nullptr ||
      bleServer == nullptr ||
      bleServer->getConnectedCount() == 0) {
    return;
  }

  constexpr size_t BLE_CHUNK_SIZE = 20;

  for (size_t offset = 0; offset < length; offset += BLE_CHUNK_SIZE) {
    const size_t chunkLength =
        min(BLE_CHUNK_SIZE, length - offset);

    bleTxCharacteristic->setValue(
        reinterpret_cast<const uint8_t *>(text + offset),
        chunkLength);

    bleTxCharacteristic->notify();

    delay(10);
  }
}

void consolePrintln(const char *text) {
  Serial.println(text);

  sendBleText(text, strlen(text));
  sendBleText("\r\n", 2);
}

void consolePrintf(const char *format, ...) {
  char buffer[256];

  va_list arguments;
  va_start(arguments, format);

  const int length =
      vsnprintf(buffer, sizeof(buffer), format, arguments);

  va_end(arguments);

  if (length <= 0) {
    return;
  }

  const size_t outputLength =
      min(static_cast<size_t>(length),
          sizeof(buffer) - 1);

  Serial.write(
      reinterpret_cast<const uint8_t *>(buffer),
      outputLength);

  sendBleText(buffer, outputLength);
}


// -----------------------------------------------------------------------------
// BLE RX callbacks
// -----------------------------------------------------------------------------

class BleUartRxCallbacks : public BLECharacteristicCallbacks {
 public:
  void onWrite(BLECharacteristic *characteristic) override {
    const uint8_t *data = characteristic->getData();
    const size_t length = characteristic->getLength();

    consolePrintf(
        "BLE UART RX write: %u bytes\n",
        static_cast<unsigned>(length));

    for (size_t index = 0; index < length; ++index) {
      const char value =
          static_cast<char>(data[index]);

      if (value == '\r') {
        continue;
      }

      if (value == '\n' || value == 0x03) {
        BleCommand command{};

        if (value == 0x03) {
          command.text[0] = value;
        } else {
          memcpy(
              command.text,
              line_,
              lineLength_);

          command.text[lineLength_] = '\0';
        }

        if (bleCommandQueue != nullptr) {
          xQueueSend(
              bleCommandQueue,
              &command,
              0);
        }

        lineLength_ = 0;
        continue;
      }

      if (lineLength_ < sizeof(line_) - 1) {
        line_[lineLength_++] = value;
      } else {
        lineLength_ = 0;
      }
    }
  }

 private:
  char line_[64]{};
  size_t lineLength_ = 0;
};


// -----------------------------------------------------------------------------
// BLE TX callbacks
// -----------------------------------------------------------------------------

class BleUartTxCallbacks : public BLECharacteristicCallbacks {
 public:
  void onStatus(
      BLECharacteristic *,
      Status status,
      uint32_t code) override {

    if (status == SUCCESS_NOTIFY) {
      return;
    }

    // Bewusst nur Serial verwenden.
    // Kein consolePrintf() innerhalb eines BLE-Callbacks,
    // damit bei einem Notify-Fehler nicht erneut notify()
    // ausgelöst wird.

    Serial.printf(
        "BLE UART TX notification status=%u "
        "code=%lu (0x%08lX)\n",
        static_cast<unsigned>(status),
        static_cast<unsigned long>(code),
        static_cast<unsigned long>(code));
  }
};


// -----------------------------------------------------------------------------
// BLE server diagnostic callbacks
// -----------------------------------------------------------------------------

class BleServerDebugCallbacks : public BLEServerCallbacks {
 public:

  void onConnect(
      BLEServer *server,
      ble_gap_conn_desc *desc) override {

    // Nur Serial verwenden.
    // Im Connect-Callback kann getConnectedCount() noch 0 sein.
    Serial.println();
    Serial.println("========== BLE CONNECT ==========");

    Serial.printf(
        "handle       : %u\n",
        static_cast<unsigned>(desc->conn_handle));

    Serial.printf(
        "clients      : %lu\n",
        static_cast<unsigned long>(
            server->getConnectedCount()));

    Serial.printf(
        "local MTU    : %u\n",
        static_cast<unsigned>(BLEDevice::getMTU()));

    Serial.printf(
        "peer MTU     : %u\n",
        static_cast<unsigned>(
            server->getPeerMTU(desc->conn_handle)));

    Serial.printf(
        "interval     : %u (%.2f ms)\n",
        static_cast<unsigned>(desc->conn_itvl),
        desc->conn_itvl * 1.25);

    Serial.printf(
        "latency      : %u\n",
        static_cast<unsigned>(desc->conn_latency));

    Serial.printf(
        "timeout      : %u (%.0f ms)\n",
        static_cast<unsigned>(
            desc->supervision_timeout),
        desc->supervision_timeout * 10.0);

    Serial.printf(
        "encrypted    : %s\n",
        desc->sec_state.encrypted ? "yes" : "no");

    Serial.printf(
        "authenticated: %s\n",
        desc->sec_state.authenticated ? "yes" : "no");

    Serial.printf(
        "bonded       : %s\n",
        desc->sec_state.bonded ? "yes" : "no");

    Serial.println("=================================");
  }


  void onDisconnect(
      BLEServer *server,
      ble_gap_conn_desc *desc) override {

    // Besonders wichtig:
    // desc->reason ist der Grund für den Disconnect.

    Serial.println();
    Serial.println("======== BLE DISCONNECT =========");

    Serial.printf(
        "handle       : %u\n",
        static_cast<unsigned>(desc->conn_handle));

    Serial.printf(
        "reason       : %u (%s)\n",
        static_cast<unsigned>(desc->reason),
        BLEUtils::returnCodeToString(desc->reason));

    Serial.printf(
        "clients      : %lu\n",
        static_cast<unsigned long>(
            server->getConnectedCount()));

    Serial.printf(
        "encrypted    : %s\n",
        desc->sec_state.encrypted ? "yes" : "no");

    Serial.printf(
        "authenticated: %s\n",
        desc->sec_state.authenticated ? "yes" : "no");

    Serial.printf(
        "bonded       : %s\n",
        desc->sec_state.bonded ? "yes" : "no");

    Serial.println("=================================");

    // Advertising nach Disconnect wieder starten.
    BLEDevice::startAdvertising();
  }


  void onMtuChanged(
      BLEServer *server,
      ble_gap_conn_desc *desc,
      uint16_t mtu) override {

    Serial.printf(
        "BLE MTU CHANGED: "
        "handle=%u, mtu=%u, peer MTU=%u\n",
        static_cast<unsigned>(desc->conn_handle),
        static_cast<unsigned>(mtu),
        static_cast<unsigned>(
            server->getPeerMTU(desc->conn_handle)));
  }


  void onConnParamsUpdate(
      uint16_t conn_handle,
      uint16_t interval,
      uint16_t latency,
      uint16_t timeout,
      uint8_t status) override {

    Serial.printf(
        "BLE CONN PARAMS: "
        "handle=%u, "
        "interval=%u (%.2f ms), "
        "latency=%u, "
        "timeout=%u (%.0f ms), "
        "status=%u\n",
        static_cast<unsigned>(conn_handle),
        static_cast<unsigned>(interval),
        interval * 1.25,
        static_cast<unsigned>(latency),
        static_cast<unsigned>(timeout),
        timeout * 10.0,
        static_cast<unsigned>(status));
  }
};


// -----------------------------------------------------------------------------
// Forward declarations
// -----------------------------------------------------------------------------

void checkForFirmwareUpdate();
void handleSerialLine(const String &line);
void cancelWifiProvisioning();


// -----------------------------------------------------------------------------
// Settings
// -----------------------------------------------------------------------------

void loadSettings() {
  Preferences preferences;

  if (preferences.begin(WIFI_NAMESPACE, true)) {
    wifiSsid =
        preferences.getString("ssid", "");

    wifiPassword =
        preferences.getString("password", "");

    preferences.end();
  }

  if (preferences.begin(OTA_NAMESPACE, true)) {
    installedReleaseTag =
        preferences.getString("tag", "");

    preferences.end();
  }
}


// -----------------------------------------------------------------------------
// Wi-Fi
// -----------------------------------------------------------------------------

void connectToWifi() {
  WiFi.mode(WIFI_STA);
  WiFi.setHostname(deviceHostname.c_str());
  WiFi.setAutoReconnect(true);

  WiFi.begin(
      wifiSsid.c_str(),
      wifiPassword.c_str());

  consolePrintln(
      "Connecting to saved Wi-Fi network");
}

void startWifiProvisioning() {
  WiFi.disconnect(false, false);

  pendingSsid = "";
  provisioningState =
      ProvisioningState::WaitingForSsid;

  consolePrintln(
      "Wi-Fi setup: existing credentials will be overwritten");

  consolePrintln(
      "Enter SSID in the serial monitor");
}


// -----------------------------------------------------------------------------
// Hostname
// -----------------------------------------------------------------------------

String createDeviceHostname() {
  const uint64_t mac =
      ESP.getEfuseMac();

  char hostname[32];

  snprintf(
      hostname,
      sizeof(hostname),
      "%s%06llX",
      DEVICE_HOSTNAME_PREFIX,
      static_cast<unsigned long long>(
          mac & 0xFFFFFFULL));

  return String(hostname);
}


// -----------------------------------------------------------------------------
// BLE setup
// -----------------------------------------------------------------------------

void startBleUart() {
  Serial.println();
  Serial.println("========== BLE INIT ==============");

  if (!BLEDevice::init(deviceHostname)) {
    Serial.println(
        "BLE initialization failed");
    Serial.println(
        "=================================");
    return;
  }

  Serial.printf(
      "BLE local MTU: %u\n",
      static_cast<unsigned>(
          BLEDevice::getMTU()));

  bleCommandQueue =
      xQueueCreate(
          4,
          sizeof(BleCommand));

  if (bleCommandQueue == nullptr) {
    Serial.println(
        "BLE UART command queue creation failed");
    Serial.println(
        "=================================");
    return;
  }

  bleServer =
      BLEDevice::createServer();

  if (bleServer == nullptr) {
    Serial.println(
        "BLE server creation failed");
    Serial.println(
        "=================================");
    return;
  }

  bleServer->setCallbacks(
      new BleServerDebugCallbacks());

  BLEService *service =
      bleServer->createService(
          BLE_UART_SERVICE_UUID);

  if (service == nullptr) {
    Serial.println(
        "BLE service creation failed");
    Serial.println(
        "=================================");
    return;
  }

  BLECharacteristic *rxCharacteristic =
      service->createCharacteristic(
          BLE_UART_RX_UUID,
          BLECharacteristic::PROPERTY_WRITE |
          BLECharacteristic::PROPERTY_WRITE_NR);

  if (rxCharacteristic == nullptr) {
    Serial.println(
        "BLE RX characteristic creation failed");
    Serial.println(
        "=================================");
    return;
  }

  rxCharacteristic->setCallbacks(
      new BleUartRxCallbacks());

  bleTxCharacteristic =
      service->createCharacteristic(
          BLE_UART_TX_UUID,
          BLECharacteristic::PROPERTY_NOTIFY);

  if (bleTxCharacteristic == nullptr) {
    Serial.println(
        "BLE TX characteristic creation failed");
    Serial.println(
        "=================================");
    return;
  }

  bleTxCharacteristic->setValue("");

  bleTxCharacteristic->setCallbacks(
      new BleUartTxCallbacks());

  service->start();

  BLEAdvertising *advertising =
      BLEDevice::getAdvertising();

  advertising->addServiceUUID(
      BLE_UART_SERVICE_UUID);

  advertising->setScanResponse(true);

  BLEDevice::startAdvertising();

  Serial.printf(
      "BLE service UUID: %s\n",
      BLE_UART_SERVICE_UUID);

  Serial.printf(
      "BLE RX UUID     : %s\n",
      BLE_UART_RX_UUID);

  Serial.printf(
      "BLE TX UUID     : %s\n",
      BLE_UART_TX_UUID);

  Serial.println(
      "BLE advertising started");

  Serial.println(
      "BLE UART ready; connect with a Nordic UART compatible app");

  Serial.println(
      "=================================");
}


// -----------------------------------------------------------------------------
// BLE command processing
// -----------------------------------------------------------------------------

void processBleCommands() {
  if (bleCommandQueue == nullptr) {
    return;
  }

  BleCommand command;

  while (
      xQueueReceive(
          bleCommandQueue,
          &command,
          0) == pdTRUE) {

    if (static_cast<uint8_t>(
            command.text[0]) == 0x03) {

      cancelWifiProvisioning();

    } else {

      handleSerialLine(
          String(command.text));
    }
  }
}


// -----------------------------------------------------------------------------
// Console help
// -----------------------------------------------------------------------------

void printSerialHelp() {
  consolePrintln("Serial commands:");
  consolePrintln("  wifi - configure Wi-Fi credentials");
  consolePrintln("  Ctrl+C - cancel Wi-Fi configuration");
  consolePrintln("  ip - show the current IP address");
  consolePrintln("  hostname - show the device hostname");
  consolePrintln("  wlan - show Wi-Fi network information");
  consolePrintln("  time - show UTC and German local time");
  consolePrintln("  version - show the installed firmware version");
  consolePrintln("  update - check for a firmware update");
}


// -----------------------------------------------------------------------------
// Wi-Fi information
// -----------------------------------------------------------------------------

void printIpAddress() {
  if (WiFi.status() == WL_CONNECTED) {
    consolePrintf(
        "IP address: %s\n",
        WiFi.localIP().toString().c_str());
  } else {
    consolePrintln(
        "IP address: Wi-Fi is not connected");
  }
}

void printHostname() {
  const char *hostname =
      WiFi.getHostname();

  consolePrintf(
      "Hostname: %s\n",
      hostname != nullptr
          ? hostname
          : "not set");
}

void printWlanInfo() {
  consolePrintf(
      "Wi-Fi status: %s\n",
      WiFi.status() == WL_CONNECTED
          ? "connected"
          : "disconnected");

  if (WiFi.status() != WL_CONNECTED) {
    return;
  }

  consolePrintf(
      "SSID: %s\n",
      WiFi.SSID().c_str());

  consolePrintf(
      "Signal strength: %d dBm\n",
      WiFi.RSSI());

  consolePrintf(
      "MAC address: %s\n",
      WiFi.macAddress().c_str());

  consolePrintf(
      "IP address: %s\n",
      WiFi.localIP().toString().c_str());

  consolePrintf(
      "Gateway: %s\n",
      WiFi.gatewayIP().toString().c_str());

  consolePrintf(
      "Subnet mask: %s\n",
      WiFi.subnetMask().toString().c_str());

  consolePrintf(
      "DNS server: %s\n",
      WiFi.dnsIP().toString().c_str());
}


// -----------------------------------------------------------------------------
// Time
// -----------------------------------------------------------------------------

void printCurrentTime() {
  const time_t currentTime =
      time(nullptr);

  if (currentTime < VALID_TIME_THRESHOLD) {
    consolePrintln(
        "Time: not synchronized");
    return;
  }

  struct tm utcTime;
  gmtime_r(
      &currentTime,
      &utcTime);

  struct tm localTime;
  localtime_r(
      &currentTime,
      &localTime);

  char formattedUtcTime[24];
  char formattedLocalTime[24];

  strftime(
      formattedUtcTime,
      sizeof(formattedUtcTime),
      "%Y-%m-%dT%H:%M:%S",
      &utcTime);

  strftime(
      formattedLocalTime,
      sizeof(formattedLocalTime),
      "%Y-%m-%dT%H:%M:%S",
      &localTime);

  consolePrintf(
      "Time (UTC): %sZ\n",
      formattedUtcTime);

  consolePrintf(
      "Time (Germany): %s %s\n",
      formattedLocalTime,
      localTime.tm_isdst > 0
          ? "CEST"
          : "CET");
}


// -----------------------------------------------------------------------------
// Firmware version
// -----------------------------------------------------------------------------

void printFirmwareVersion() {
  if (installedReleaseTag.isEmpty()) {
    consolePrintln(
        "Firmware version: unknown (no release tag in NVS)");
  } else {
    consolePrintf(
        "Firmware version: %s\n",
        installedReleaseTag.c_str());
  }
}


// -----------------------------------------------------------------------------
// Wi-Fi provisioning
// -----------------------------------------------------------------------------

void cancelWifiProvisioning() {
  provisioningState =
      ProvisioningState::Idle;

  pendingSsid = "";
  serialLine = "";

  consolePrintln(
      "Wi-Fi setup cancelled; existing credentials were preserved");

  if (!wifiSsid.isEmpty()) {
    connectToWifi();
  }
}

bool saveWifiCredentials(
    const String &ssid,
    const String &password) {

  Preferences preferences;

  if (!preferences.begin(
          WIFI_NAMESPACE,
          false)) {
    return false;
  }

  const bool saved =
      preferences.putString(
          "ssid",
          ssid) > 0 &&
      preferences.putString(
          "password",
          password) > 0;

  preferences.end();

  return saved;
}


// -----------------------------------------------------------------------------
// Command handling
// -----------------------------------------------------------------------------

void handleSerialLine(
    const String &line) {

  if (provisioningState ==
      ProvisioningState::Idle) {

    if (line == "wifi") {

      startWifiProvisioning();

    } else if (
        line == "help" ||
        line.isEmpty()) {

      printSerialHelp();

    } else if (line == "ip") {

      printIpAddress();

    } else if (line == "hostname") {

      printHostname();

    } else if (line == "wlan") {

      printWlanInfo();

    } else if (line == "time") {

      printCurrentTime();

    } else if (line == "version") {

      printFirmwareVersion();

    } else if (line == "update") {

      checkForFirmwareUpdate();
    }

    return;
  }

  if (provisioningState ==
      ProvisioningState::WaitingForSsid) {

    if (line.isEmpty() ||
        line.length() > 32) {

      consolePrintln(
          "SSID must contain 1 to 32 characters; enter it again");

      return;
    }

    pendingSsid = line;

    provisioningState =
        ProvisioningState::WaitingForPassword;

    consolePrintln(
        "Enter Wi-Fi password (empty for an open network)");

    return;
  }

  if (line.length() > 63) {
    consolePrintln(
        "Password is too long; enter it again");

    return;
  }

  if (!saveWifiCredentials(
          pendingSsid,
          line)) {

    consolePrintln(
        "Could not save Wi-Fi credentials to NVS; retry with 'wifi'");

    provisioningState =
        ProvisioningState::Idle;

    return;
  }

  wifiSsid = pendingSsid;
  wifiPassword = line;

  pendingSsid = "";

  provisioningState =
      ProvisioningState::Idle;

  otaCheckComplete = false;

  connectToWifi();
}


// -----------------------------------------------------------------------------
// Serial input
// -----------------------------------------------------------------------------

void handleSerialInput() {
  while (Serial.available() > 0) {

    const char character =
        static_cast<char>(
            Serial.read());

    if (character == 0x03) {
      cancelWifiProvisioning();
      continue;
    }

    if (character == '\r') {
      continue;
    }

    if (character == '\n') {

      handleSerialLine(
          serialLine);

      serialLine = "";

    } else if (
        serialLine.length() < 64) {

      serialLine += character;

    } else {

      serialLine = "";

      consolePrintln(
          "Input too long; line discarded");
    }
  }
}


// -----------------------------------------------------------------------------
// NTP
// -----------------------------------------------------------------------------

bool synchronizeClock() {
  configTime(
      0,
      0,
      "pool.ntp.org",
      "time.nist.gov");

  const uint32_t startedAt =
      millis();

  while (
      time(nullptr) < VALID_TIME_THRESHOLD &&
      millis() - startedAt <
          TIME_SYNC_TIMEOUT_MS) {

    delay(250);
  }

  return time(nullptr) >=
         VALID_TIME_THRESHOLD;
}


// -----------------------------------------------------------------------------
// OTA
// -----------------------------------------------------------------------------

bool saveInstalledReleaseTag(
    const String &tag) {

  Preferences preferences;

  if (!preferences.begin(
          OTA_NAMESPACE,
          false)) {
    return false;
  }

  const bool saved =
      preferences.putString(
          "tag",
          tag) > 0;

  preferences.end();

  return saved;
}

void checkForFirmwareUpdate() {
  if (!synchronizeClock()) {
    consolePrintln(
        "OTA skipped: system time could not be synchronized");
    return;
  }

  WiFiClientSecure apiClient;

  apiClient.useBuiltinCACertBundle();
  apiClient.setTimeout(15000);

  HTTPClient apiRequest;

  if (!apiRequest.begin(
          apiClient,
          GITHUB_RELEASE_API)) {

    consolePrintln(
        "OTA skipped: could not start GitHub API request");

    return;
  }

  apiRequest.setTimeout(15000);

  apiRequest.addHeader(
      "Accept",
      "application/vnd.github+json");

  apiRequest.addHeader(
      "User-Agent",
      "ESP32-C6");

  apiRequest.addHeader(
      "X-GitHub-Api-Version",
      "2022-11-28");

  const int statusCode =
      apiRequest.GET();

  if (statusCode != HTTP_CODE_OK) {

    consolePrintf(
        "GitHub API request failed with HTTP %d\n",
        statusCode);

    apiRequest.end();

    return;
  }

  const String releasePayload =
      apiRequest.getString();

  apiRequest.end();

  if (releasePayload.isEmpty()) {

    consolePrintln(
        "Could not parse GitHub release JSON: empty response");

    return;
  }

  JsonDocument release;

  const DeserializationError jsonError =
      deserializeJson(
          release,
          releasePayload);

  if (jsonError) {

    consolePrintf(
        "Could not parse GitHub release JSON: %s\n",
        jsonError.c_str());

    return;
  }

  const String latestTag =
      release["tag_name"] | "";

  String firmwareUrl;

  for (
      JsonObject asset :
      release["assets"].as<JsonArray>()) {

    if (asset["name"] ==
        FIRMWARE_ASSET_NAME) {

      firmwareUrl =
          asset["browser_download_url"] | "";

      break;
    }
  }

  if (latestTag.isEmpty() ||
      firmwareUrl.isEmpty()) {

    consolePrintln(
        "Latest GitHub release has no tag or firmware asset");

    return;
  }

  if (latestTag ==
      installedReleaseTag) {

    consolePrintf(
        "OTA: release %s is already installed\n",
        latestTag.c_str());

    return;
  }

  if (!firmwareUrl.startsWith(
          "https://")) {

    consolePrintln(
        "OTA skipped: release asset URL is not HTTPS");

    return;
  }

  consolePrintf(
      "Installing GitHub release %s\n",
      latestTag.c_str());

  WiFiClientSecure firmwareClient;

  firmwareClient.useBuiltinCACertBundle();
  firmwareClient.setTimeout(15000);

  HTTPUpdate updater;

  updater.setFollowRedirects(
      HTTPC_STRICT_FOLLOW_REDIRECTS);

  updater.rebootOnUpdate(false);

  int lastReportedPercent = -1;

  updater.onProgress(
      [&lastReportedPercent](
          int current,
          int total) {

        if (total <= 0) {
          return;
        }

        const int percent =
            current * 100 / total;

        if (percent !=
            lastReportedPercent) {

          lastReportedPercent =
              percent;

          consolePrintf(
              "OTA progress: %d%%\n",
              percent);
        }
      });

  const t_httpUpdate_return result =
      updater.update(
          firmwareClient,
          firmwareUrl,
          installedReleaseTag);

  if (result ==
      HTTP_UPDATE_NO_UPDATES) {

    consolePrintln(
        "OTA server reports no update");

  } else if (
      result == HTTP_UPDATE_FAILED) {

    consolePrintf(
        "OTA failed (%d): %s\n",
        updater.getLastError(),
        updater.getLastErrorString().c_str());

  } else if (
      result == HTTP_UPDATE_OK) {

    if (!saveInstalledReleaseTag(
            latestTag)) {

      consolePrintln(
          "Could not save installed release tag to NVS");
    }

    consolePrintln(
        "OTA complete; restarting");

    ESP.restart();
  }
}


// -----------------------------------------------------------------------------
// Arduino setup
// -----------------------------------------------------------------------------

void setup() {
  Serial.begin(115200);

  setenv(
      "TZ",
      GERMAN_TIME_ZONE,
      1);

  tzset();

  pinMode(
      LED_BUILTIN,
      OUTPUT);

  digitalWrite(
      LED_BUILTIN,
      LOW);

  deviceHostname =
      createDeviceHostname();

  loadSettings();

  startBleUart();

  lastTemperatureSent =
      millis();

  if (wifiSsid.isEmpty()) {

    startWifiProvisioning();

  } else {

    connectToWifi();
  }

  printSerialHelp();
}


// -----------------------------------------------------------------------------
// Arduino loop
// -----------------------------------------------------------------------------

void loop() {
  handleSerialInput();

  processBleCommands();

  const uint32_t now =
      millis();

  if (now - lastLedToggle >=
      BLINK_INTERVAL_MS) {

    lastLedToggle = now;

    ledState = !ledState;

    digitalWrite(
        LED_BUILTIN,
        ledState ? HIGH : LOW);
  }

  if (!otaCheckComplete &&
      WiFi.status() == WL_CONNECTED) {

    otaCheckComplete = true;

    consolePrintln(
        "Wi-Fi connected; checking latest GitHub release");

    checkForFirmwareUpdate();
  }

  if (bleServer != nullptr &&
      bleServer->getConnectedCount() > 0 &&
      now - lastTemperatureSent >=
          TEMPERATURE_INTERVAL_MS) {

    lastTemperatureSent = now;

    const float temperatureCelsius =
        temperatureRead();

    if (isfinite(temperatureCelsius)) {

      consolePrintf(
          "Internal ESP32-C6 temperature: %.2f deg C\r\n",
          temperatureCelsius);

    } else {

      consolePrintln(
          "Internal temperature sensor read failed");
    }
  }
}
