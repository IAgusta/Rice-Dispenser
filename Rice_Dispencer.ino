#include <Keypad.h>
#include <WiFi.h>
#include <WebServer.h>
#include <EEPROM.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>

LiquidCrystal_I2C lcd(0x27, 16, 2);

#include <HX711_ADC.h>
#include <ESP32Servo.h>

const int HX711_dout = 4;
const int HX711_sck = 16;
HX711_ADC LoadCell(HX711_dout, HX711_sck);
const float CALIBRATION_FACTOR = 180.0;

#define Buzzer 5
bool buzzerFailed = false;
unsigned long buzzerStartTime = 0;
unsigned long lastBuzzerToggle = 0;
bool buzzerState = false;

#define SERVO_PIN_1 19
#define SERVO_PIN_2 23

Servo servo1, servo2;
int selectedServo = 1;
const int SERVO_OPEN_ANGLE = 90;
const int SERVO_CLOSE_ANGLE = 0;

const byte ROWS = 4, COLS = 4;
char keys[ROWS][COLS] = {
  {'1','2','3','A'},
  {'4','5','6','B'},
  {'7','8','9','C'},
  {'*','0','#','D'}
};
byte rowPins[ROWS] = {13, 12, 14, 27};
byte colPins[COLS] = {26, 25, 33, 32};
Keypad keypad = Keypad(makeKeymap(keys), rowPins, colPins, ROWS, COLS);

unsigned long keyPressStart = 0;
char lastKeyPressed = 0;
bool showPassword = false;

const String ADMIN_PASSWORD = "6969";
enum PasswordMode {
  NONE_PASS,
  ENTER_PASS
} passwordMode = NONE_PASS;

char pendingKey = '\0'; // temporarily holds which key is trying access
String passwordInput = "";

enum SpecialMode {
  NONE,
  EDIT_MIN_VALUE,
  EDIT_PRICE,
  CONFIRM_RESET,
  CONFIRM_EXECUTE,
  SHOW_IP,
  CONFIRM_ROLLBACK,
  SETTING_FACTOR
} specialMode = NONE;

String specialInput = "";
bool awaitingInput = false;

WebServer server(80);
const char* ssid = "RiceDropperAP";
const char* password = "password123";

#define EEPROM_SIZE 512
#define PRICE_A_ADDR 0
#define PRICE_B_ADDR (PRICE_A_ADDR + sizeof(float))
#define MIN_PRICE_ADDR (PRICE_B_ADDR + sizeof(float))
#define MIN_QTY_ADDR (MIN_PRICE_ADDR + sizeof(float))
#define RICE_ADDR (MIN_QTY_ADDR + sizeof(float))
#define EARN_ADDR (RICE_ADDR + sizeof(float))
#define CAL_FACTOR_ADDR (EARN_ADDR + sizeof(float))

String currentInput = "";
bool inputMode = false;
bool inputCleared = false;
float minPrice = 5000.0;
float minQuantity = 0.25;
float totalRiceDropped = 0.0;
float totalEarnings = 0.0;
float pricePerKgA = 15000.0;
float pricePerKgB = 15000.0;
float lastDropWeight = 0.0;
float calFactor = CALIBRATION_FACTOR;
int lastDropType = 1;

void BuzzerActive(bool state) {
  buzzerState = state;
  digitalWrite(Buzzer, state ? HIGH : LOW);  // LOW = ON (relay active)
  Serial.println(state ? "Buzzer ON" : "Buzzer OFF");
}

void cancelBuzzer() {
  if (buzzerFailed) {
    buzzerFailed = false;
    BuzzerActive(false);
  }
}

void initializeBuzzer(){
  pinMode(Buzzer, OUTPUT);
  BuzzerActive(false);          // make sure buzzer is OFF
}

void initializeLCD() {
  Wire.begin(21, 22);
  lcd.begin();
  lcd.backlight();
  lcd.clear(); lcd.print("Loading...");
  delay(500);
  lcd.setCursor(0, 1); lcd.print("Tunggu Sebentar");
}

void initializeScale() {
  LoadCell.begin();
  LoadCell.start(2000);
  LoadCell.setCalFactor(CALIBRATION_FACTOR);
  LoadCell.tare();
  lcd.clear();  lcd.print("Memulai...");
  delay(1000);
  lcd.setCursor(0, 1); lcd.print("Menyiapkan...");
  delay(2000);
}

void initializeServo() {
  ESP32PWM::allocateTimer(0);
  ESP32PWM::allocateTimer(1);
  ESP32PWM::allocateTimer(2);
  ESP32PWM::allocateTimer(3);
  servo1.setPeriodHertz(50);
  servo2.setPeriodHertz(50);
  servo1.attach(SERVO_PIN_1, 500, 2400);
  servo2.attach(SERVO_PIN_2, 500, 2400);
  servo1.write(SERVO_CLOSE_ANGLE);
  servo2.write(SERVO_CLOSE_ANGLE);
}

void loadPersistentData() {
  EEPROM.get(PRICE_A_ADDR, pricePerKgA);
  EEPROM.get(PRICE_B_ADDR, pricePerKgB);
  EEPROM.get(MIN_PRICE_ADDR, minPrice);
  EEPROM.get(MIN_QTY_ADDR, minQuantity);
  EEPROM.get(RICE_ADDR, totalRiceDropped);
  EEPROM.get(EARN_ADDR, totalEarnings);
  EEPROM.get(CAL_FACTOR_ADDR, calFactor);
  if (isnan(calFactor) || calFactor < 10.0) calFactor = CALIBRATION_FACTOR;
  LoadCell.setCalFactor(calFactor);
  if (isnan(pricePerKgA) || pricePerKgA < 1000) pricePerKgA = 15000.0;
  if (isnan(pricePerKgB) || pricePerKgB < 1000) pricePerKgB = 15000.0;
  if (isnan(minPrice) || minPrice < 100) minPrice = 5000.0;
  if (isnan(minQuantity) || minQuantity < 0.01) minQuantity = 0.25;
  if (isnan(totalRiceDropped) || totalRiceDropped < 0) totalRiceDropped = 0.0;
  if (isnan(totalEarnings) || totalEarnings < 0) totalEarnings = 0.0;
}

void savePersistentData() {
  EEPROM.put(PRICE_A_ADDR, pricePerKgA);
  EEPROM.put(PRICE_B_ADDR, pricePerKgB);
  EEPROM.put(MIN_PRICE_ADDR, minPrice);
  EEPROM.put(MIN_QTY_ADDR, minQuantity);
  EEPROM.put(RICE_ADDR, totalRiceDropped);
  EEPROM.put(EARN_ADDR, totalEarnings);
  EEPROM.put(CAL_FACTOR_ADDR, calFactor);
  EEPROM.commit();
}

void executeTransaction() {
  float inputValue = currentInput.toFloat();
  float targetWeight = 0.0;
  float pricePerKg = (selectedServo == 1) ? pricePerKgA : pricePerKgB;

  if (inputMode) {
    if (inputValue < minQuantity) {
      lcd.clear(); lcd.print("Min "); lcd.print(minQuantity); lcd.print(" kg");
      delay(2000); return;
    }
    targetWeight = inputValue;
  } else {
    if (inputValue < minPrice) {
      lcd.clear(); lcd.print("Min Rp."); lcd.print(minPrice);
      delay(2000); return;
    }
    targetWeight = inputValue / pricePerKg;
  }

  lcd.clear(); lcd.print("Memulai Proses..");
  delay(500);
  lcd.setCursor(0, 1); lcd.print("Tunggu Sebentar...");
  delay(1500);
  lcd.clear(); lcd.print("Menyiapkan");
  lcd.setCursor(0, 1); lcd.print("Timbangan...");
  LoadCell.tare();

  unsigned long tareStart = millis();
  while (!LoadCell.update() || abs(LoadCell.getData()) > 50) {
    if (millis() - tareStart > 5000) {
      lcd.clear(); lcd.print("Tare Gagal!");
      delay(2000);
      return;
    }
    delay(100);
  }

  lcd.clear(); lcd.print("Proses...");
  lcd.setCursor(0, 1); lcd.print("Menimbang...");

  if (selectedServo == 1) servo1.write(SERVO_OPEN_ANGLE);
  else servo2.write(SERVO_OPEN_ANGLE);
  delay(500);

    // Weighing logic with 10-second timeout (like you wanted)
  float weightBuffer[10] = {0};
  int bufferIndex = 0;
  float avgWeight = 0.0;
  float lastSignificantWeight = 0.0;
  unsigned long lastWeightIncreaseTime = millis();
  unsigned long startTime = millis();
  const float WEIGHT_THRESHOLD = 0.01; // 10 grams minimum increase
  const unsigned long NO_FLOW_TIMEOUT = 10000; // 10 seconds (your change)

  while (true) {
    if (LoadCell.update()) {
      float newWeight = abs(LoadCell.getData()) / 1000.0;

      weightBuffer[bufferIndex] = newWeight;
      bufferIndex = (bufferIndex + 1) % 10;

      float sum = 0;
      for (int i = 0; i < 10; i++) sum += weightBuffer[i];
      avgWeight = sum / 10.0;

      lcd.setCursor(0, 1);
      lcd.print("               ");
      lcd.setCursor(0, 1);
      lcd.print(avgWeight, 2);
      lcd.print("/");
      lcd.print(targetWeight, 2); lcd.print("kg");

      // Check if weight has increased significantly
      if (avgWeight - lastSignificantWeight > WEIGHT_THRESHOLD) {
        lastSignificantWeight = avgWeight;
        lastWeightIncreaseTime = millis();
      }

      // Check if target weight is reached
      if (avgWeight >= targetWeight) {
        break;
      }

      // Check if no rice has been dropping for 10 seconds
      if (millis() - lastWeightIncreaseTime > NO_FLOW_TIMEOUT) {
        lcd.clear(); lcd.print("Beras Habis");
        lcd.setCursor(0, 1); lcd.print("Isi Beras");
        delay(2000);
        break;
      }
    }
    delay(100);
  }

  // Close rice valve
  if (selectedServo == 1) servo1.write(SERVO_CLOSE_ANGLE);
  else servo2.write(SERVO_CLOSE_ANGLE);
  delay(1000);

  if (avgWeight >= targetWeight) {
    BuzzerActive(false);
    lastDropWeight = avgWeight;
    lastDropType = selectedServo;

    float rawPrice = avgWeight * pricePerKg;
    int roundedPrice = ((int)(rawPrice + 250) / 500) * 500;

    totalRiceDropped += avgWeight;
    totalEarnings += roundedPrice;
    savePersistentData();

    lcd.clear(); lcd.print("Transaksi Berhasil");
    lcd.setCursor(0, 1); lcd.print("Dapat Diambil");
    delay(2500);

    lcd.clear(); lcd.print("Harga Beras:");
    lcd.setCursor(0, 1); lcd.print("Rp."); lcd.print(roundedPrice);
    delay(2500);

    lcd.clear(); lcd.print("Bisa Digunakan");
    lcd.setCursor(0, 1); lcd.print("Tekan Tombol");
  } else {
       // FAILURE - Keep buzzer on and start failure sequence
    lcd.clear(); lcd.print("Tidak Cukup");
    lcd.setCursor(0, 1); lcd.print(avgWeight, 2); lcd.print(" kg");
    delay(3000);
    
    lastDropWeight = 0;
    lastDropType = 0;

    // Start buzzer failure sequence (30 seconds total, toggle every 3 seconds)
    buzzerFailed = true;
    buzzerStartTime = millis();
    lastBuzzerToggle = millis();
    buzzerState = true;
    BuzzerActive(true);
    
    lcd.clear(); lcd.print("Tekan Tombol");
    lcd.setCursor(0, 1); lcd.print("Untuk Memulai");
  }

  currentInput = "";
  inputCleared = true;
}

void processKey(char key) {
  // cancelBuzzer();
  if (specialMode != NONE || awaitingInput) return;

  if (inputCleared) { currentInput = ""; inputCleared = false; }
  if (key >= '0' && key <= '9') currentInput += key;
  else {
    switch (key) {
      case 'A':
        inputMode = !inputMode;
        lcd.clear(); lcd.setCursor(0, 0);
        lcd.print(inputMode ? "Mode: Berat" : "Mode: Harga");
        lcd.setCursor(0, 1);
        lcd.print("Beras Jenis "); lcd.print((selectedServo == 1) ? 'A' : 'B');
        break;
      case 'B':
        selectedServo = (selectedServo == 1) ? 2 : 1;
        lcd.clear();
        lcd.setCursor(0, 0);
        if (inputMode) lcd.print("Berat " + currentInput + " kg");
        else lcd.print("Rp." + currentInput);
        lcd.setCursor(0, 1);
        lcd.print("Beras Jenis "); lcd.print((selectedServo == 1) ? 'A' : 'B');
        break;
      case '*':
        if (currentInput.indexOf('.') == -1) currentInput += (currentInput.length() ? "." : "0.");
        break;
      case '#':
        if (currentInput.length() > 0) currentInput.remove(currentInput.length() - 1);
        break;
      case 'C':
        currentInput = "";
        inputCleared = true;
        break;
      case 'D':
        if (currentInput.length() > 0 && specialMode == NONE) {
          specialMode = CONFIRM_EXECUTE;
          awaitingInput = true;
          lcd.clear(); lcd.print("Lanjut Transaksi");
          lcd.setCursor(0, 1); lcd.print("Tidak: C, Ya: D");
        }
      return;
    }
  }
  if (key != 'A' && key != 'B') {
    lcd.clear(); lcd.setCursor(0, 0);
    if (inputMode) lcd.print("Berat " + currentInput + " kg");
    else lcd.print("Rp." + currentInput);
    lcd.setCursor(0, 1);
    lcd.print("Beras Jenis "); lcd.print((selectedServo == 1) ? 'A' : 'B');
  }
}

void handleHeldKey(char key) {
  specialInput = "";
  awaitingInput = true;

  if (key == 'A' || key == 'B' || key == 'C' || key == '#' || key == '*') {
    passwordMode = ENTER_PASS;
    passwordInput = "";
    pendingKey = key;
    lcd.clear();
    lcd.print("Masukkan Password");
    lcd.setCursor(0, 1);
    return;
  }

  if (key == 'D') {
    specialMode = NONE;
    awaitingInput = false;
  }
}

void handleAuthorizedHeldKey(char key) {
  specialInput = "";
  awaitingInput = true;

  switch (key) {
    case 'A':
      specialMode = inputMode ? EDIT_MIN_VALUE : EDIT_MIN_VALUE;
      lcd.clear();
      lcd.print(inputMode ? "Atur Berat Min." : "Atur Harga Min.");
      lcd.setCursor(0, 1);
      lcd.print(inputMode ? "Min. " + String(minQuantity, 2) + " kg" : "Rp." + String(minPrice, 0));
      break;
    case 'B':
      specialMode = EDIT_PRICE;
      lcd.clear();
      lcd.print("Harga Tipe ");
      lcd.print((selectedServo == 1) ? "A" : "B");
      lcd.setCursor(0, 1);
      lcd.print("Rp." + String((selectedServo == 1 ? pricePerKgA : pricePerKgB), 0));
      break;
    case 'C':
      specialMode = CONFIRM_RESET;
      lcd.clear();
      lcd.print("Hapus Pendapatan?");
      lcd.setCursor(0, 1);
      lcd.print("No: C, Yes: D");
      break;
    case '#':
      if (lastDropWeight > 0.0) {
        specialMode = CONFIRM_ROLLBACK;
        lcd.clear();
        lcd.print("Batalkan Terakhir?");
        lcd.setCursor(0, 1);
        lcd.print("No: C, Yes: D");
      } else {
        lcd.clear();
        lcd.print("Tidak Ada Data");
        lcd.setCursor(0, 1);
        lcd.print("Untuk Dibatalkan");
        delay(2500);
        specialMode = NONE;
        awaitingInput = false;
      }
      break;
    case '*':
      specialMode = SETTING_FACTOR;
      lcd.clear();
      lcd.setCursor(0, 0);
      lcd.print("Cal_Factor =");
      lcd.print(calFactor, 2);
      lcd.setCursor(0, 1);
      lcd.print("New_Cal: ");
      break;
  }
}

void handleSpecialInput(char key) {
  // Handle RESET mode (C = cancel, D = confirm)
  if (specialMode == CONFIRM_RESET) {
    if (key == 'C') {
      specialMode = NONE;
      awaitingInput = false;
      lcd.clear(); lcd.print("Dibatalkan");
      delay(1500);
    } else if (key == 'D') {
      totalEarnings = 0;
      totalRiceDropped = 0;
      lastDropWeight = 0;
      lastDropType = 0;
      savePersistentData();
      specialMode = NONE;
      awaitingInput = false;
      lcd.clear(); lcd.print("Data Direset!");
      delay(2000);
    }
    return; // no further processing
  }

  if (specialMode == SETTING_FACTOR) {
    if (key == 'C') {
      specialMode = NONE;
      awaitingInput = false;
      specialInput = "";
      lcd.clear(); lcd.print("Dibatalkan");
      delay(1500);
      return;
    }

    if (key == '#') {
      if (specialInput.length() > 0)
        specialInput.remove(specialInput.length() - 1);
    } else if (key == 'D') {
      if (specialInput.length() > 0) {
        float val = specialInput.toFloat();
        if (val >= 10.0 && val <= 100000.0) {
          calFactor = val;
          LoadCell.setCalFactor(calFactor);
          savePersistentData();
          lcd.clear(); lcd.print("Disimpan:");
          lcd.setCursor(0, 1); lcd.print("Cal: "); lcd.print(calFactor, 2);
          delay(2000);
        } else {
          lcd.clear(); lcd.print("Nilai Tidak Valid");
          delay(2000);
        }
      }
      specialMode = NONE;
      awaitingInput = false;
      return;
    } else if (key >= '0' && key <= '9') {
      specialInput += key;
    } else if (key == '*') {
      if (specialInput.indexOf('.') == -1)
        specialInput += ".";
    }

    lcd.setCursor(0, 1);
    lcd.print("                ");
    lcd.setCursor(0, 1);
    lcd.print("New_Cal: " + specialInput);
    return;
  }

  if (specialMode == CONFIRM_EXECUTE) {
    if (key == 'D') {
      // Confirm and run transaction
      specialMode = NONE;
      awaitingInput = false;
      executeTransaction();
    } else if (key == 'C') {
      // Cancel, reopen door and return to input
      specialMode = NONE;
      awaitingInput = false;
      lcd.clear();
      lcd.print("Dibatalkan");
      delay(1500);

      lcd.clear(); lcd.setCursor(0, 0);
      if (inputMode) lcd.print("Berat " + currentInput + " kg");
      else lcd.print("Rp. " + currentInput);
      lcd.setCursor(0, 1);
      lcd.print("Beras Jenis "); lcd.print((selectedServo == 1) ? 'A' : 'B');
    }
    return;
  }

  if (specialMode == CONFIRM_ROLLBACK) {
    if (key == 'C') {
      specialMode = NONE;
      awaitingInput = false;
      lcd.clear(); lcd.print("Dibatalkan");
      delay(1500);
    } else if (key == 'D' && specialMode == CONFIRM_ROLLBACK) {
    float rollbackPrice = lastDropWeight * ((lastDropType == 1) ? pricePerKgA : pricePerKgB);
    int roundedRollback = ((int)(rollbackPrice + 250) / 500) * 500;

    totalEarnings -= roundedRollback;
    totalRiceDropped -= lastDropWeight;

    if (totalEarnings < 0) totalEarnings = 0;
    if (totalRiceDropped < 0) totalRiceDropped = 0;

    lastDropWeight = 0;
    lastDropType = 0;
    savePersistentData();

    specialMode = NONE;
    awaitingInput = false;

    lcd.clear(); lcd.print("Transaksi Terak-");
    lcd.setCursor(0, 1); lcd.print("hir Dibatalkan");
    delay(2000);
    }
    return;
  }

  // Cancel editing min or price
  if ((specialMode == EDIT_MIN_VALUE || specialMode == EDIT_PRICE) && key == 'C') {
    specialMode = NONE;
    awaitingInput = false;
    specialInput = "";
    lcd.clear();
    lcd.print("Dibatalkan");
    delay(1500);
    return;
  }

  // Backspace input
  if ((specialMode == EDIT_MIN_VALUE || specialMode == EDIT_PRICE) && key == '#') {
    if (specialInput.length() > 0) {
      specialInput.remove(specialInput.length() - 1);
    }
  }

  // Confirm input with D
  else if ((specialMode == EDIT_MIN_VALUE || specialMode == EDIT_PRICE) && key == 'D') {
    if (specialInput.length() > 0) {
      float val = specialInput.toFloat();

      if (specialMode == EDIT_MIN_VALUE) {
        if (inputMode) minQuantity = val;
        else minPrice = val;
        savePersistentData();
        lcd.clear();
        lcd.print("Disimpan:");
        lcd.setCursor(0, 1);
        lcd.print(inputMode ? "Min " + String(minQuantity, 2) + " kg" : "Rp." + String(minPrice, 0));
      } else if (specialMode == EDIT_PRICE) {
        if (selectedServo == 1) pricePerKgA = val;
        else pricePerKgB = val;
        savePersistentData();
        lcd.clear();
        lcd.print("Disimpan:");
        lcd.setCursor(0, 1);
        lcd.print("Rp." + String(val, 0));
      }

      delay(2000);
      specialMode = NONE;
      awaitingInput = false;
      specialInput = "";
    }
    return;
  }

  // Append digit or dot
  else if (key >= '0' && key <= '9') {
    specialInput += key;
  } else if (key == '*') {
    if (specialInput.indexOf('.') == -1) specialInput += ".";
  } else {
    return; // ignore other keys
  }

  // Show updated input
  lcd.setCursor(0, 1);
  lcd.print("                "); // clear line
  lcd.setCursor(0, 1);

  if (specialMode == EDIT_MIN_VALUE) {
    lcd.print(inputMode ? "Min. " + specialInput + " kg" : "Rp." + specialInput);
  } else if (specialMode == EDIT_PRICE) {
    lcd.print("Rp." + specialInput);
  }
}

void handleWebserver(){
server.on("/", HTTP_GET, []() {
  String html = R"rawliteral(
    <!DOCTYPE html>
    <html>
    <head>
      <title>RiceDropper Config</title>
      <meta name="viewport" content="width=device-width, initial-scale=1">
      <style>
        body {
          font-family: Arial;
          background: #f4f4f4;
          padding: 20px;
        }

        .container {
          background: #fff;
          padding: 20px;
          border-radius: 8px;
          max-width: 500px;
          margin: 40px auto;
          box-shadow: 0 0 10px rgba(0,0,0,0.1);
        }

        input[type='number'], input[type='text'] {
          width: 100%;
          padding: 10px;
          margin: 10px 0;
          box-sizing: border-box;
        }

        label {
          display: block;
          margin-top: 15px;
        }

        form {
          margin-bottom: 20px;
        }

        button {
          padding: 10px 15px;
          background: #28a745;
          color: white;
          border: none;
          border-radius: 4px;
          cursor: pointer;
        }

        button:hover {
          background: #218838;
        }

        .danger {
          background: #dc3545;
        }

        .danger:hover {
          background: #c82333;
        }

        .summary {
          background: #e9ecef;
          padding: 10px;
          margin-top: 20px;
          border-radius: 5px;
        }

        .summary h3 {
          margin-top: 0;
        }
      </style>
    </head>
    <body>
      <div class="container">
        <h2>RiceDropper Configuration</h2>
        <form action="/update" method="POST">
          <label>Min Quantity (kg):</label>
          <input type="number" step="0.01" name="minQty" value="%MINQ%">
          <label>Min Price (Rp):</label>
          <input type="number" step="100" name="minPrice" value="%MINP%">
          <label>Price per Kg A (Rp):</label>
          <input type="number" step="100" name="priceA" value="%PRICEA%">
          <label>Price per Kg B (Rp):</label>
          <input type="number" step="100" name="priceB" value="%PRICEB%">
          <label>Calibration Factor:</label>
          <input type="number" step="1" name="calFactor" value="%CAL%">
          <button type="submit">Save</button>
        </form>
        <br>
        <form action="/reset" method="POST">
          <button class="danger" type="submit">Reset Dropped & Earnings</button>
        </form>
        <div class="summary">
          <h3>Total Dropped</h3>
          <p><strong>Beras:</strong> %TOTALKG% kg</p>
          <p><strong>Pendapatan:</strong> Rp.%TOTALEARN%</p>
        </div>
      </div>
    </body>
    </html>
  )rawliteral";

  html.replace("%MINQ%", String(minQuantity, 2));
  html.replace("%MINP%", String(minPrice, 0));
  html.replace("%PRICEA%", String(pricePerKgA, 0));
  html.replace("%PRICEB%", String(pricePerKgB, 0));
  html.replace("%CAL%", String(calFactor, 2));
  html.replace("%TOTALKG%", String(totalRiceDropped, 2));
  html.replace("%TOTALEARN%", String(totalEarnings, 0));

  server.send(200, "text/html", html);
});

  server.on("/update", HTTP_POST, []() {
    if (server.hasArg("minQty")) minQuantity = server.arg("minQty").toFloat();
    if (server.hasArg("minPrice")) minPrice = server.arg("minPrice").toFloat();
    if (server.hasArg("priceA")) pricePerKgA = server.arg("priceA").toFloat();
    if (server.hasArg("priceB")) pricePerKgB = server.arg("priceB").toFloat();
    if (server.hasArg("calFactor")) calFactor = server.arg("calFactor").toFloat();
    LoadCell.setCalFactor(calFactor);
    savePersistentData();
    server.sendHeader("Location", "/");
    server.send(303);
  });

  server.on("/reset", HTTP_POST, []() {
    totalRiceDropped = 0.0;
    totalEarnings = 0.0;
    savePersistentData();
    server.sendHeader("Location", "/");
    server.send(303);
  });
}

void setup() {
  Serial.begin(115200);
  EEPROM.begin(EEPROM_SIZE);
  initializeBuzzer();
  initializeLCD();
  loadPersistentData();
  initializeScale();
  initializeServo();

  keypad.setHoldTime(2000); // long press duration

  delay(300);
  WiFi.softAP(ssid, password);
  IPAddress IP = WiFi.softAPIP();
  handleWebserver();

  server.begin();
  lcd.clear(); lcd.print("AP IP:"); lcd.setCursor(0, 1); lcd.print(IP);
  delay(3000);
  lcd.clear(); lcd.print("Sistem Siap");
  lcd.setCursor(0,1); lcd.print("Masukkan Wadah");

  delay(2500);
  lcd.clear(); lcd.print("Lalu, Tekan");
  lcd.setCursor(0,1); lcd.print("Tombol Keypad");
}

void loop() {
  server.handleClient();
  LoadCell.update();

  keypad.getKeys();  // always update key states

  if (passwordMode == ENTER_PASS) {
    keypad.getKeys();

    static bool showPassword = false; // Tracks toggle state

    for (int i = 0; i < LIST_MAX; i++) {
      char key = keypad.key[i].kchar;
      KeyState state = keypad.key[i].kstate;

      if (state == PRESSED && keypad.key[i].stateChanged) {
        if (key >= '0' && key <= '9') {
          passwordInput += key;
        } else if (key == '#') {
          if (passwordInput.length() > 0)
            passwordInput.remove(passwordInput.length() - 1);
        } else if (key == 'C') {
          passwordInput = "";
        } else if (key == 'A') {
          showPassword = !showPassword;  // Toggle visibility
        } else if (key == 'D') {
          if (passwordInput == ADMIN_PASSWORD) {
            passwordMode = NONE_PASS;
            lcd.clear(); lcd.print("Password Benar");
            delay(1000);
            handleAuthorizedHeldKey(pendingKey);
            pendingKey = '\0';
            passwordInput = "";
            showPassword = false;
          } else {
            lcd.clear(); lcd.print("Password Salah");
            delay(1500);
            passwordMode = NONE_PASS;
            pendingKey = '\0';
            passwordInput = "";
            showPassword = false;
            lcd.clear(); lcd.print("Tekan Tombol");
            lcd.setCursor(0, 1); lcd.print("Untuk Memulai");
            awaitingInput = false;
          }
          return;
        }

        // Show password input (either * or plain)
        lcd.setCursor(0, 1);
        lcd.print("                "); // clear line
        lcd.setCursor(0, 1);
        if (showPassword) {
          lcd.print(passwordInput);
        } else {
          for (int i = 0; i < passwordInput.length(); i++) {
            lcd.print("*");
          }
        }
      }
    }

    return; // don't continue loop while in password mode
  }

  for (int i = 0; i < LIST_MAX; i++) {
    char key = keypad.key[i].kchar;
    KeyState state = keypad.key[i].kstate;

    // Handle long-press
    if (state == HOLD && specialMode == NONE && !awaitingInput) {
      handleHeldKey(key);
      return;
    }

    // Handle fresh key press (prevent multi-triggering)
    if (state == PRESSED && keypad.key[i].stateChanged) {
      if (specialMode != NONE && awaitingInput) {
        handleSpecialInput(key);
        return;
      } else if (specialMode == NONE) {
        processKey(key);
        return;
      }
    }
  }

  if (buzzerFailed) {
    unsigned long currentTime = millis();

    if (currentTime - buzzerStartTime >= 30000) {
      // 0.5 minute passed
      buzzerFailed = false;
      BuzzerActive(false);
    } else if (currentTime - lastBuzzerToggle >= 3000) {
      // Toggle every 3 seconds
      buzzerState = !buzzerState;
      BuzzerActive(buzzerState);
      lastBuzzerToggle = currentTime;
    }
  }
}
