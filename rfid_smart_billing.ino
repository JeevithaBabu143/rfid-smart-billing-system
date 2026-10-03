/*
  RFID Smart Shopping Cart - Automatic Billing System
  Board    : Arduino Uno / Nano
  Modules  : RC522 RFID reader, 16x2 I2C LCD, HC-05 Bluetooth, SIM800L GSM,
             2 push buttons, buzzer

  How it works
    1. Scan a product tag  -> item is added, price shown on LCD, total updates
    2. Press REMOVE button -> remove mode ON, scan a tag to remove that item
    3. Press PAY button    -> bill is sent to phone (Bluetooth Terminal app)
                              and as SMS (GSM), then the cart resets

  Bluetooth Terminal app (Android: "Serial Bluetooth Terminal")
    Pair HC-05 (PIN 1234 or 0000), baud 9600. Commands you can type:
      B = show bill     C = checkout     R = reset cart     H = help

  Libraries (Library Manager):
    "MFRC522" by GithubCommunity / Miguel Balboa
    "LiquidCrystal I2C" by Frank de Brabander
    SoftwareSerial (built in)

  Pins
    RC522  : SDA(SS)=D10, RST=D9, MOSI=D11, MISO=D12, SCK=D13, VCC=3.3V
    LCD I2C: SDA=A4, SCL=A5, VCC=5V
    HC-05  : TXD->D2, RXD<-D3 (use 1k+2k divider on RXD)
    SIM800L: TX->D4, RX<-D5 (use divider), VCC = 3.7-4.2V (separate supply)
    Buttons: REMOVE=D6, PAY=D7 (other leg to GND)
    Buzzer : D8
*/

#include <SPI.h>
#include <MFRC522.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <SoftwareSerial.h>

// ---------- Pins ----------
#define SS_PIN      10
#define RST_PIN     9
#define BT_RX       2    // Arduino RX  <- HC-05 TXD
#define BT_TX       3    // Arduino TX  -> HC-05 RXD (through divider)
#define GSM_RX      4    // Arduino RX  <- SIM800L TX
#define GSM_TX      5    // Arduino TX  -> SIM800L RX (through divider)
#define REMOVE_BTN  6
#define PAY_BTN     7
#define BUZZER      8

// ---------- Settings ----------
const char PHONE_NUMBER[] = "+91XXXXXXXXXX";   // number that receives the SMS bill

// ---------- Product table ----------
// Scan an unknown tag: its UID is printed on the Serial Monitor and in the
// Bluetooth Terminal. Copy that UID here with the product name and price.
#define NUM_PRODUCTS 4
struct Product {
  byte uid[4];
  const char* name;
  int price;
};
Product products[NUM_PRODUCTS] = {
  {{0xAA, 0xBB, 0xCC, 0xDD}, "Milk",     30},
  {{0x11, 0x22, 0x33, 0x44}, "Bread",    25},
  {{0x55, 0x66, 0x77, 0x88}, "Rice 1kg", 60},
  {{0x99, 0x00, 0xAB, 0xCD}, "Soap",     40}
};

// ---------- Objects ----------
MFRC522 rfid(SS_PIN, RST_PIN);
LiquidCrystal_I2C lcd(0x27, 16, 2);      // change to 0x3F if LCD stays blank
SoftwareSerial bt(BT_RX, BT_TX);
SoftwareSerial gsm(GSM_RX, GSM_TX);

// ---------- State ----------
int  qty[NUM_PRODUCTS];
long total = 0;
bool removeMode = false;
unsigned long lastButton = 0;

// ---------- Helpers ----------
void beep(int times) {
  for (int i = 0; i < times; i++) {
    digitalWrite(BUZZER, HIGH);
    delay(80);
    digitalWrite(BUZZER, LOW);
    delay(80);
  }
}

void showLine(byte row, const char* text) {
  lcd.setCursor(0, row);
  lcd.print("                ");
  lcd.setCursor(0, row);
  lcd.print(text);
}

void showTotal() {
  char buf[17];
  snprintf(buf, sizeof(buf), "Total: Rs.%ld", total);
  showLine(1, buf);
}

void printUID(Print &p) {
  for (byte i = 0; i < rfid.uid.size; i++) {
    if (rfid.uid.uidByte[i] < 0x10) p.print('0');
    p.print(rfid.uid.uidByte[i], HEX);
    p.print(' ');
  }
  p.println();
}

int findProduct() {
  if (rfid.uid.size < 4) return -1;
  for (int i = 0; i < NUM_PRODUCTS; i++) {
    bool match = true;
    for (byte k = 0; k < 4; k++) {
      if (rfid.uid.uidByte[k] != products[i].uid[k]) { match = false; break; }
    }
    if (match) return i;
  }
  return -1;
}

void printBill(Print &p) {
  p.println(F("------ BILL ------"));
  for (int i = 0; i < NUM_PRODUCTS; i++) {
    if (qty[i] > 0) {
      p.print(products[i].name);
      p.print(F(" x"));
      p.print(qty[i]);
      p.print(F(" = Rs."));
      p.println((long)qty[i] * products[i].price);
    }
  }
  p.print(F("TOTAL: Rs."));
  p.println(total);
  p.println(F("------------------"));
}

void resetCart() {
  for (int i = 0; i < NUM_PRODUCTS; i++) qty[i] = 0;
  total = 0;
  removeMode = false;
  lcd.clear();
  showLine(0, "Scan items...");
  showTotal();
}

// ---------- Cart actions ----------
void addItem(int i) {
  qty[i]++;
  total += products[i].price;
  char buf[17];
  snprintf(buf, sizeof(buf), "+%s Rs.%d", products[i].name, products[i].price);
  showLine(0, buf);
  showTotal();
  beep(1);
  bt.print(F("Added: "));
  bt.print(products[i].name);
  bt.print(F("  Total: Rs."));
  bt.println(total);
}

void removeItem(int i) {
  if (qty[i] > 0) {
    qty[i]--;
    total -= products[i].price;
    char buf[17];
    snprintf(buf, sizeof(buf), "-%s Rs.%d", products[i].name, products[i].price);
    showLine(0, buf);
    showTotal();
    beep(2);
    bt.print(F("Removed: "));
    bt.print(products[i].name);
    bt.print(F("  Total: Rs."));
    bt.println(total);
  } else {
    showLine(0, "Not in cart");
    beep(3);
  }
}

void sendSMS() {
  gsm.listen();                         // only one SoftwareSerial listens at a time
  gsm.println(F("AT+CMGF=1"));
  delay(500);
  gsm.print(F("AT+CMGS=\""));
  gsm.print(PHONE_NUMBER);
  gsm.println(F("\""));
  delay(500);
  gsm.print(F("Smart Cart Bill. Total: Rs."));
  gsm.print(total);
  gsm.write(26);                        // Ctrl+Z sends the message
  delay(3000);
  bt.listen();                          // back to Bluetooth
}

void checkout() {
  if (total == 0) {
    showLine(0, "Cart is empty");
    beep(3);
    return;
  }
  printBill(bt);
  printBill(Serial);
  showLine(0, "Sending bill...");
  sendSMS();
  showLine(0, "Payment done");
  showLine(1, "Thank you!");
  beep(2);
  delay(2500);
  resetCart();
}

// ---------- Input handling ----------
void handleButtons() {
  if (millis() - lastButton < 300) return;      // simple debounce

  if (digitalRead(REMOVE_BTN) == LOW) {
    lastButton = millis();
    removeMode = !removeMode;
    showLine(0, removeMode ? "REMOVE MODE" : "ADD MODE");
    showTotal();
    beep(1);
  }
  if (digitalRead(PAY_BTN) == LOW) {
    lastButton = millis();
    checkout();
  }
}

void handleBluetooth() {
  while (bt.available()) {
    char c = toupper(bt.read());
    if (c == 'B') {
      printBill(bt);
    } else if (c == 'C') {
      checkout();
    } else if (c == 'R') {
      resetCart();
      bt.println(F("Cart cleared"));
    } else if (c == 'H') {
      bt.println(F("B=bill  C=checkout  R=reset  H=help"));
    }
  }
}

// ---------- Setup / Loop ----------
void setup() {
  Serial.begin(9600);
  bt.begin(9600);
  gsm.begin(9600);

  pinMode(REMOVE_BTN, INPUT_PULLUP);
  pinMode(PAY_BTN, INPUT_PULLUP);
  pinMode(BUZZER, OUTPUT);

  SPI.begin();
  rfid.PCD_Init();

  lcd.init();
  lcd.backlight();
  showLine(0, "Smart Cart");
  showLine(1, "Starting...");
  delay(1500);

  bt.listen();
  resetCart();
  bt.println(F("Smart Cart ready. B=bill C=checkout R=reset H=help"));
  beep(1);
}

void loop() {
  handleBluetooth();
  handleButtons();

  if (!rfid.PICC_IsNewCardPresent() || !rfid.PICC_ReadCardSerial()) return;

  int idx = findProduct();
  if (idx >= 0) {
    if (removeMode) removeItem(idx);
    else            addItem(idx);
  } else {
    showLine(0, "Unknown tag");
    beep(3);
    Serial.print(F("Unknown tag UID: "));
    printUID(Serial);
    bt.print(F("Unknown tag UID: "));
    printUID(bt);
  }

  rfid.PICC_HaltA();
  rfid.PCD_StopCrypto1();
  delay(700);                           // avoid reading the same tag twice
}
