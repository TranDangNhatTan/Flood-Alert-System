/* ==========================================================================
 * PROJECT: TRẠM CẢNH BÁO LŨ 4 CẤP (FINAL FIX - NO SPAM, CLEAN SMS, STABLE CALL)
 * ==========================================================================
 * LOGIC:
 * - Cấp 0, 1: Chỉ báo đèn.
 * - Cấp 2: Đèn Đỏ + Gửi SMS (1 lần duy nhất khi chuyển cấp).
 * - Cấp 3: Đèn Đỏ + Còi + Gửi SMS (1 lần duy nhất khi chuyển cấp).
 * - Cấp 4: Đèn Đỏ Nháy + Còi + Gửi SMS + Gọi Điện (1 lần duy nhất).
 * ==========================================================================
 */

#include <Adafruit_GFX.h>    
#include <Adafruit_ST7735.h> 
#include <SPI.h>
#include <EEPROM.h>
#include <ESP32Time.h>       

// ===  1 Thông tin Blynk và Wifi ===
#define BLYNK_TEMPLATE_ID "TMPL6TIusvQtp"
#define BLYNK_TEMPLATE_NAME "Trạm cảnh báo lũ lụt"
#define BLYNK_AUTH_TOKEN "GBFKLaFe7T9x-p0iY47hRmJLzwRPki-p"

#define WIFI_SSID           "Khu tro"   // <--- Tên Wifi
#define WIFI_PASS           "0905120084"     // <--- Mật khẩu

#include <WiFi.h>
#include <WiFiClient.h>
#include <BlynkSimpleEsp32.h>

// === 1. CẤU HÌNH CHÂN ===
#define TFT_CS        5
#define TFT_RST       4
#define TFT_DC        2
// SCK->18, MOSI->23

#define TRIGGER_PIN   32      
#define ECHO_PIN      33      
#define LED_GREEN     25      
#define LED_YELLOW    26      
#define LED_RED       14      
#define BUZZER_PIN    27      
#define SIM_RX_PIN    16 
#define SIM_TX_PIN    17 

// === 2. CẤU HÌNH NGƯỠNG (cm) ===
float LEVEL_1_DIST = 30.0; // Vàng
float LEVEL_2_DIST = 20.0; // Đỏ + SMS
float LEVEL_3_DIST = 15.0; // Đỏ + Còi + SMS
float LEVEL_4_DIST = 10.0; // Đỏ nháy + Còi + Gọi

#define SIM_BAUDRATE  115200
#define EEPROM_SIZE   512
#define ADDR_MIN      0
#define ADDR_MAX      10
#define ADDR_DAY      20

Adafruit_ST7735 tft = Adafruit_ST7735(TFT_CS, TFT_DC, TFT_RST);
ESP32Time rtc(0);

// --- BIẾN TOÀN CỤC ---
float min_dist = 1000.0;
float max_dist = 0.0;
String phone_number = "0392479919"; // SỐ ĐIỆN THOẠI CỦA BẠN

// Biến logic
int currentLevel = 0;   
int previousLevel = -1; // Khởi tạo -1 để lần đầu tiên chạy nó sẽ cập nhật ngay

// Biến quản lý cuộc gọi (Non-blocking)
bool isCalling = false;
unsigned long callStartTime = 0;
const unsigned long callDuration = 25000; // Gọi 25 giây cho chắc ăn

const int NUM_READINGS = 9; 
float readings[NUM_READINGS];

// Khai báo hàm
void handleAlertLogic(float dist); 
float getFilteredDistance();
void updateDisplayData(float dist, int level);
void drawStaticInterface();
void sendSMS(String message);
void startCall();       
void checkCallStatus(); 
void sim_at_cmd(String cmd);
void checkSerialForTimeUpdate();

// --- [THÊM MỚI 2] BIẾN TRẠNG THÁI WIFI ---
bool wifi_connected = false;

// ==========================================================================
//                                  SETUP
// ==========================================================================
void setup() {
  Serial.begin(115200);
  Serial2.begin(SIM_BAUDRATE, SERIAL_8N1, SIM_RX_PIN, SIM_TX_PIN);
  
  pinMode(TRIGGER_PIN, OUTPUT); pinMode(ECHO_PIN, INPUT);
  pinMode(LED_GREEN, OUTPUT); pinMode(LED_YELLOW, OUTPUT);
  pinMode(LED_RED, OUTPUT); pinMode(BUZZER_PIN, OUTPUT);
  
  digitalWrite(LED_GREEN, LOW); digitalWrite(LED_YELLOW, LOW);
  digitalWrite(LED_RED, LOW); digitalWrite(BUZZER_PIN, LOW);

  tft.initR(INITR_BLACKTAB); 
  tft.fillScreen(ST7735_BLACK);
  tft.setRotation(0); 
  
  tft.setTextColor(ST7735_WHITE);
  tft.setTextSize(1);
  tft.setCursor(5, 20); tft.println("Dang khoi dong...");
  
  delay(2000); // Chờ SIM ổn định

  EEPROM.begin(EEPROM_SIZE);
  
  // --- CẤU HÌNH SIM (FIX LỖI TIN NHẮN RÁC) ---
  Serial.println("Cau hinh SIM...");
  sim_at_cmd("ATE0"); // Tắt Echo (Quan trọng!)
  sim_at_cmd("AT+CMGF=1");
  sim_at_cmd("AT+CSCS=\"GSM\"");
  sim_at_cmd("AT+CNMI=2,2,0,0,0");
  sim_at_cmd("AT+CLVL=100"); // Max âm lượng

  min_dist = EEPROM.readFloat(ADDR_MIN);
  max_dist = EEPROM.readFloat(ADDR_MAX);
  if (isnan(min_dist) || min_dist > 1000) min_dist = 1000.0;
  if (isnan(max_dist) || max_dist < 0) max_dist = 0.0;

  tft.fillScreen(ST7735_BLACK);
  drawStaticInterface();

  // --- [THÊM MỚI 3] KẾT NỐI BLYNK (KHÔNG CHẶN) ---
  tft.setCursor(5, 60); tft.println("Ket noi WiFi...");
  
  Blynk.config(BLYNK_AUTH_TOKEN);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  
  // Chờ kết nối tối đa 10 giây
  unsigned long startWifi = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - startWifi < 10000) {
    delay(500); Serial.print(".");
  }
  
  if (WiFi.status() == WL_CONNECTED) {
    wifi_connected = true;
    tft.println("WiFi OK!");
  } else {
    wifi_connected = false;
    tft.println("WiFi Failed!"); // Mất mạng vẫn chạy tiếp
  }
  delay(1000);
}

// ==========================================================================
//                                   LOOP
// ==========================================================================
void loop() {

  // --- [THÊM MỚI 4] DUY TRÌ BLYNK ---
  if (wifi_connected) {
    if (WiFi.status() == WL_CONNECTED) Blynk.run();
    else wifi_connected = false;
  }

  // 1. Kiểm tra tắt cuộc gọi ngầm
  checkCallStatus();

  // 2. Đo đạc
  float distance = getFilteredDistance();
  checkSerialForTimeUpdate();

  // 3. Xử lý Logic
  handleAlertLogic(distance);

  // 4. Lưu EEPROM
  if (distance < min_dist && distance > 0) { min_dist = distance; EEPROM.writeFloat(ADDR_MIN, min_dist); EEPROM.commit(); }
  if (distance > max_dist && distance < 1000) { max_dist = distance; EEPROM.writeFloat(ADDR_MAX, max_dist); EEPROM.commit(); }

  // 5. Reset ngày
  static int lastDay = -1;
  if (rtc.getDay() != lastDay && rtc.getHour(true) == 0) {
    min_dist = distance; max_dist = distance;
    lastDay = rtc.getDay();
  }

  updateDisplayData(distance, currentLevel);
  
  delay(200); 
}

// ==========================================================================
//                      LOGIC XỬ LÝ (ĐÃ FIX LỖI SPAM)
// ==========================================================================
void handleAlertLogic(float dist) {
  if (dist <= LEVEL_4_DIST) currentLevel = 4;      
  else if (dist <= LEVEL_3_DIST) currentLevel = 3; 
  else if (dist <= LEVEL_2_DIST) currentLevel = 2; 
  else if (dist <= LEVEL_1_DIST) currentLevel = 1; 
  else currentLevel = 0;                           

  // Chỉ thực hiện hành động khi CÓ SỰ THAY ĐỔI CẤP ĐỘ
  bool statusChanged = (currentLevel != previousLevel);

  // Reset đèn (trừ cấp 4)
  if(currentLevel != 4) {
     digitalWrite(LED_GREEN, LOW); digitalWrite(LED_YELLOW, LOW); digitalWrite(LED_RED, LOW);
  }

  switch (currentLevel) {
    case 0: // AN TOÀN
      digitalWrite(LED_GREEN, HIGH); digitalWrite(BUZZER_PIN, LOW);
      break;

    case 1: // VÀNG
      digitalWrite(LED_YELLOW, HIGH); digitalWrite(BUZZER_PIN, LOW);
      break;

    case 2: // ĐỎ + SMS (CHỈ GỬI 1 LẦN)
      digitalWrite(LED_RED, HIGH); digitalWrite(BUZZER_PIN, LOW);
      if (statusChanged) { // Chỉ gửi khi vừa chuyển sang cấp 2
        sendSMS("CAP 2: Nuoc dang cao (" + String(dist) + "cm)");
      }
      break;

    case 3: // ĐỎ + CÒI + SMS (CHỈ GỬI 1 LẦN)
      digitalWrite(LED_RED, HIGH); digitalWrite(BUZZER_PIN, HIGH);
      if (statusChanged) { // Chỉ gửi khi vừa chuyển sang cấp 3
        sendSMS("CAP 3: NGUY HIEM! (" + String(dist) + "cm)");
      }
      break;

    case 4: // ĐỎ NHÁY + CÒI + GỌI (CHỈ GỌI 1 LẦN)
      // Nháy đèn
      if ((millis() / 200) % 2 == 0) digitalWrite(LED_RED, HIGH);
      else digitalWrite(LED_RED, LOW);
      
      digitalWrite(BUZZER_PIN, HIGH);
      
      if (statusChanged) { // Chỉ thực hiện khi vừa chuyển sang cấp 4
        sendSMS("KHAN CAP: LU LUT! (" + String(dist) + "cm)");
        startCall(); // Gọi điện 1 lần duy nhất
      }
      break;
  }
  // --- [THÊM MỚI 5] ĐẨY DỮ LIỆU LÊN APP ---
  if (wifi_connected) {
    Blynk.virtualWrite(V0, dist); // Đẩy mực nước
    
    String statusTxt = "An Toan";
    if(currentLevel == 1) statusTxt = "Canh Bao (Vang)";
    if(currentLevel == 2) statusTxt = "NGUY HIEM (Do)";
    if(currentLevel >= 3) statusTxt = "KHAN CAP!";
    Blynk.virtualWrite(V1, statusTxt); // Đẩy dòng chữ trạng thái
  }
  
  // Cập nhật trạng thái cũ để so sánh lần sau
  previousLevel = currentLevel;
}

// ==========================================================================
//                  XỬ LÝ GỌI ĐIỆN (ĐÃ FIX LỖI CHẬP CHỜN)
// ==========================================================================

void startCall() {
  // Chỉ gọi nếu không đang bận gọi
  if (!isCalling) { 
    Serial.println("Bat dau goi dien...");
    // Gửi lệnh 1 lần duy nhất
    Serial2.println("ATD" + phone_number + ";");
    
    isCalling = true;
    callStartTime = millis(); 
  }
}

void checkCallStatus() {
  // Tự động tắt máy sau 25 giây
  if (isCalling && (millis() - callStartTime > callDuration)) {
    Serial.println("Het gio, tat may.");
    sim_at_cmd("ATH"); 
    isCalling = false; 
  }
}

// ==========================================================================
//                            HÀM GỬI TIN NHẮN (ĐÃ FIX ECHO)
// ==========================================================================
void sendSMS(String message) {
  // Xóa bộ đệm
  while(Serial2.available()) Serial2.read();

  Serial2.print("AT+CMGS=\""); 
  Serial2.print(phone_number); 
  Serial2.println("\"");
  
  delay(500); 
  Serial2.print(message); 
  delay(500); 
  
  Serial2.write(26); 
  delay(2000); // Chờ gửi xong
}

// ==========================================================================
//                            CÁC HÀM PHỤ TRỢ
// ==========================================================================

float getFilteredDistance() {
  for (int i = 0; i < NUM_READINGS; i++) {
    digitalWrite(TRIGGER_PIN, LOW); delayMicroseconds(2);
    digitalWrite(TRIGGER_PIN, HIGH); delayMicroseconds(10);
    digitalWrite(TRIGGER_PIN, LOW);
    long duration = pulseIn(ECHO_PIN, HIGH, 25000); 
    readings[i] = (duration == 0) ? 999 : (duration * 0.0343 / 2);
    delay(5);
  }
  for (int i = 0; i < NUM_READINGS - 1; i++) {
    for (int j = 0; j < NUM_READINGS - i - 1; j++) {
      if (readings[j] > readings[j + 1]) { float temp = readings[j]; readings[j] = readings[j + 1]; readings[j + 1] = temp; }
    }
  }
  return readings[NUM_READINGS / 2];
}

void drawStaticInterface() {
  tft.drawFastHLine(0, 25, 80, ST7735_WHITE); 
  tft.setTextSize(1); tft.setTextColor(ST7735_WHITE);
  tft.setCursor(5, 35); tft.print("MUC NUOC:");
  tft.drawFastHLine(0, 90, 80, ST7735_WHITE);
}

void updateDisplayData(float dist, int level) {
  tft.setTextSize(1); tft.setCursor(2, 5);
  uint16_t color; String txt;
  switch(level) {
    case 0: color=ST7735_GREEN; txt=" AN TOAN "; break;
    case 1: color=ST7735_YELLOW; txt=" CANH BAO"; break;
    case 2: color=ST7735_ORANGE; txt=" NGUY HIEM"; break;
    case 3: color=ST7735_RED; txt=" CAP 3 !!"; break;
    case 4: color=ST7735_RED; txt=" KHAN CAP"; break;
  }
  tft.setTextColor(ST7735_WHITE, color); tft.print(txt);
  
  tft.setTextColor(ST7735_WHITE, ST7735_BLACK);
  tft.setCursor(45, 15); tft.print(rtc.getTime("%H:%M"));

  tft.setTextSize(2); tft.setCursor(15, 50);
  if (level >= 3) tft.setTextColor(ST7735_RED, ST7735_BLACK);
  else tft.setTextColor(ST7735_CYAN, ST7735_BLACK);
  tft.print(dist, 1); 
  tft.setTextSize(1); tft.setCursor(30, 70); tft.print("cm");

  tft.setTextColor(ST7735_YELLOW, ST7735_BLACK);
  tft.setCursor(5, 100); tft.print("Min: "); tft.setTextColor(ST7735_WHITE, ST7735_BLACK); tft.print(min_dist, 0);
  tft.setCursor(5, 120); tft.print("Max: "); tft.setTextColor(ST7735_WHITE, ST7735_BLACK); tft.print(max_dist, 0);
}

void checkSerialForTimeUpdate() {
  if (Serial.available()) {
    String input = Serial.readStringUntil('\n'); input.trim();
    if (input.length() >= 19) {
      int Y=input.substring(0,4).toInt(); int M=input.substring(5,7).toInt(); int D=input.substring(8,10).toInt();
      int h=input.substring(11,13).toInt(); int m=input.substring(14,16).toInt(); int s=input.substring(17,19).toInt();
      rtc.setTime(s, m, h, D, M, Y);
    }
  }
}

void sim_at_cmd(String cmd) {
  Serial2.println(cmd); delay(50); 
  while (Serial2.available()) Serial.write(Serial2.read());
}