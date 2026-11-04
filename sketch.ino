#include <Arduino.h>
#include <SPI.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>
#include <GxEPD2_3C.h>
#include <Fonts/FreeMonoBold9pt7b.h>


#define EPD_CS 10
#define EPD_DC 9
#define EPD_RST 8
#define EPD_BUSY 7
#define EPD_SRAM_CS 6
#define EPD_SD_CS 5
#define EPD_SCLK 12
#define EPD_MISO 13
#define EPD_MOSI 11
GxEPD2_3C<GxEPD2_290_T94_V2, GxEPD2_290_T94_V2::HEIGHT> display(
  GxEPD2_290_T94_V2(EPD_CS, EPD_DC, EPD_RST, EPD_BUSY));

static const char* SERVICE_UUID = "7d2ea28a-f7bd-485a-bd9d-92ad6ecfe93e";
static const char* DATA_UUID    = "7d2ea28b-f7bd-485a-bd9d-92ad6ecfe93e";
static const char* STATUS_UUID  = "7d2ea28c-f7bd-485a-bd9d-92ad6ecfe93e";
BLECharacteristic* statusChar;

struct Task { String text; bool done; uint32_t created; } tasks[7];
uint8_t taskCount = 0;
int64_t epochAtSync = 0;
uint32_t millisAtSync = 0;
bool haveTime = false, dirty = true;
const char* quotes[] = {"Make today count.", "One step at a time.", "Begin where you are.", "Keep going."};

int64_t nowEpoch() { return haveTime ? epochAtSync + (uint32_t)(millis() - millisAtSync) / 1000 : 0; }
void reply(const String& s) { if (statusChar) { statusChar->setValue(s.c_str()); statusChar->notify(); } }

class DataCallbacks : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic* c) override {
    String s = c->getValue().c_str(); s.trim();
    if (s.startsWith("TIME:")) {
      int64_t t = strtoll(s.substring(5).c_str(), nullptr, 10);
      if (t > 0) { epochAtSync=t; millisAtSync=millis(); haveTime=true; reply("OK:TIME"); }
      else reply("ERR:TIME");
      return;
    }
    if (s.startsWith("LIST:")) {
      String body=s.substring(5); taskCount=0; int start=0;
      while (start <= body.length() && taskCount < 7) {
        int end=body.indexOf('|',start); if(end<0) end=body.length();
        String item=body.substring(start,end); item.trim();
        tasks[taskCount++]={item,false,(uint32_t)(nowEpoch())};
        start=end+1; if(end>=body.length()) break;
      }
      dirty=true; reply("OK:LIST"); return;
    }
    if (s.startsWith("DONE:")) {
      int i=s.substring(5).toInt();
      if(i<0 || i>=taskCount) { reply("ERR:INDEX"); return; }
      if(haveTime && nowEpoch()-(int64_t)tasks[i].created >= 86400) { reply("410:EXPIRED"); return; }
      tasks[i].done=true; dirty=true; reply("OK:DONE"); return;
    }
    reply("ERR:COMMAND");
  }
};

void drawScreen() {
  display.setFullWindow(); display.firstPage();
  do {
    display.fillScreen(GxEPD_WHITE); display.setTextColor(GxEPD_BLACK);
    display.setFont(&FreeMonoBold9pt7b); display.setCursor(4,18);
    if(haveTime) { int64_t t=nowEpoch(); char b[24]; snprintf(b,sizeof(b),"%02lld:%02lld  %02lld/%02lld",(long long)((t/3600)%24),(long long)((t/60)%60),(long long)((t/86400)%31+1),(long long)((t/2592000)%12+1)); display.print(b); }
    else display.print("Time: sync phone");
    display.drawLine(0,24,296,24,GxEPD_BLACK);
    if(!taskCount) { display.setCursor(4,48); display.setTextColor(GxEPD_RED); display.print(quotes[(millis()/60000)%4]); }
    for(uint8_t i=0;i<taskCount && i<7;i++) {
      int y=44+i*25; display.drawRect(4,y-12,12,12,GxEPD_BLACK);
      if(tasks[i].done) { display.drawLine(5,y-8,14,y-1,GxEPD_RED); display.drawLine(14,y-8,5,y-1,GxEPD_RED); }
      display.setCursor(22,y-2); display.setTextColor(tasks[i].done?GxEPD_RED:GxEPD_BLACK);
      String text=tasks[i].text; if(text.length()>31) text=text.substring(0,31); display.print(text);
    }
  } while(display.nextPage());
  dirty=false;
}

void setup() {
  Serial.begin(115200); pinMode(EPD_SRAM_CS,OUTPUT); digitalWrite(EPD_SRAM_CS,HIGH);
  pinMode(EPD_SD_CS,OUTPUT); digitalWrite(EPD_SD_CS,HIGH);
  SPI.begin(EPD_SCLK,EPD_MISO,EPD_MOSI,EPD_CS);
  display.init(115200); display.setRotation(1);
  BLEDevice::init("ToDos Watch"); BLEServer* server=BLEDevice::createServer();
  BLEService* service=server->createService(SERVICE_UUID);
  BLECharacteristic* data=service->createCharacteristic(DATA_UUID,BLECharacteristic::PROPERTY_WRITE);
  data->setCallbacks(new DataCallbacks());
  statusChar=service->createCharacteristic(STATUS_UUID,BLECharacteristic::PROPERTY_READ|BLECharacteristic::PROPERTY_NOTIFY);
  statusChar->addDescriptor(new BLE2902()); statusChar->setValue("READY");
  service->start(); server->getAdvertising()->start(); drawScreen();
}
void loop() {
  static uint32_t lastClock=0;
  if(millis()-lastClock>=60000) { lastClock=millis(); dirty=true; }
  if(dirty) drawScreen();
  delay(10);
}
