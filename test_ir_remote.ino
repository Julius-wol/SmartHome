// ESP32 UART2 + WiFi + ThingsBoard (Low Power Mode)
#include <WiFi.h>
#include <PubSubClient.h>

// ==================== Cấu hình UART2 ====================
#define RXD2 16  // GPIO16 - UART2 RX
#define TXD2 17  // GPIO17 - UART2 TX

// ==================== Cấu hình WiFi ====================
#define SSID "IOT-12345"
#define PASSWORD "12345678"

// ==================== Cấu hình ThingsBoard ====================
#define THINGSBOARD_SERVER "thingsboard.cloud"
#define THINGSBOARD_PORT 1883
#define ACCESS_TOKEN "uZP5nh8HQNtsN2STUAfg"

// ==================== Biến toàn cục ====================
WiFiClient espClient;
PubSubClient client(espClient);

String dataBuffer = "";
unsigned long lastSend = 0;
unsigned long lastWiFiCheck = 0;
unsigned long lastAttributeRequest = 0;
unsigned long sendInterval = 10000;              // Gửi mỗi 10 giây
unsigned long attributeRequestInterval = 30000;  // Request mỗi 30 giây
boolean wifiConnected = false;

// ==================== Cấu trúc dữ liệu ====================
struct SensorData {
  float humidity;
  float temperature;
  int gas;
  int rain;
  bool gas_detect;
  bool rain_detect;
};

struct DeviceControl {
  bool fan;
  bool door;
  bool windows;
  bool lcd;
};

SensorData sensorData;
DeviceControl deviceControl = {false, false, false, false};

// ==================== Forward Declarations ====================
void setup();
void loop();
void initPowerSaving();
void initWiFi();
void parseAndProcessData(String data);
void reconnectMQTT();
void requestSharedAttributes();
void sendDataToThingsBoard();
void sendControlCommand();
void mqttCallback(char* topic, byte* payload, unsigned int length);

// ==================== Setup ====================
void setup() {
  Serial.begin(115200);
  delay(2000);

  Serial.println("\n\n=== ESP32 UART2 + ThingsBoard (Low Power) ===");

  // Power saving setup
  initPowerSaving();

  // Khởi tạo UART2
  Serial2.begin(9600, SERIAL_8N1, RXD2, TXD2);
  Serial.println("UART2 initialized (GPIO16 RX, GPIO17 TX, 9600 baud)");
  delay(1000);

  // WiFi setup
  initWiFi();

  // MQTT setup
  client.setServer(THINGSBOARD_SERVER, THINGSBOARD_PORT);
  client.setCallback(mqttCallback);

  Serial.println("Setup completed!\n");
}

// ==================== Power Saving Init ====================
void initPowerSaving() {
  // Giảm CPU từ 240MHz → 80MHz
  setCpuFrequencyMhz(80);
  Serial.print("CPU: ");
  Serial.print(getCpuFrequencyMhz());
  Serial.println(" MHz");

  // Tắt Bluetooth
  btStop();

  Serial.println("Power saving enabled\n");
}

// ==================== WiFi Init ====================
void initWiFi() {
  // Giảm TX power
  WiFi.setTxPower(WIFI_POWER_7dBm);

  WiFi.mode(WIFI_OFF);
  delay(500);

  Serial.print("Connecting to WiFi ");
  WiFi.mode(WIFI_STA);
  WiFi.begin(SSID, PASSWORD);

  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 15) {
    delay(1000);
    Serial.print(".");
    attempts++;
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("✓ WiFi Connected!");
    Serial.print("IP: ");
    Serial.println(WiFi.localIP());
    wifiConnected = true;
  } else {
    Serial.println("✗ WiFi connection failed - running in UART mode");
    wifiConnected = false;
  }
}

// ==================== Main Loop ====================
void loop() {
  // Đọc dữ liệu từ UART2
  while (Serial2.available()) {
    char ch = Serial2.read();

    if (ch == '\n') {
      if (dataBuffer.length() > 0) {
        parseAndProcessData(dataBuffer);
        dataBuffer = "";
      }
    } else if (ch != '\r') {
      dataBuffer += ch;
    }
  }

  // Kiểm tra WiFi định kỳ
  if (millis() - lastWiFiCheck > 30000) {  // Check mỗi 30 giây
    if (WiFi.status() != WL_CONNECTED) {
      Serial.println("WiFi disconnected! Reconnecting...");
      WiFi.begin(SSID, PASSWORD);
      wifiConnected = false;
    } else {
      if (!wifiConnected) {
        Serial.println("WiFi reconnected!");
        wifiConnected = true;
      }
    }
    lastWiFiCheck = millis();
  }

  // MQTT - chỉ nếu WiFi có kết nối
  if (wifiConnected) {
    if (!client.connected()) {
      reconnectMQTT();
    }
    client.loop();

    // Gửi dữ liệu định kỳ
    if (millis() - lastSend > sendInterval) {
      sendDataToThingsBoard();
      lastSend = millis();
    }

    // Request shared attributes định kỳ
    if (millis() - lastAttributeRequest > attributeRequestInterval) {
      requestSharedAttributes();
      lastAttributeRequest = millis();
    }
  }

  delay(100);
}

// ==================== Parse dữ liệu ====================
void parseAndProcessData(String data) {
  Serial.print("Received: ");
  Serial.println(data);

  // Ví dụ: H:47.00,T:28.00,G:106,R:1021

  // Parse Humidity
  int hIndex = data.indexOf("H:");
  if (hIndex != -1) {
    int hEnd = data.indexOf(",", hIndex);
    if (hEnd == -1) hEnd = data.length();
    String hStr = data.substring(hIndex + 2, hEnd);
    sensorData.humidity = hStr.toFloat();
  }

  // Parse Temperature
  int tIndex = data.indexOf("T:");
  if (tIndex != -1) {
    int tEnd = data.indexOf(",", tIndex);
    if (tEnd == -1) tEnd = data.length();
    String tStr = data.substring(tIndex + 2, tEnd);
    sensorData.temperature = tStr.toFloat();
  }

  // Parse Gas
  int gIndex = data.indexOf("G:");
  if (gIndex != -1) {
    int gEnd = data.indexOf(",", gIndex);
    if (gEnd == -1) gEnd = data.length();
    String gStr = data.substring(gIndex + 2, gEnd);
    sensorData.gas = gStr.toInt();
    sensorData.gas_detect = (sensorData.gas > 500);
  }

  // Parse Rain
  int rIndex = data.indexOf("R:");
  if (rIndex != -1) {
    int rEnd = data.indexOf(",", rIndex);
    if (rEnd == -1) rEnd = data.length();
    String rStr = data.substring(rIndex + 2, rEnd);
    sensorData.rain = rStr.toInt();
    sensorData.rain_detect = (sensorData.rain > 500);
  }

  // In thông tin đã parse
  Serial.println("Data:");
  Serial.print("  H:");
  Serial.print(sensorData.humidity);
  Serial.print("% T:");
  Serial.print(sensorData.temperature);
  Serial.print("°C G:");
  Serial.print(sensorData.gas);
  Serial.print(" R:");
  Serial.println(sensorData.rain);
}

// ==================== Kết nối MQTT ====================
void reconnectMQTT() {
  if (!wifiConnected) return;

  static int attempts = 0;
  if (attempts > 0) return;  // Chỉ thử 1 lần mỗi cycle

  Serial.print("MQTT: Connecting...");
  if (client.connect("ESP32_IoT", ACCESS_TOKEN, "")) {
    Serial.println(" OK");
    attempts = 0;

    // Subscribe to shared attributes response
    String topic = "v1/devices/me/attributes";
    client.subscribe(topic.c_str());
    Serial.println("Subscribed to shared attributes");

    // Request shared attributes
    delay(500);
    requestSharedAttributes();

  } else {
    Serial.print(" FAIL (");
    Serial.print(client.state());
    Serial.println(")");
    attempts++;
  }
}

// ==================== Callback MQTT ====================
void mqttCallback(char* topic, byte* payload, unsigned int length) {
  // Convert payload to string
  char payloadStr[512];
  if (length < sizeof(payloadStr)) {
    memcpy(payloadStr, payload, length);
    payloadStr[length] = '\0';

    Serial.print("MQTT RX on topic: ");
    Serial.println(topic);
    Serial.print("Payload: ");
    Serial.println(payloadStr);

    // Parse shared attributes
    if (strstr(topic, "/attributes") != NULL) {
      parseSharedAttributes(payloadStr);
    }
  }
}

// ==================== Parse Shared Attributes ====================
void parseSharedAttributes(char* payload) {
  // Tìm "fan"
  char* fanPtr = strstr(payload, "\"fan\":");
  if (fanPtr != NULL) {
    deviceControl.fan = (strstr(fanPtr, "true") != NULL);
  }

  // Tìm "door"
  char* doorPtr = strstr(payload, "\"door\":");
  if (doorPtr != NULL) {
    deviceControl.door = (strstr(doorPtr, "true") != NULL);
  }

  // Tìm "windows"
  char* windowsPtr = strstr(payload, "\"windows\":");
  if (windowsPtr != NULL) {
    deviceControl.windows = (strstr(windowsPtr, "true") != NULL);
  }

  // Tìm "lcd"
  char* lcdPtr = strstr(payload, "\"lcd\":");
  if (lcdPtr != NULL) {
    deviceControl.lcd = (strstr(lcdPtr, "true") != NULL);
  }
  
  Serial.print("Control: F:");
  Serial.print(deviceControl.fan ? "1" : "0");
  Serial.print(" D:");
  Serial.print(deviceControl.door ? "1" : "0");
  Serial.print(" W:");
  Serial.print(deviceControl.windows ? "1" : "0");
  Serial.print(" L:");
  Serial.println(deviceControl.lcd ? "1" : "0");
  
  // Gửi lệnh điều khiển
  sendControlCommand();
}

// ==================== Send Control Command via UART2 ====================
void sendControlCommand() {
  // Format: F:1,W:0,D:1,L:0\n
  String command = "F:";
  command += (deviceControl.fan ? "1" : "0");
  command += ",W:";
  command += (deviceControl.windows ? "1" : "0");
  command += ",D:";
  command += (deviceControl.door ? "1" : "0");
  command += ",L:";
  command += (deviceControl.lcd ? "1" : "0");
  command += "\n";

  Serial2.print(command);

  Serial.print("UART2 TX: ");
  Serial.print(command);
}

// ==================== Request Shared Attributes ====================
void requestSharedAttributes() {
  if (!client.connected() || !wifiConnected) return;
  
  String topic = "v1/devices/me/attributes/request/1";
  String payload = "{\"clientKeys\": \"fan,door,windows,lcd\"}";
  
  if (client.publish(topic.c_str(), payload.c_str())) {
    Serial.println("Request: Shared attributes");
  }
}

// ==================== Gửi dữ liệu lên ThingsBoard ====================
void sendDataToThingsBoard() {
  if (!client.connected() || !wifiConnected) {
    return;
  }

  // Tạo JSON payload
  String payload = "";
  payload += "{";
  payload += "\"humidity\":" + String(sensorData.humidity, 2) + ",";
  payload += "\"temperature\":" + String(sensorData.temperature, 2) + ",";
  payload += "\"gas\":" + String(sensorData.gas) + ",";
  payload += "\"gas_detect\":" + String(sensorData.gas_detect ? "true" : "false") + ",";
  payload += "\"rain\":" + String(sensorData.rain) + ",";
  payload += "\"rain_detect\":" + String(sensorData.rain_detect ? "true" : "false");
  payload += "}";

  // Gửi qua MQTT
  if (client.publish("v1/devices/me/telemetry", payload.c_str())) {
    Serial.print("ThingsBoard TX: ");
    Serial.println(payload);
  } else {
    Serial.println("ThingsBoard TX: FAIL");
  }
}
