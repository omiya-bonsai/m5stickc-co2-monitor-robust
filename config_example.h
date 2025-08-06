#ifndef CONFIG_H
#define CONFIG_H

// ===========================
// WiFi設定
// ===========================
// WiFiのSSIDとパスワードをここで指定します
const char *ssid = "YOUR_WIFI_SSID";          // あなたのWiFi SSIDに変更
const char *password = "YOUR_WIFI_PASSWORD";  // あなたのWiFiパスワードに変更

// ===========================
// MQTTブローカー設定
// ===========================
// MQTTサーバーのIPアドレスやトピックなどを指定します
const char *mqtt_server = "192.168.1.100";            // あなたのMQTTサーバーIPに変更
const int mqtt_port = 1883;                           // MQTTのデフォルトポート
const char *mqtt_client_id = "M5StickC_CO2_Monitor";  // 必要に応じて変更
const char *mqtt_topic = "m5stickc_co2/co2_data";     // 必要に応じて変更

// ===========================
// 堅牢性設定（24/7運用向け）
// ===========================
// 自動再起動設定
const unsigned long AUTO_RESTART_INTERVAL = 24 * 60 * 60 * 1000UL;  // 24時間ごとに再起動（ミリ秒）
const bool ENABLE_AUTO_RESTART = true;                              // 自動再起動を有効にする

// ウォッチドッグタイマー設定
const unsigned long WATCHDOG_TIMEOUT = 30 * 1000;  // 30秒でタイムアウト
const bool ENABLE_WATCHDOG = true;                 // ウォッチドッグタイマーを有効にする

// 接続リトライ設定
const int MAX_WIFI_RETRY = 3;                         // WiFi接続最大リトライ回数
const int MAX_MQTT_RETRY = 5;                         // MQTT接続最大リトライ回数
const unsigned long WIFI_RETRY_INTERVAL = 30 * 1000;  // WiFi再接続間隔（ミリ秒）
const unsigned long MQTT_RETRY_INTERVAL = 10 * 1000;  // MQTT再接続間隔（ミリ秒）

// センサー異常検知設定
const unsigned long SENSOR_TIMEOUT = 60 * 1000;  // センサーデータ取得タイムアウト（1分）
const int MAX_SENSOR_ERROR_COUNT = 10;           // 連続エラー許容回数

// メモリ監視設定
const unsigned long MIN_FREE_HEAP = 10000;  // 最低空きヒープメモリ（バイト）

#endif  // CONFIG_H
