#include "M5StickCPlus2.h" // M5StickC Plus2用ライブラリ
#include "M5UnitENV.h"     // 環境センサーユニット用ライブラリ
#include <WiFi.h>          // WiFi通信ライブラリ
#include <time.h>          // 時刻取得用ライブラリ
#include <PubSubClient.h>  // MQTT通信ライブラリ
#include "config.h"        // 設定ファイル

// ===========================
// MQTTクライアント設定
// ===========================
// WiFiとMQTTクライアントのインスタンス（設定はconfig.hで定義）
WiFiClient espClient;
PubSubClient client(espClient);

// ===========================
// NTP（時刻サーバー）設定
// ===========================
// 日本のNTPサーバーを指定し、JST（日本標準時）で設定
const char *ntpServer = "ntp.nict.jp";
const long gmtOffset_sec = 9 * 3600; // JST (UTC+9)
const int daylightOffset_sec = 0;

// ===========================
// CO2センサー設定
// ===========================
// SCD4X（CO2センサー）のインスタンスを作成
SCD4X scd4x;

// ===========================
// データ送信間隔設定
// ===========================
// MQTTで2分ごとにデータ送信するためのタイマー
const unsigned long mqtt_interval = 60 * 1000; // 1分（ミリ秒）
unsigned long lastMqttSend = 0;                // 最後に送信した時刻

// ===========================
// センサーデータ保持用変数
// ===========================
// 最新のCO2、温度、湿度の値を保持
uint16_t current_co2 = 0;
float current_temp = 0;
float current_humidity = 0;

// ===========================
// CO2濃度のレベル定義
// ===========================
// 良好・注意・危険の閾値を設定
const uint16_t CO2_GOOD = 1000;
const uint16_t CO2_WARNING = 1500;

// ===========================
// 表示色定義（Apple風）
// ===========================
// 状態に応じて色を変えるための定義
const uint16_t COLOR_GOOD = 0x4CD9;
const uint16_t COLOR_WARNING = 0xFDC0;
const uint16_t COLOR_DANGER = 0xF3A6;
const uint16_t COLOR_BG = 0x0000;
const uint16_t COLOR_TEXT = 0xFFFF;
const uint16_t COLOR_SECONDARY = 0x8410;
const uint16_t COLOR_ACCENT = 0x2D5F;

// ===========================
// グローバル変数
// ===========================
// 前回のCO2値や更新時刻など
uint16_t prev_co2 = 0;
unsigned long lastUpdateTime = 0;

// ===========================
// 堅牢性管理用変数
// ===========================
unsigned long bootTime = 0;          // 起動時刻
unsigned long lastWatchdogReset = 0; // 最後のウォッチドッグリセット時刻
unsigned long lastWiFiCheck = 0;     // 最後のWiFi状態確認時刻
unsigned long lastMQTTCheck = 0;     // 最後のMQTT状態確認時刻
unsigned long lastMemoryCheck = 0;   // 最後のメモリ確認時刻
int wifiRetryCount = 0;              // WiFi再接続試行回数
int mqttRetryCount = 0;              // MQTT再接続試行回数
int sensorErrorCount = 0;            // センサー連続エラー回数
bool systemHealthy = true;           // システム健全性フラグ

// ===========================
// 堅牢性関数群
// ===========================

// ウォッチドッグタイマーのリセット
void resetWatchdog()
{
  if (ENABLE_WATCHDOG)
  {
    lastWatchdogReset = millis();
    // ESP32のウォッチドッグをソフトウェアでリセット
    // 代替案：定期的な状態チェックで異常検知
  }
}

// メモリ使用量チェック
bool checkMemoryHealth()
{
  size_t freeHeap = ESP.getFreeHeap();
  if (freeHeap < MIN_FREE_HEAP)
  {
    Serial.printf("WARNING: Low memory! Free heap: %d bytes\n", freeHeap);
    return false;
  }
  return true;
}

// システム統計情報を送信
void sendSystemStats()
{
  size_t freeHeap = ESP.getFreeHeap();
  unsigned long uptime = millis() - bootTime;
  time_t now = time(nullptr);

  char payload[200];
  snprintf(payload, sizeof(payload),
           "{\"type\":\"system\",\"timestamp\":%ld,\"uptime\":%lu,\"free_heap\":%d,\"wifi_rssi\":%d,\"sensor_errors\":%d}",
           now, uptime / 1000, freeHeap, WiFi.RSSI(), sensorErrorCount);

  if (client.connected())
  {
    String systemTopic = String(mqtt_topic) + "/system";
    client.publish(systemTopic.c_str(), payload);
  }
}

// 緊急時の安全な再起動
void safeRestart(const char *reason)
{
  Serial.printf("RESTART: %s\n", reason);

  // 緊急ログを送信
  if (client.connected())
  {
    time_t now = time(nullptr);
    char payload[150];
    snprintf(payload, sizeof(payload), "{\"type\":\"restart\",\"timestamp\":%ld,\"reason\":\"%s\"}", now, reason);
    String alertTopic = String(mqtt_topic) + "/alert";
    client.publish(alertTopic.c_str(), payload);
    client.loop();
    delay(1000); // 送信完了を待つ
  }

  // LCD表示
  M5.Lcd.fillScreen(COLOR_DANGER);
  M5.Lcd.setTextSize(2);
  M5.Lcd.setTextColor(COLOR_TEXT);
  M5.Lcd.setCursor(10, 30);
  M5.Lcd.println("RESTARTING...");
  M5.Lcd.setCursor(10, 50);
  M5.Lcd.println(reason);
  delay(3000);

  ESP.restart();
}

// WiFi接続状態の監視と復旧
bool maintainWiFiConnection()
{
  if (WiFi.status() != WL_CONNECTED)
  {
    if (millis() - lastWiFiCheck > WIFI_RETRY_INTERVAL)
    {
      lastWiFiCheck = millis();
      wifiRetryCount++;

      Serial.printf("WiFi disconnected. Retry attempt: %d/%d\n", wifiRetryCount, MAX_WIFI_RETRY);

      if (wifiRetryCount <= MAX_WIFI_RETRY)
      {
        WiFi.disconnect();
        delay(1000);
        WiFi.begin(ssid, password);
        return false;
      }
      else
      {
        safeRestart("WiFi connection failed");
        return false;
      }
    }
    return false;
  }
  else
  {
    wifiRetryCount = 0; // 接続成功でカウンターリセット
    return true;
  }
}

// MQTT接続状態の監視と復旧
bool maintainMQTTConnection()
{
  if (!client.connected())
  {
    if (millis() - lastMQTTCheck > MQTT_RETRY_INTERVAL)
    {
      lastMQTTCheck = millis();
      mqttRetryCount++;

      Serial.printf("MQTT disconnected. Retry attempt: %d/%d\n", mqttRetryCount, MAX_MQTT_RETRY);

      if (mqttRetryCount <= MAX_MQTT_RETRY)
      {
        if (client.connect(mqtt_client_id))
        {
          Serial.println("MQTT reconnected");
          mqttRetryCount = 0;
          return true;
        }
        return false;
      }
      else
      {
        safeRestart("MQTT connection failed");
        return false;
      }
    }
    return false;
  }
  else
  {
    mqttRetryCount = 0;
    return true;
  }
}

// センサー健全性チェック
bool checkSensorHealth()
{
  unsigned long currentTime = millis();

  // センサーデータ取得タイムアウトチェック
  if (currentTime - lastUpdateTime > SENSOR_TIMEOUT)
  {
    sensorErrorCount++;
    Serial.printf("Sensor timeout. Error count: %d/%d\n", sensorErrorCount, MAX_SENSOR_ERROR_COUNT);

    if (sensorErrorCount >= MAX_SENSOR_ERROR_COUNT)
    {
      safeRestart("Sensor not responding");
      return false;
    }
  }
  else
  {
    sensorErrorCount = 0; // データ取得成功でカウンターリセット
  }

  return true;
}

// 定期的なシステムヘルスチェック
void performHealthCheck()
{
  unsigned long currentTime = millis();

  // 5分ごとにヘルスチェック実行
  static unsigned long lastHealthCheck = 0;
  if (currentTime - lastHealthCheck > 5 * 60 * 1000)
  {
    lastHealthCheck = currentTime;

    bool memoryOK = checkMemoryHealth();
    bool wifiOK = maintainWiFiConnection();
    bool mqttOK = maintainMQTTConnection();
    bool sensorOK = checkSensorHealth();

    systemHealthy = memoryOK && wifiOK && mqttOK && sensorOK;

    // システム統計を送信
    sendSystemStats();

    Serial.printf("Health Check - Memory:%s WiFi:%s MQTT:%s Sensor:%s\n",
                  memoryOK ? "OK" : "NG",
                  wifiOK ? "OK" : "NG",
                  mqttOK ? "OK" : "NG",
                  sensorOK ? "OK" : "NG");
  }

  // 自動再起動チェック
  if (ENABLE_AUTO_RESTART && (currentTime - bootTime > AUTO_RESTART_INTERVAL))
  {
    safeRestart("Scheduled restart");
  }
}

// ===========================
// WiFi接続関数
// ===========================
// WiFiに接続し、NTPサーバーから時刻を取得する
void setupWiFi()
{
  M5.Lcd.setTextSize(1);
  M5.Lcd.setTextColor(COLOR_TEXT);
  M5.Lcd.setCursor(10, 10);
  M5.Lcd.print("Connecting to WiFi");

  WiFi.begin(ssid, password);
  int attempts = 0;

  // 最大20回（約10秒）まで接続を試みる
  while (WiFi.status() != WL_CONNECTED && attempts < 20)
  {
    delay(500);
    M5.Lcd.print(".");
    attempts++;
  }

  if (WiFi.status() == WL_CONNECTED)
  {
    // WiFi接続成功
    M5.Lcd.setTextColor(COLOR_GOOD);
    M5.Lcd.println("\nConnected!");

    // NTPサーバーから時刻を取得
    configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);

    struct tm timeinfo;
    if (getLocalTime(&timeinfo))
    {
      // 時刻取得成功
      M5.Lcd.setTextColor(COLOR_ACCENT);
      M5.Lcd.printf("\nTime sync: %02d:%02d:%02d",
                    timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);
    }
    else
    {
      // 時刻取得失敗
      M5.Lcd.setTextColor(COLOR_WARNING);
      M5.Lcd.println("\nFailed to get time");
    }
  }
  else
  {
    // WiFi接続失敗
    M5.Lcd.setTextColor(COLOR_DANGER);
    M5.Lcd.println("\nWiFi connection failed");
  }

  delay(1000); // 表示が見えるように少し待つ
}

// ===========================
// MQTT再接続関数
// ===========================
// MQTTブローカーに再接続する
void reconnect()
{
  while (!client.connected())
  {
    Serial.print("Attempting MQTT connection...");

    // MQTTサーバーに接続
    if (client.connect(mqtt_client_id))
    {
      Serial.println("connected");
    }
    else
    {
      Serial.print("failed, rc=");
      Serial.print(client.state());
      Serial.println(" retrying in 5 seconds");
      delay(5000); // 5秒後に再試行
    }
  }
}

// ===========================
// センサーデータ送信関数
// ===========================
// 現在の時刻・CO2・温度・湿度をMQTTで送信
void sendSensorData()
{
  time_t now = time(nullptr);

  // 時刻が正しく取得できているかチェック
  if (now < 100000)
  {
    Serial.println("WARNING: Time not synchronized, skipping MQTT send");
    return;
  }

  // センサーデータが有効かチェック
  if (current_co2 == 0 || current_co2 > 50000)
  {
    Serial.printf("WARNING: Invalid CO2 value: %d, skipping MQTT send\n", current_co2);
    return;
  }

  // JSON形式でデータを作成（UNIXタイムスタンプを使用）
  char payload[200];
  int len = snprintf(payload, sizeof(payload),
                     "{\"timestamp\":%ld,\"co2\":%u,\"temp\":%.1f,\"hum\":%.1f}",
                     (long)now, (unsigned int)current_co2, current_temp, current_humidity);

  // JSONが正しく作成されたかチェック
  if (len >= sizeof(payload))
  {
    Serial.println("ERROR: JSON payload too large");
    return;
  }

  // デバッグ用：送信するJSONを表示
  Serial.printf("Sending MQTT: %s\n", payload);

  // MQTTで送信
  if (client.publish(mqtt_topic, payload))
  {
    Serial.println("MQTT publish succeeded");
  }
  else
  {
    Serial.println("MQTT publish failed");
  }
}

// ===========================
// CO2値を中央に大きく表示
// ===========================
void drawCenteredCO2(uint16_t co2, uint16_t color)
{
  int screenWidth = M5.Lcd.width();
  int screenHeight = M5.Lcd.height();
  String co2Str = String(co2);
  int digits = co2Str.length();
  int charWidth = 8 * 6; // フォントサイズ8の1文字幅
  int totalWidth = charWidth * digits;
  int xPos = (screenWidth - totalWidth) / 2;
  int yPos = (screenHeight - 8 * 8) / 2;

  M5.Lcd.setTextSize(8);
  M5.Lcd.setTextColor(color);
  M5.Lcd.setCursor(xPos, yPos);
  M5.Lcd.print(co2Str);
}

// ===========================
// 温度・湿度を下部に表示
// ===========================
void displayEnvironmentalData(float temperature, float humidity)
{
  // 下線を描画
  M5.Lcd.drawFastHLine(10, M5.Lcd.height() - 25, M5.Lcd.width() - 20, COLOR_SECONDARY);
  M5.Lcd.setTextSize(1);
  M5.Lcd.setTextColor(COLOR_TEXT);
  M5.Lcd.setCursor(15, M5.Lcd.height() - 15);
  M5.Lcd.print("T:");
  M5.Lcd.setTextColor(COLOR_ACCENT);
  M5.Lcd.setCursor(25, M5.Lcd.height() - 15);
  M5.Lcd.printf("%.1fC", temperature);
  M5.Lcd.setTextColor(COLOR_TEXT);
  M5.Lcd.setCursor(70, M5.Lcd.height() - 15);
  M5.Lcd.print("|");
  M5.Lcd.setCursor(80, M5.Lcd.height() - 15);
  M5.Lcd.print("H:");
  M5.Lcd.setTextColor(COLOR_ACCENT);
  M5.Lcd.setCursor(90, M5.Lcd.height() - 15);
  M5.Lcd.printf("%.1f%%", humidity);
}

// ===========================
// ステータスバー（上部バー）
// ===========================
void displayStatusBar()
{
  M5.Lcd.fillRect(0, 0, M5.Lcd.width(), 12, COLOR_BG);
  M5.Lcd.setTextSize(1);
  M5.Lcd.setTextColor(COLOR_SECONDARY);
  M5.Lcd.setCursor(5, 3);
  M5.Lcd.print("CO2");

  struct tm timeinfo;
  if (getLocalTime(&timeinfo))
  {
    M5.Lcd.setCursor(M5.Lcd.width() - 45, 3);
    M5.Lcd.printf("%02d:%02d", timeinfo.tm_hour, timeinfo.tm_min);
  }
}

// ===========================
// 初期化処理
// ===========================
void setup()
{
  // 起動時刻記録
  bootTime = millis();

  auto cfg = M5.config();
  M5.begin(cfg);               // M5StickC Plus2の初期化
  Serial.begin(115200);        // シリアルモニタの初期化
  M5.Lcd.setRotation(3);       // 画面を横向きに設定
  M5.Lcd.fillScreen(COLOR_BG); // 画面を黒でクリア
  M5.Lcd.setTextSize(2);
  M5.Lcd.setTextColor(COLOR_TEXT);

  // 起動情報を表示
  Serial.println("=== M5StickC Plus2 CO2 Monitor ===");
  Serial.printf("Boot time: %lu\n", bootTime);
  Serial.printf("Free heap: %d bytes\n", ESP.getFreeHeap());
  Serial.printf("Chip model: %s\n", ESP.getChipModel());
  Serial.printf("CPU frequency: %d MHz\n", ESP.getCpuFreqMHz());

  int centerX = M5.Lcd.width() / 2;
  int centerY = M5.Lcd.height() / 2;

  // タイトル表示
  M5.Lcd.setTextSize(2);
  M5.Lcd.setCursor(centerX - 70, centerY - 30);
  M5.Lcd.setTextColor(COLOR_ACCENT);
  M5.Lcd.println("CO2 Monitor");

  // バージョン・堅牢性情報表示
  M5.Lcd.setTextSize(1);
  M5.Lcd.setTextColor(COLOR_SECONDARY);
  M5.Lcd.setCursor(centerX - 45, centerY);
  M5.Lcd.println("Robust v2.0");
  M5.Lcd.setCursor(centerX - 45, centerY + 10);
  M5.Lcd.println("Initializing...");

  // WiFiとNTP時刻取得
  setupWiFi();

  // NTP時刻同期の確認
  Serial.print("Waiting for NTP sync...");
  time_t now = time(nullptr);
  int sync_attempts = 0;
  while (now < 100000 && sync_attempts < 10)
  {
    delay(1000);
    now = time(nullptr);
    sync_attempts++;
    Serial.print(".");
  }
  if (now >= 100000)
  {
    Serial.printf("\nNTP sync successful. Current timestamp: %ld\n", (long)now);
  }
  else
  {
    Serial.println("\nWARNING: NTP sync failed!");
  }

  // I2C通信の初期化（SDA:32, SCL:33）
  Wire.begin(32, 33);

  // CO2センサー初期化
  M5.Lcd.fillScreen(COLOR_BG);
  M5.Lcd.setCursor(10, 10);
  M5.Lcd.println("Initializing CO2 sensor...");
  Serial.println("CO2 Sensor Initializing...");

  if (!scd4x.begin(&Wire, 0x62, 32, 33, 100000U))
  {
    M5.Lcd.setTextColor(COLOR_DANGER);
    M5.Lcd.setCursor(10, 30);
    M5.Lcd.println("Sensor not found!");
    while (1)
      ; // センサーがなければ停止
  }

  M5.Lcd.setTextColor(COLOR_SECONDARY);
  M5.Lcd.setCursor(10, 30);
  M5.Lcd.print("Starting");

  // 測定を一度停止してから再スタート
  uint16_t error = scd4x.stopPeriodicMeasurement();
  delay(500);
  error = scd4x.startPeriodicMeasurement();

  // プログレスアニメーション
  for (int i = 0; i < 5; i++)
  {
    M5.Lcd.setCursor(70 + i * 6, 30);
    M5.Lcd.print(".");
    delay(500);
  }

  M5.Lcd.setTextColor(COLOR_GOOD);
  M5.Lcd.setCursor(10, 50);
  M5.Lcd.println("Ready! Starting...");
  delay(1000);

  // MQTTクライアント初期化
  client.setServer(mqtt_server, mqtt_port);
  client.setKeepAlive(60); // キープアライブ間隔60秒

  // 堅牢性機能の初期化
  lastUpdateTime = millis();
  lastWatchdogReset = millis();
  lastWiFiCheck = millis();
  lastMQTTCheck = millis();
  lastMemoryCheck = millis();

  // 起動完了ログ
  Serial.println("System initialization completed");
  Serial.printf("Robust features enabled - Auto restart: %s, Watchdog: %s\n",
                ENABLE_AUTO_RESTART ? "ON" : "OFF",
                ENABLE_WATCHDOG ? "ON" : "OFF");
}

// ===========================
// メインループ
// ===========================
void loop()
{
  unsigned long currentTime = millis();

  // 堅牢性チェック（最優先）
  performHealthCheck();
  resetWatchdog();

  // WiFi・MQTT接続維持
  if (!maintainWiFiConnection() || !maintainMQTTConnection())
  {
    // 接続に問題がある場合は短いディレイで次のループへ
    delay(1000);
    return;
  }

  // センサーからデータ取得できた場合
  if (scd4x.update())
  {
    lastUpdateTime = currentTime;

    // センサーデータを一時変数に取得
    uint16_t temp_co2 = scd4x.getCO2();
    float temp_temp = scd4x.getTemperature();
    float temp_humidity = scd4x.getHumidity();

    // データ範囲チェック（異常値検知）
    bool data_valid = true;
    if (temp_co2 > 10000 || temp_co2 < 300)
    {
      Serial.printf("WARNING: Abnormal CO2 value: %d ppm\n", temp_co2);
      sensorErrorCount++;
      data_valid = false;
    }
    else if (temp_temp > 60.0 || temp_temp < -20.0)
    {
      Serial.printf("WARNING: Abnormal temperature: %.1f°C\n", temp_temp);
      sensorErrorCount++;
      data_valid = false;
    }
    else if (temp_humidity > 100.0 || temp_humidity < 0.0)
    {
      Serial.printf("WARNING: Abnormal humidity: %.1f%%\n", temp_humidity);
      sensorErrorCount++;
      data_valid = false;
    }

    // データが有効な場合のみ更新
    if (data_valid)
    {
      current_co2 = temp_co2;
      current_temp = temp_temp;
      current_humidity = temp_humidity;
      sensorErrorCount = 0; // 正常値でエラーカウンターリセット
    }

    // シリアルモニタに出力（デバッグ用）
    Serial.println();
    Serial.print(F("CO2(ppm): "));
    Serial.print(current_co2);
    Serial.print(F("\tTemperature(C): "));
    Serial.print(current_temp, 1);
    Serial.print(F("\tHumidity(%RH): "));
    Serial.print(current_humidity, 1);
    Serial.printf(F("\tUptime: %lu s\tFree heap: %d\n"),
                  (currentTime - bootTime) / 1000, ESP.getFreeHeap());

    // 画面クリア
    M5.Lcd.fillScreen(COLOR_BG);
    // ステータスバーとセンサーデータを表示
    displayStatusBar();

    uint16_t textColor;
    if (current_co2 < CO2_GOOD)
    {
      textColor = COLOR_GOOD;
    }
    else if (current_co2 < CO2_WARNING)
    {
      textColor = COLOR_WARNING;
    }
    else
    {
      textColor = COLOR_DANGER;
    }

    // CO2値を中央に大きく表示
    drawCenteredCO2(current_co2, textColor);
    // 温度・湿度を下部に表示
    displayEnvironmentalData(current_temp, current_humidity);

    // システム健全性インジケーター
    if (!systemHealthy)
    {
      M5.Lcd.fillCircle(M5.Lcd.width() - 10, 10, 3, COLOR_DANGER);
    }

    prev_co2 = current_co2;
  }
  else
  {
    // データ取得できない場合は「Reading...」を点滅表示
    if (currentTime - lastUpdateTime > 5000)
    {
      if ((currentTime / 500) % 2)
      {
        M5.Lcd.setTextColor(COLOR_SECONDARY);
        M5.Lcd.setCursor(M5.Lcd.width() / 2 - 45, M5.Lcd.height() / 2);
        M5.Lcd.print("Reading...");

        // システム状態も表示
        M5.Lcd.setTextSize(1);
        M5.Lcd.setCursor(10, M5.Lcd.height() - 30);
        M5.Lcd.printf("Errors: %d", sensorErrorCount);
      }
      else
      {
        M5.Lcd.fillRect(M5.Lcd.width() / 2 - 45, M5.Lcd.height() / 2, 90, 10, COLOR_BG);
        M5.Lcd.fillRect(10, M5.Lcd.height() - 30, 100, 10, COLOR_BG);
      }
    }
    Serial.print(F("."));
  }

  // MQTT通信管理
  client.loop();

  // 1分ごとにデータ送信（センサーデータが有効な場合のみ）
  if (currentTime - lastMqttSend >= mqtt_interval && current_co2 > 0)
  {
    sendSensorData();
    lastMqttSend = currentTime;
  }

  // M5StickCのボタンやセンサーの状態を更新
  M5.update();

  delay(100); // 100msごとにループ
}
