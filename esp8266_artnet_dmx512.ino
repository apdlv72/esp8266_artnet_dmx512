
/*
  This sketch receives Art-Net data of one DMX universes over WiFi
  and sends it over the serial interface to a MAX485 module.

  It provides an interface between wireless Art-Net and wired DMX512.

*/

#include <ESP8266WiFi.h>         // https://github.com/esp8266/Arduino
#include <ESP8266WebServer.h>
#include <ESP8266mDNS.h>
#include <ArduinoOTA.h>
#include <WiFiManager.h>         // https://github.com/tzapu/WiFiManager
#include <WiFiClient.h>
#include <ArtnetWifi.h>          // https://github.com/rstephan/ArtnetWifi
#include <FS.h>

#include "setup_ota.h"
#include "send_break.h"

#define MIN(x,y) (x<y ? x : y)

#define ENABLE_OTA
#define ENABLE_MDNS
#define ENABLE_WEBINTERFACE
#define ENABLE_STATS

#define LED_B 16  // GPIO16/D0
#define LED_G 5   // GPIO05/D1
#define LED_R 4   // GPIO04/D2

//#define COMMON_ANODE
#ifdef COMMON_ANODE
#define ON  LOW
#define OFF HIGH
#define DIMMED 4095-100
#else 
#define ON  HIGH
#define OFF LOW
#define DIMMED 100
#endif

//const char* host = "😎ArtNet " __DATE__ "😍";
const char* host = "🎂 Tommy 🥳";
const char* version = __DATE__ " / " __TIME__;

unsigned long totalPacketsReceived = 0;
unsigned long packetsReceived = 0;

unsigned long totalFramesSent = 0;
unsigned long framesSent = 0;

// Global universe buffer
struct {
  uint16_t universe;
  uint16_t length;
  uint8_t sequence;
  uint8_t *data;
} global;

// keep track of the timing of the function calls
long tic_loop = 0;
long tic_stats = 0;
long tic_packet = 0;
long tic_web = 0;
float pps;
float fps;

long loopCount = 0;
boolean connectedToAccessPoint = false;

long lastLoop = millis();
long redOffTime = 0;

bool debugTiming = false;
bool wasReceivingPackets = false;

volatile byte strobeInterval = 0;
volatile boolean packetReceived = false;
volatile long lastPacketReceived = 0;

Config config;
ESP8266WebServer server(80);
ArtnetWifi artnet;
WiFiManager wifiManager;

void setup() {
  Serial1.begin(250000, SERIAL_8N2);
  Serial.begin(115200);
  while (!Serial) {
    ;
  }
  Serial.println("Starting setup");
  pinMode(LED_R, OUTPUT);
  pinMode(LED_G, OUTPUT);
  pinMode(LED_B, OUTPUT);
  singleWhite();  

  global.universe = 0;
  global.sequence = 0;
  global.length = 512;
  global.data = (uint8_t *)malloc(512);
  for (int i = 0; i < 512; i++) {
    global.data[i] = 0;
  }

  int R = 255;
  int G = 190;
  int B = 45;

  // Set defaults for 8 ceiling spots (warm white) at address 1
  for (int spot = -1; spot < 8; spot++) {

    singleWhite();  
    delay(125);
    allBlack();  
    delay(125);

    // A songle ceiling spots behind bar at has address 48
    int adr = spot < 0 ? 48 : 3 * spot;
    global.data[adr + 0] = R; // r
    global.data[adr + 1] = G; // g
    global.data[adr + 2] = B; // b

    sendInitial();
  }
  allBlack();

  Serial.println("Initiating configuration");
  SPIFFS.begin();
  initialConfig();
  
  Serial.print("Default FPS delay is ");
  Serial.print(config.delay);
  Serial.println(" ms");

  Serial.println("Loading initial configuration");
  if (loadConfig()) {
    Serial.println("Successfully loaded initial configuration");
    Serial.print("Configured FPS delay is ");
    Serial.print(config.delay);
    Serial.println(" ms");
    singleYellow();
  } else {
    Serial.println("Failed to load initial config");
    singleRed();
  }  
  delay(250);
  allBlack();

  if (WiFi.status() == WL_CONNECTED) {
    blueOn();
  } else {
    greenOn();
  }
  delay(250);
  allBlack();

  int CONFIG_TIMEOUT = 7 * 24 * 3600; // 1 week
  
  Serial.println("Starting WiFiManager");
  wifiManager.setDarkMode(true);
  wifiManager.setConfigPortalBlocking(false);
  wifiManager.setConfigPortalTimeout(CONFIG_TIMEOUT);
  // if this is set, it will exit after config, even if connection is unsuccessful.
  // TODO: what does that mean?
  // wifiManager.setBreakAfterConfig(true);
    
  WiFi.hostname(host);    
  wifiManager.setAPStaticIPConfig(IPAddress(192, 168, 111, 1), IPAddress(192, 168, 111, 1), IPAddress(255, 255, 255, 0));
  wifiManager.autoConnect(host);       
  
  if (WiFi.status() == WL_CONNECTED) {
    blueOn();
  } else {
    greenOn();
  }

#ifdef ENABLE_OTA
  Serial.println("Starting OTA");
  ArduinoOTA.setHostname("ArtnetDmx_Ota");
  ArduinoOTA.setPassword("test99");
  ArduinoOTA.onStart([]() { allBlack(); digitalWrite(LED_B, ON); } );
  ArduinoOTA.onError([](ota_error_t error) { 
      Serial.printf("Error[%u]: ", error); 
      allBlack(); 
      digitalWrite(LED_R, ON); 
    } 
  );
  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
    analogWrite(LED_B, 4096-(20*millis())%4096);
    Serial.printf("Progress: %u%%\r", (progress / (total / 100)));
  });
  ArduinoOTA.onEnd([]()   { 
    allBlack(); 
    digitalWrite(LED_G, ON); 
    delay(500);
    allBlack(); 
  });
  ArduinoOTA.begin();
  Serial.println("OTA started");
#endif

#ifdef ENABLE_WEBINTERFACE
  setupServer();
#endif

  // announce the hostname and web server through zeroconf
#ifdef ENABLE_MDNS
  MDNS.begin(host);
  MDNS.addService("http", "tcp", 80);
#endif

  Serial.println("Starting ArtNet");
  artnet.begin();
  artnet.setArtDmxCallback(onDmxPacket);
  Serial.println("ArtNet started");

  // initialize all timers
  tic_loop   = millis();
  tic_packet = millis();
  tic_stats    = millis();
  tic_web    = 0;

  Serial.println("Setup complete");
} // setup


void loop() {

  long now = millis();
  loopCount++;

  if (debugTiming) {
    long loopDelta = now-lastLoop;
    if (loopDelta>8) {
      Serial.print("loopDelta=");
      Serial.println(loopDelta);
    }
  }
  lastLoop = now;

  if (redOffTime>0 && now>=redOffTime) {
    redOff();
    redOffTime=0;
  }
  
  boolean isReceivingPackets = false;  
  if (lastPacketReceived>0) {
    long delta = now-lastPacketReceived;
    if (delta < 1*1000) {
      isReceivingPackets = true;
    }
  } 

  if (isReceivingPackets && !wasReceivingPackets) {
    singleWhite();
    Serial.println("DMX packet stream starts");
    delay(50);
    allBlack();
  }
  else if (!isReceivingPackets && wasReceivingPackets) {
    singleWhite();
    Serial.println("DMX packet stream stopped");
    delay(50);
    allBlack();
  }

  greenOff();
  blueOff();
  if (strobeInterval>0) { 
    blueOn();
    greenOn();
  } else if (connectedToAccessPoint) {
    blueOn();
  } else {
    greenOn();
  }

  // handle servers only when there is no DMX data
  if (!isReceivingPackets) {        

    //Serial.println("handle servers");
    wifiManager.process();
      
    if (WiFi.status() == WL_CONNECTED) {
      if (!connectedToAccessPoint) {
        Serial.println("**** CONNECTED TO ACCESS POINT! ****");  
        ArduinoOTA.begin();
        Serial.println("OTA restarted");
      }
      connectedToAccessPoint = true;
    } 
    
    ArduinoOTA.handle();
    server.handleClient();
  }
  
  artnet.read();
  if (packetReceived) {    
    redOn();
//    for (int i=0; i<512; i++) {
//      if (global.data[i]>0) {
//        Serial.print("recv "); Serial.print(i);
//        Serial.print(" value "); Serial.println(global.data[i]);
//      }
//    }
    redOffTime = now+3; // flash red for 10 ms
  }

  // this section gets executed at a maximum rate of around 40Hz
  if ((millis() - tic_loop) >= config.delay) {
    tic_loop = millis();
    
    totalFramesSent++;
    framesSent++;

    bool clearStrobe = false;
    if (strobeInterval > 0) {

      int divisor = (256 - strobeInterval) / 20;
      divisor++; // avoid division by zero      

      if (0 == totalFramesSent % divisor) {
        // set the original values below, then clear immediately after
        clearStrobe = true;
      } else {
        blankStrobeChannels();
      }
    }

//    for (int i=0; i<512; i++) {
//      if (global.data[i]>0) {
//        Serial.print("send "); Serial.print(i);
//        Serial.print(" value "); Serial.println(global.data[i]);
//      }
//    }

    sendBreak();
    Serial1.write(0); // Start-Byte
    // send out the value of the selected channels (up to 512)
    for (int i = 0; i < MIN(global.length, config.channels); i++) {
      Serial1.write(global.data[i]);
    }

    if (clearStrobe) {
      blankStrobeChannels();
      sendBreak();
      Serial1.write(0); // Start-Byte
      // send out the value of the selected channels (up to 512)
      for (int i = 0; i < MIN(global.length, config.channels); i++) {
        Serial1.write(global.data[i]);
      }
    }
  }

  if (packetReceived) {    
    printStats(loopCount);
  }
  
  if (debugTiming) {
    long after = millis();
    long delta = after-now;
    if (delta>10) {
      Serial.print("delta=");
      Serial.println(delta);
    }
  }

  packetReceived = false;
  wasReceivingPackets = isReceivingPackets;
  
} // loop

void blankStrobeChannels() {
      // 9 ceiling lamps with RGB 
      for (int i = 0; i < 9 * 3; i++) {
        global.data[i] = 0;
      }

      // the RGBW array's master dimmers:
      global.data[63 + 0*8] = 0;
      global.data[63 + 1*8] = 0;
      global.data[63 + 2*8] = 0;
      global.data[63 + 3*8] = 0;  
}

//this will be called for each UDP packet received
void onDmxPacket(uint16_t universe, uint16_t length, uint8_t sequence, uint8_t * data) {

  lastPacketReceived = millis();
  packetReceived = true;
  totalPacketsReceived++;
  packetsReceived++;

  if (universe == config.universe) {
    // copy the data from the UDP packet over to the global universe buffer
    global.universe = universe;
    global.sequence = sequence;
    if (length < 512)
      global.length = length;
    for (int i = 0; i < global.length; i++) {
      global.data[i] = data[i];
    }
    strobeInterval = data[200 - 1];
  }
} // onDmxpacket

void sendInitial() {
  for (int n = 0; n < 3; n++) {
    sendBreak();
    Serial1.write(0); // Start-Byte
    // send out the value of the selected channels (up to 256)
    for (int i = 0; i < 256; i++) {
      Serial1.write(global.data[i]);
    }
    delay(1);
  }
}

void setupServer() {
  // this serves all URIs that can be resolved to a file on the SPIFFS filesystem
  server.onNotFound(handleNotFound);

  server.on("/", HTTP_GET, []() {
    tic_web = millis();
    redOn();
    Serial.println("GET /");
    handleRedirect("/index");
    redOffTime = millis()+50;
  });

  server.on("/index", HTTP_GET, []() {
    tic_web = millis();
    redOn();
    Serial.println("GET /index");
    handleStaticFile("/index.html");
    redOffTime = millis()+50;
  });

  server.on("/defaults", HTTP_GET, []() {
    tic_web = millis();
    redOn();
    Serial.println("GET /defaults");
    handleStaticFile("/reload_success.html");
    delay(2000);
    singleRed();
    initialConfig();
    saveConfig();
    WiFiManager wifiManager;
    wifiManager.resetSettings();
    WiFi.hostname(host);
    redOff();
    ESP.restart();
  });

  server.on("/reconnect", HTTP_GET, []() {
    tic_web = millis();
    redOn();
    Serial.println("GET /reconnect");
    handleStaticFile("/reload_success.html");
    delay(2000);
    singleRed();
    WiFiManager wifiManager;
    wifiManager.setAPStaticIPConfig(IPAddress(192, 168, 1, 1), IPAddress(192, 168, 1, 1), IPAddress(255, 255, 255, 0));
    wifiManager.startConfigPortal(host);
    Serial.println("connected");
    if (WiFi.status() == WL_CONNECTED) {
      singleGreen();
    }
    redOffTime = millis()+50;
  });

  server.on("/reset", HTTP_GET, []() {
    tic_web = millis();
    Serial.println("/reset");
    handleStaticFile("GET /reload_success.html");
    delay(2000);
    singleRed();
    ESP.restart();
  });

  server.on("/monitor", HTTP_GET, [] {
    tic_web = millis();
    redOn();
    Serial.println("GET /monitor");
    handleStaticFile("/monitor.html");
    redOffTime = millis()+50;
  });

  server.on("/hello", HTTP_GET, [] {
    tic_web = millis();
    redOn();
    Serial.println("GET /hello");
    handleStaticFile("/hello.html");
    redOffTime = millis()+50;
  });

  server.on("/settings", HTTP_GET, [] {
    tic_web = millis();
    redOn();
    Serial.println("GET /settings");
    handleStaticFile("/settings.html");
    redOffTime = millis()+50;
  });

  server.on("/dir", HTTP_GET, [] {
    tic_web = millis();
    redOn();
    Serial.println("GET /dir");
    handleDirList();
    redOffTime = millis()+50;
  });

  server.on("/json", HTTP_PUT, [] {
    tic_web = millis();
    redOn();
    Serial.println("PUT /json");
    handleJSON();
    redOffTime = millis()+50;
  });

  server.on("/json", HTTP_POST, [] {
    tic_web = millis();
    redOn();
    Serial.println("POST /json");
    handleJSON();
    redOffTime = millis()+50;
  });

  server.on("/json", HTTP_GET, [] {
    tic_web = millis();
    redOn();
    Serial.println("GET /json");
    DynamicJsonDocument root(300);
    CONFIG_TO_JSON(universe, "universe");
    CONFIG_TO_JSON(channels, "channels");
    CONFIG_TO_JSON(delay, "delay");
    root["version"] = version;
    root["uptime"]  = long(millis() / 1000);
    root["packets"] = totalPacketsReceived;
    root["frames"]  = totalFramesSent;
    root["pps"] = pps;
    root["fps"] = fps;
    String str;
    serializeJson(root, str);
    server.send(200, "application/json", str);
    redOffTime = millis()+50;
  });

  server.on("/update", HTTP_GET, [] {
    tic_web = millis();
    redOn();
    Serial.println("GET /update");
    handleStaticFile("/update.html");
    redOffTime = millis()+50;
  });

  server.on("/favicon.ico", HTTP_GET, [] {
    tic_web = millis();
    redOn();
    Serial.println("GET /favicon.ico");
    handleStaticFile("/favicon.ico");
    redOffTime = millis()+50;
  });

  server.on("/logo.png", HTTP_GET, [] {
    tic_web = millis();
    redOn();
    Serial.println("GET /logo.png");
    handleStaticFile("/logo.png");
    redOffTime = millis()+50;
  });

  server.on("/update", HTTP_POST, handleUpdate1, handleUpdate2);
  
  // start the web server
  Serial.println("Starting web interface");
  server.begin();  
  Serial.println("Web interface started");
}

void printStats(long loopCount) {
  long now = millis();
  long delta = now - tic_stats;
  // don't estimate the FPS too frequently. every 10 seconds
  if (delta > 10000) {
    pps  = 1000 * packetsReceived / delta;
    fps = 1000 * framesSent / delta;
    tic_stats = millis();
#ifdef ENABLE_STATS
    Serial.print("now=");
    Serial.print(now);
    Serial.print(",delta=");
    Serial.print(delta);
    Serial.print(",loopCount=");
    Serial.print(loopCount);
    Serial.print(",totalPacketsReceived=");
    Serial.print(totalPacketsReceived);
    Serial.print(",totalFramesSent=");
    Serial.print(totalFramesSent);
    Serial.print(",packetsReceived=");
    Serial.print(packetsReceived);
    Serial.print(",framesSent=");
    Serial.print(framesSent);
    Serial.print(",pps=");
    Serial.print(pps);
    Serial.print(",fps=");
    Serial.print(fps);
    Serial.print(",strobeInterval=");
    Serial.print(strobeInterval);
    Serial.println();
#endif    
    packetsReceived = 0;
    framesSent = 0;
  }
} 

#ifdef COMMON_ANODE
#define ON  LOW
#define OFF HIGH
#define DIMMED_G 4095-10
#define DIMMED_B 4095-100
#else 
#define ON  HIGH
#define OFF LOW
#define DIMMED_G 10
#define DIMMED_B 100
#endif

void singleWhite() {
  digitalWrite(LED_R, ON);
  digitalWrite(LED_G, ON);
  digitalWrite(LED_B, ON);
}

void singleRed() {
  digitalWrite(LED_R, ON);
  digitalWrite(LED_G, OFF);
  digitalWrite(LED_B, OFF);
}

void redOn() {
  digitalWrite(LED_R, ON);
}

void greenOn() {
  analogWrite(LED_G, DIMMED_G);
}

void blueOn() {
  analogWrite(LED_B, DIMMED_B);
}

void redOff() {
  digitalWrite(LED_R, OFF);
}

void greenOff() {
  digitalWrite(LED_G, OFF);
}

void blueOff() {
  digitalWrite(LED_B, OFF);
}

void singleGreen() {
  digitalWrite(LED_R, OFF);
  digitalWrite(LED_G, ON);
  digitalWrite(LED_B, OFF);
}

void singleBlue() {
  digitalWrite(LED_R, OFF);
  digitalWrite(LED_G, OFF);
  digitalWrite(LED_B, ON);
}

void singleYellow() {
  digitalWrite(LED_R, ON);
  digitalWrite(LED_G, ON);
  digitalWrite(LED_B, OFF);
}

void allBlack() {
  digitalWrite(LED_R, OFF);
  digitalWrite(LED_G, OFF);
  digitalWrite(LED_B, OFF);
}
