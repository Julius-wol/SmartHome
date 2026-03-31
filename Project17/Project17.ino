#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <IRremote.h>
#include <SPI.h>
#include <RFID.h>
#include <Servo.h>
#include <dht.h>

// Sử dụng Serial Hardware (D0=RX, D1=TX) 
// CHÚ Ý: Khi upload code, ngắt kết nối ESP32 khỏi D0-D1
// Sau upload, kết nối lại ESP32 vào D0-D1



// publish data: DHT11: Humidity, Temperature; MQ-2: Gas Level; Rain: Rain Level to ESP32
//  Control  Fan, Door, Window, LCD Backlight via Remote and RFID Card


// --- KHAI BÁO CHÂN THEO ĐÚNG SƠ ĐỒ ---
RFID rfid(10, 9);    // D10: SDA, D9: RST
#define LED_PIN 8    // Module LED vàng chiếu sáng
#define KEY_2 A7     // Nút nhấn màu vàng
#define buzzer 15    // Chân A1 - Còi báo động (Kích mức LOW)
#define smokeA0 A0   // Cảm biến Gas MQ-2
#define DHT11_PIN 4  // Cảm biến nhiệt độ, độ ẩm DHT11
#define IR_PIN 2     // Mắt thu tín hiệu Remote hồng ngoại
#define RelayPin 16  // Chân A2 - Relay (Kích mức LOW)     // check pinout
#define FanPinA 7    // Quạt hút khí gas
#define Rain A3      // Cảm biến giọt nước (Mưa)

// --- MÃ REMOTE HỒNG NGOẠI ---
#define KEY11 16753245  // Bật đèn màn hình LCD
#define KEY22 16736925  // Tắt đèn màn hình LCD
#define KEY33 16769565  // Bật Relay và Quạt
#define KEY44 16720605  // Tắt Relay và Quạt
#define KEY55 16712445  // Mở cửa sổ mái nhà
#define KEY66 16761405  // Đóng cửa sổ mái nhà
#define KEY77 16769055  // Mở cửa chính
#define KEY88 16754775  // Đóng cửa chính
#define KEY00 16750695  // Bật chế độ tự động đóng cửa khi trời mưa

// --- KHỞI TẠO BIẾN & THIẾT BỊ ---
LiquidCrystal_I2C lcd(0x27, 16, 2);  // Màn hình LCD 16 cột 2 dòng
IRrecv irrecv(IR_PIN);
decode_results results;
dht DHT;

Servo lockServo;  // Servo cửa chính (Chân 3)
Servo myservo;    // Servo cửa sổ mái nhà (Chân 6)

// Biến nhận dữ liệu điều khiển từ ESP32
boolean fanControl = false;    // Điều khiển quạt (true = BÂT, false = TẮT)
boolean windowControl = false; // Điều khiển cửa sổ (true = MỞ, false = ĐÓNG)
boolean doorControl = false;   // Điều khiển cửa (true = MỞ, false = ĐÓNG)
boolean lcdBacklight = true;   // Điều khiển LCD backlight

unsigned char str[MAX_LEN];
String accessGranted[2] = { "2394121306", "96111131033" };  // Mã thẻ từ được phép mở cửa
int accessGrantedSize = 2;

int lockPos = 0;     // Góc khóa cửa
int unlockPos = 90;  // Góc mở cửa
boolean locked = true;
int rain;
int RainFlag = 0;

int KEY_BIT = 1, LED_BIT = 0;
int sensorThres = 300;  // Ngưỡng báo cháy (Không khí bình thường ~ 160)

void setup() {
  Serial.begin(9600);  // Serial Hardware (D0=RX, D1=TX) cho ESP32

  pinMode(KEY_2, INPUT);
  pinMode(LED_PIN, OUTPUT);
  pinMode(buzzer, OUTPUT);
  pinMode(smokeA0, INPUT);
  pinMode(FanPinA, OUTPUT);
  pinMode(RelayPin, OUTPUT);
  pinMode(Rain, INPUT);

  // KHỞI ĐỘNG CÁC THIẾT BỊ OUTPUT Ở TRẠNG THÁI TẮT AN TOÀN
  digitalWrite(RelayPin, HIGH);  // Rơ-le kích mức LOW -> Để HIGH để tắt
  digitalWrite(buzzer, HIGH);    // Còi kích mức LOW -> Để HIGH để tắt
  digitalWrite(FanPinA, LOW);    // Quạt kích mức HIGH -> Để LOW để tắt
  digitalWrite(LED_PIN, LOW);    // Tắt đèn LED vàng

  myservo.attach(6);
  lockServo.attach(3);
  lockServo.write(lockPos);  // Khóa cửa lúc vừa bật máy

  lcd.init();
  lcd.backlight();
  irrecv.enableIRIn();
  SPI.begin();
  rfid.init();

  Serial.println("HE THONG NHA THONG MINH DA SAN SANG!");
}

void loop() {
  LED();        // 1. Kiểm tra nút nhấn bật/tắt đèn vàng
  rainwater();  // 2. Kiểm tra cảm biến mưa
  RD();         // 3. Quét thẻ từ RFID mở cửa
  smoke();      // 4. Quét khí gas báo cháy
  LCD();        // 5. Nhận tia hồng ngoại và hiển thị Nhiệt độ/Độ ẩm
  
  // 6. Gửi dữ liệu tới ESP32 (D0-D1)
  sendDataToESP32();
  
  // 7. Nhận dữ liệu điều khiển từ ESP32 (D0-D1)
  receiveDataFromESP32();
}

// ---------------- CÁC HÀM CHỨC NĂNG ----------------

// 1. Hàm xử lý nút nhấn đèn vàng (Bật/Tắt luân phiên có chống nhiễu)
void LED() {
  int buttonState = analogRead(A6);

  if (buttonState > 500 && KEY_BIT == 1) {  // Khi có người ấn nút xuống
    KEY_BIT = 0;
    LED_BIT = !LED_BIT;  // Đảo trạng thái đèn
    if (LED_BIT) digitalWrite(LED_PIN, HIGH);
    else digitalWrite(LED_PIN, LOW);
  } else if (buttonState < 100) {  // Khi thả nút ra
    KEY_BIT = 1;
  }
}

// 2. Cảm biến tự động đóng cửa sổ khi có mưa
void rainwater() {
  rain = map(analogRead(Rain), 0, 1023, 255, 0);
  if (RainFlag == 1) {  // Chỉ tự động nếu đã bấm nút kích hoạt (phím EQ trên remote)
    Serial.println("receive IR 2");
    if (rain > 130) {
      myservo.write(130);  // Trời mưa -> Đóng cửa sổ
    } else {
      myservo.write(10);  // Trời tạnh -> Mở cửa sổ
    }
  }
}

// 3. Hệ thống quẹt thẻ từ RFID
void RD() {
  if (rfid.findCard(PICC_REQIDL, str) == MI_OK) {
    String temp = "";
    if (rfid.anticoll(str) == MI_OK) {
      Serial.print("Ma the quet vao la: ");
      for (int i = 0; i < 4; i++) {
        temp = temp + (0x0F & (str[i] >> 4));
        temp = temp + (0x0F & str[i]);
      }
      Serial.println(temp);
      checkAccess(temp);
    }
    rfid.selectTag(str);
  }
  rfid.halt();
}

void checkAccess(String temp) {
  boolean granted = false;
  for (int i = 0; i < accessGrantedSize; i++) {
    if (accessGranted[i] == temp) {
      Serial.println("Dung the. Mo cua!");
      granted = true;
      if (locked == true) {
        lockServo.write(unlockPos);
        locked = false;
      } else {
        lockServo.write(lockPos);
        locked = true;
      }
    }
  }
  if (granted == false) {
    Serial.println("Sai the. Tu choi!");
  }
}

// 4. Báo động khí gas an toàn cháy nổ
void smoke() {
  int analogSensor = analogRead(smokeA0);
  if (analogSensor > sensorThres) {
    // KHI CÓ KHÍ GAS VƯỢT NGƯỠNG
    digitalWrite(FanPinA, HIGH);  // Bật quạt thổi khí (HIGH)
    digitalWrite(RelayPin, LOW);  // Đóng rơ-le (LOW)

    digitalWrite(buzzer, LOW);  // Hú còi inh ỏi (LOW)
    delay(100);
    digitalWrite(buzzer, HIGH);  // Ngắt còi tạo nhịp
    delay(100);
  } else {
    // KHI KHÔNG KHÍ SẠCH HOẶC ĐÃ HẾT GAS
    digitalWrite(FanPinA, LOW);    // Tắt quạt
    digitalWrite(RelayPin, HIGH);  // Ngắt rơ-le
    digitalWrite(buzzer, HIGH);    // Tắt còi
  }
}

// 5. Điều khiển từ xa qua Remote và cập nhật màn hình LCD
void LCD() {
  ShowHumiture();
  if (irrecv.decode(&results)) {
    Serial.println("===== IR RECEIVED =====");
    Serial.print("Raw Value (DEC): ");
    Serial.println(results.value);
    Serial.print("Raw Value (HEX): 0x");
    Serial.println(results.value, HEX);
    
    // Kiểm tra REPEAT code
    if (results.value == 0xFFFFFFFF) {
      Serial.println("REPEAT KEY (holding button)");
      irrecv.resume();
      return;
    }
    
    if (results.value == 0) {
      Serial.println("ERROR: Value = 0, khong decode duoc");
      irrecv.resume();
      return;
    }
    
    Serial.println("---Checking Keys---");

    // Nhận lệnh từ Remote
    if (results.value == KEY11) {
      Serial.println("Nhan KEY11 - BAT DEN LCD");
      lcd.backlight();
    } else if (results.value == KEY22) {
      Serial.println("Nhan KEY22 - TAT DEN LCD");
      lcd.noBacklight();
      lcd.clear();
    } else if (results.value == KEY33) {
      Serial.println("Nhan KEY33 - BAT RELAY VA QUAN");
      digitalWrite(RelayPin, LOW);
      digitalWrite(FanPinA, HIGH);
    } else if (results.value == KEY44) {
      Serial.println("Nhan KEY44 - TAT RELAY VA QUAN");
      digitalWrite(RelayPin, HIGH);
      digitalWrite(FanPinA, LOW);
    } else if (results.value == KEY55) {
      Serial.println("Nhan KEY55 - MO CUA SO MAI NHA");
      RainFlag = 0;
      myservo.write(10);
      delay(15);
    } else if (results.value == KEY66) {
      Serial.println("Nhan KEY66 - DONG CUA SO MAI NHA");
      RainFlag = 0;
      myservo.write(130);
      delay(15);
    } else if (results.value == KEY77) {
      Serial.println("Nhan KEY77 - MO CUA CHINH");
      lockServo.write(unlockPos);
      delay(15);
    } else if (results.value == KEY88) {
      Serial.println("Nhan KEY88 - DONG CUA CHINH");
      lockServo.write(lockPos);
      delay(15);
    } else if (results.value == KEY00) {
      Serial.println("Nhan KEY00 - BAT CHE DO TU DONG");
      RainFlag = 1;
    } else {
      Serial.print("KHONG MATCH KEY NAO! Value nhan duoc: 0x");
      Serial.println(results.value, HEX);
      Serial.println("Cap nhat ma KEY trong code!");
    }
    Serial.println("======================");

    irrecv.resume();  // Chờ nhận tín hiệu hồng ngoại tiếp theo
  }
}

// 6. Đọc cảm biến DHT11 và hiển thị LCD
void ShowHumiture() {
  DHT.read11(DHT11_PIN);
  lcd.setCursor(0, 0);
  lcd.print("Humi: ");
  lcd.print(DHT.humidity);
  lcd.print(" %");

  lcd.setCursor(0, 1);
  lcd.print("Temp: ");
  lcd.print(DHT.temperature);
  lcd.print(" C");
}

// 7. GỬI DỮ LIỆU TỚI ESP32 (Serial Hardware D0-D1)
void sendDataToESP32() {
  static unsigned long lastSendTime = 0;
  unsigned long currentTime = millis();
  
  // Gửi dữ liệu mỗi 2 giây (tránh quá tải)
  if (currentTime - lastSendTime >= 2000) {
    lastSendTime = currentTime;
    
    DHT.read11(DHT11_PIN);
    int gasLevel = analogRead(smokeA0);
    int rainLevel = analogRead(Rain);
    
    // Format: H:XX,T:XX,G:XXX,R:XXX\n
    Serial.print("H:");
    Serial.print(DHT.humidity);
    Serial.print(",T:");
    Serial.print(DHT.temperature);
    Serial.print(",G:");
    Serial.print(gasLevel);
    Serial.print(",R:");
    Serial.println(rainLevel);
  }
}

// 8. NHẬN DỮ LIỆU ĐIỀU KHIỂN TỪ ESP32 (Serial Hardware D0-D1)
void receiveDataFromESP32() {
  // Format nhận: F:1,W:0,D:1,L:1\n
  // F = Fan, W = Window, D = Door, L = LCD
  // 1 = ON/OPEN, 0 = OFF/CLOSE
  
  if (Serial.available()) {
    String data = Serial.readStringUntil('\n');
    
    // Bỏ debug vì Serial dùng chung cho ESP32
    
    // Parse dữ liệu
    if (data.indexOf("F:") != -1) {
      int idx = data.indexOf("F:") + 2;
      fanControl = (data[idx] == '1') ? true : false;
      digitalWrite(FanPinA, fanControl ? HIGH : LOW);
    }
    
    if (data.indexOf("W:") != -1) {
      int idx = data.indexOf("W:") + 2;
      windowControl = (data[idx] == '1') ? true : false;
      myservo.write(windowControl ? 10 : 130);  // 10=MỞ, 130=ĐÓNG
    }
    
    if (data.indexOf("D:") != -1) {
      int idx = data.indexOf("D:") + 2;
      doorControl = (data[idx] == '1') ? true : false;
      lockServo.write(doorControl ? unlockPos : lockPos);  // 90=MỞ, 0=ĐÓNG
    }
    
    if (data.indexOf("L:") != -1) {
      int idx = data.indexOf("L:") + 2;
      lcdBacklight = (data[idx] == '1') ? true : false;
      lcdBacklight ? lcd.backlight() : lcd.noBacklight();
    }
  }
}