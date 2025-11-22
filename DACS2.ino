/* ==========================================================================
 * PROJECT: HỆ THỐNG CẢNH BÁO NGẬP ÚNG (PHIÊN BẢN TFT SPI 8 CHÂN)
 * ==========================================================================
 * CẤU HÌNH PHẦN CỨNG MỚI:
 * - MÀN HÌNH TFT (ST7735):
 * + SCL (SCK)  -> GPIO 18
 * + SDA (MOSI) -> GPIO 23
 * + RES (RST)  -> GPIO 4
 * + DC (A0)    -> GPIO 2
 * + CS         -> GPIO 5
 * - CẢM BIẾN SR04:
 * + TRIG       -> GPIO 32
 * + ECHO       -> GPIO 33
 * - CẢNH BÁO:
 * + BUZZER (+) -> GPIO 27
 * + LED (+)    -> GPIO 14
 * - SIM A7680C:
 * + TX         -> GPIO 16
 * + RX         -> GPIO 17
 * ==========================================================================
 */

#include <Adafruit_GFX.h>    // Thư viện đồ họa
#include <Adafruit_ST7735.h> // Thư viện màn hình ST7735
#include <SPI.h>
#include <EEPROM.h>
#include <ESP32Time.h>       // Thư viện thời gian

// === 1. CẤU HÌNH CHÂN (GPIO) ===
#define TFT_CS        5
#define TFT_RST       4
#define TFT_DC        2
// Lưu ý: SCK (18) và MOSI (23) được thư viện tự nhận diện qua Hardware SPI

#define TRIGGER_PIN   32      // Chân mới của cảm biến
#define ECHO_PIN      33      // Chân mới của cảm biến
#define BUZZER_PIN    27      // Chân mới của Còi
#define LED_PIN       14      // Chân mới của LED
#define SIM_RX_PIN    16 
#define SIM_TX_PIN    17 

// === 2. CẤU HÌNH HỆ THỐNG ===
#define SIM_BAUDRATE  115200
#define HOUR_SEND     19      // Giờ gửi báo cáo hàng ngày
#define MINUTE_SEND   00
#define EEPROM_SIZE   512

// Địa chỉ lưu trong EEPROM
#define ADDR_MIN_DISTANCE 0
#define ADDR_MAX_DISTANCE 10
#define ADDR_LAST_DAY     20

// === 3. KHỞI TẠO ĐỐI TƯỢNG ===
// Khởi tạo màn hình ST7735
Adafruit_ST7735 tft = Adafruit_ST7735(TFT_CS, TFT_DC, TFT_RST);
ESP32Time rtc(0);

// === 4. BIẾN TOÀN CỤC ===
float danger_distance = 10.0; // Ngưỡng nguy hiểm (cm)
float hysteresis_on = 2.0;
float hysteresis_off = 5.0;
bool alerting = false;

float min_distance = 1000.0;
float max_distance = 0.0;
String phone_number = "0392479919"; // THAY SỐ CỦA BẠN VÀO ĐÂY
bool report_sent = false;

// Biến lọc trung bình
float distance_samples[10];
int sample_index = 0;
float average_distance = 0.0;
bool samples_filled = false;

// ==========================================================================
//                                  SETUP
// ==========================================================================
void setup() {
  // 1. Khởi động Serial
  Serial.begin(115200);
  Serial2.begin(SIM_BAUDRATE, SERIAL_8N1, SIM_RX_PIN, SIM_TX_PIN);
  
  // 2. Cấu hình chân IO
  pinMode(TRIGGER_PIN, OUTPUT);
  pinMode(ECHO_PIN, INPUT);
  pinMode(BUZZER_PIN, OUTPUT);
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(BUZZER_PIN, LOW);
  digitalWrite(LED_PIN, LOW);

  // 3. Khởi động màn hình TFT
  // QUAN TRỌNG: Nếu màu bị sai hoặc nhiễu, thử đổi INITR_BLACKTAB thành INITR_GREENTAB
  tft.initR(INITR_BLACKTAB); 
  tft.fillScreen(ST7735_BLACK);
  tft.setRotation(1); // Xoay ngang màn hình
  
  // Hiển thị màn hình chào
  tft.setTextColor(ST7735_WHITE);
  tft.setTextSize(1);
  tft.setCursor(10, 20);
  tft.println("Dang khoi dong...");
  tft.setCursor(10, 40);
  tft.println("Tan's Project");
  delay(1000);

  // 4. Khởi động EEPROM
  EEPROM.begin(EEPROM_SIZE);
  
  // 5. Cấu hình SIM
  Serial.println("Cau hinh SIM...");
  sim_at_cmd("AT");
  sim_at_cmd("AT+CMGF=1");
  sim_at_cmd("AT+CNMI=2,2,0,0,0");

  // 6. Khôi phục dữ liệu cũ
  min_distance = EEPROM.readFloat(ADDR_MIN_DISTANCE);
  max_distance = EEPROM.readFloat(ADDR_MAX_DISTANCE);
  
  // Lọc giá trị rác
  if (isnan(min_distance) || min_distance > 1000.0 || min_distance < 0) min_distance = 1000.0;
  if (isnan(max_distance) || max_distance < 0 || max_distance > 1000.0) max_distance = 0.0;

  // Reset mảng mẫu
  for (int i = 0; i < 10; i++) distance_samples[i] = 0.0;

  // Xóa màn hình để vào màn hình chính
  tft.fillScreen(ST7735_BLACK);
  drawStaticInterface(); // Vẽ khung giao diện tĩnh
}

// ==========================================================================
//                                   LOOP
// ==========================================================================
void loop() {
  // 1. Đo đạc
  float distance = measureDistance();
  checkSerialForTimeUpdate();

  // 2. Tính trung bình
  distance_samples[sample_index] = distance;
  sample_index = (sample_index + 1) % 10;
  if (sample_index == 0) samples_filled = true;

  average_distance = 0.0;
  int sample_count = samples_filled ? 10 : sample_index;
  if (sample_count > 0) {
    for (int i = 0; i < sample_count; i++) average_distance += distance_samples[i];
    average_distance /= sample_count;
  }

  // 3. Cập nhật Min/Max
  if (distance < min_distance && distance > 0) {
    min_distance = distance;
    EEPROM.writeFloat(ADDR_MIN_DISTANCE, min_distance);
    EEPROM.commit();
  }
  if (distance > max_distance && distance < 1000) {
    max_distance = distance;
    EEPROM.writeFloat(ADDR_MAX_DISTANCE, max_distance);
    EEPROM.commit();
  }

  // 4. Reset Min/Max khi sang ngày mới
  int currentDay = rtc.getDay();
  int currentHour = rtc.getHour(true);
  byte last_day = EEPROM.read(ADDR_LAST_DAY);
  
  if (currentDay != last_day && currentHour >= 20) {
    min_distance = distance; max_distance = distance;
    EEPROM.writeFloat(ADDR_MIN_DISTANCE, min_distance);
    EEPROM.writeFloat(ADDR_MAX_DISTANCE, max_distance);
    EEPROM.write(ADDR_LAST_DAY, currentDay);
    EEPROM.commit();
  }

  // 5. Gửi báo cáo hàng ngày (19:00)
  if (currentHour == HOUR_SEND && rtc.getMinute() == MINUTE_SEND && rtc.getSecond() == 0 && !report_sent) {
    String msg = "BAO CAO NGAY " + rtc.getDate() + ": Min: " + String(min_distance) + "cm, Max: " + String(max_distance) + "cm.";
    sendSMS(msg);
    report_sent = true;
  } else if (currentHour != HOUR_SEND) {
    report_sent = false;
  }

  // 6. Logic Cảnh báo
  if (samples_filled && !alerting && average_distance < (danger_distance - hysteresis_on)) {
    alerting = true;
    digitalWrite(BUZZER_PIN, HIGH);
    digitalWrite(LED_PIN, HIGH);
    
    String msg = "CANH BAO LU! Muc nuoc: " + String(average_distance) + "cm luc " + rtc.getTime();
    Serial.println(msg);
    sendSMS(msg);
    makeCall();
  } else if (alerting && average_distance > (danger_distance + hysteresis_off)) {
    alerting = false;
    digitalWrite(BUZZER_PIN, LOW);
    digitalWrite(LED_PIN, LOW);
  }

  // 7. Hiển thị
  updateDisplayData(average_distance);
  delay(1000);
}

// ==========================================================================
//                            HÀM HIỂN THỊ (TFT)
// ==========================================================================

// Hàm vẽ khung giao diện tĩnh (Chỉ chạy 1 lần để đỡ nháy màn hình)
void drawStaticInterface() {
  tft.drawFastHLine(0, 18, 160, ST7735_WHITE); // Kẻ ngang phân cách
  
  tft.setTextSize(1);
  tft.setTextColor(ST7735_WHITE);
  tft.setCursor(0, 25);
  tft.print("Muc nuoc hien tai:");
  
  tft.setCursor(0, 75);
  tft.setTextColor(ST7735_YELLOW);
  tft.print("Min:");
  tft.setCursor(80, 75);
  tft.print("Max:");
}

// Hàm cập nhật số liệu (Chạy liên tục)
void updateDisplayData(float avg) {
  // 1. Hiển thị Tiêu đề / Trạng thái
  tft.setTextSize(1);
  tft.setCursor(0, 5);
  if (alerting) {
    tft.setTextColor(ST7735_WHITE, ST7735_RED); // Chữ trắng nền đỏ
    tft.print(" !! CANH BAO LU !! ");
  } else {
    tft.setTextColor(ST7735_GREEN, ST7735_BLACK); // Chữ xanh nền đen
    tft.print("  GIAM SAT AN TOAN ");
  }

  // 2. Hiển thị Giờ (Góc phải)
  tft.setTextColor(ST7735_WHITE, ST7735_BLACK);
  tft.setCursor(110, 5);
  tft.print(rtc.getTime("%H:%M"));

  // 3. Hiển thị Mức nước (Số to)
  tft.setTextSize(2);
  tft.setCursor(20, 45);
  // Đặt màu nền trùng màu background để tự xóa số cũ khi in số mới
  if (alerting) tft.setTextColor(ST7735_RED, ST7735_BLACK);
  else tft.setTextColor(ST7735_CYAN, ST7735_BLACK);
  
  tft.print(avg, 1); 
  tft.setTextSize(1);
  tft.print(" cm");

  // 4. Hiển thị Min/Max
  tft.setTextColor(ST7735_WHITE, ST7735_BLACK);
  tft.setCursor(25, 75);
  tft.print(min_distance, 0);
  tft.setCursor(105, 75);
  tft.print(max_distance, 0);
}

// ==========================================================================
//                            CÁC HÀM PHỤ TRỢ
// ==========================================================================

float measureDistance() {
  digitalWrite(TRIGGER_PIN, LOW); delayMicroseconds(2);
  digitalWrite(TRIGGER_PIN, HIGH); delayMicroseconds(10);
  digitalWrite(TRIGGER_PIN, LOW);
  long duration = pulseIn(ECHO_PIN, HIGH);
  if (duration == 0) return 999.0; 
  return duration * 0.0343 / 2;
}

void checkSerialForTimeUpdate() {
  if (Serial.available()) {
    String input = Serial.readStringUntil('\n'); input.trim();
    if (input.length() >= 19) {
      int year = input.substring(0, 4).toInt();
      int month = input.substring(5, 7).toInt();
      int day = input.substring(8, 10).toInt();
      int hour = input.substring(11, 13).toInt();
      int minute = input.substring(14, 16).toInt();
      int second = input.substring(17, 19).toInt();
      rtc.setTime(second, minute, hour, day, month, year);
      Serial.println("Da cap nhat gio!");
    }
  }
}

void sim_at_cmd(String cmd) {
  Serial2.println(cmd); delay(100);
  while (Serial2.available()) Serial.write(Serial2.read());
}

void sendSMS(String message) {
  sim_at_cmd("AT+CMGF=1");
  sim_at_cmd("AT+CSCS=\"GSM\"");
  Serial2.print("AT+CMGS=\""); Serial2.print(phone_number); Serial2.println("\"");
  delay(500); Serial2.print(message); delay(500);
  Serial2.write(26); delay(5000);
}

void makeCall() {
  String cmd = "ATD" + phone_number + ";";
  Serial2.println(cmd); delay(20000); Serial2.println("ATH");
}