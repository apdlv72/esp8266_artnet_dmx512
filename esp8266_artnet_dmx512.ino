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
#define ENABLE_MDNS
#define ENABLE_WEBINTERFACE
// #define COMMON_ANODE

Config config;
ESP8266WebServer server(80);
const char* host = "ARTNET_APdlV";
const char* version = __DATE__ " / " __TIME__;

#define LED_B 16  // GPIO16/D0
#define LED_G 5   // GPIO05/D1
#define LED_R 4   // GPIO04/D2

// Artnet settings
ArtnetWifi artnet;
unsigned long totalFramesReceived = 0;
unsigned long framesReceived = 0;

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
long tic_loop = 0, tic_fps = 0, tic_packet = 0, tic_web = 0;

byte strobeInterval = 0;

boolean packetReceived = false;
float fps_in, fps_out;

void printStats() {
  long delta = millis() - tic_fps;
  // every 5 seconds
  if (delta > 5000) {
    Serial.print("millis=");
    Serial.print(delta);
    Serial.print(",totalFramesSent=");
    Serial.print(totalFramesSent);
    Serial.print(",framesSent=");
    Serial.print(framesSent);
    Serial.print(",framesReceived=");
    Serial.print(framesReceived);
    Serial.print(",totalFramesReceived=");
    Serial.print(totalFramesReceived);
    Serial.print(",strobeInterval=");
    Serial.print(strobeInterval);
    // don't estimate the FPS too frequently
    fps_in  = 1000 * framesReceived / delta;
    fps_out = 1000 * framesSent / delta;
    tic_fps = millis();
    framesReceived = 0;
    framesSent = 0;
    Serial.print(",fps_in=");
    Serial.print(fps_in);
    Serial.print(",fps_out=");
    Serial.print(fps_out);
    Serial.println();
  }
} 

//this will be called for each UDP packet received
void onDmxPacket(uint16_t universe, uint16_t length, uint8_t sequence, uint8_t * data) {

  packetReceived = true;
  totalFramesReceived++;
  framesReceived++;

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

  /*
    Serial.print("universe = ");
    Serial.print(universe);
    Serial.print(", sequence = ");
    Serial.print(sequence);
    Serial.print(", data[40] = ");
    Serial.println(data[40]);
  */
  //greenOff();
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

WiFiManager wifiManager;


void setup() {
  Serial1.begin(250000, SERIAL_8N2);
  Serial.begin(115200);
  while (!Serial) {
    ;
  }
  Serial.println("setup starting");
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

  // defaults for 8 ceiling spots (warm white) at address 1
  for (int spot = -1; spot < 8; spot++) {

    singleWhite();  
    delay(125);
    allBlack();  
    delay(125);

    // ceiling spots behind bar at address 48
    int adr = spot < 0 ? 48 : 3 * spot;
    global.data[adr + 0] = R; // r
    global.data[adr + 1] = G; // g
    global.data[adr + 2] = B; // b

    sendInitial();
  }
  allBlack();

  Serial.println("starting SPIFFS");
  SPIFFS.begin();

  Serial.println("initial config");
  initialConfig();
  Serial.print("delay: ");
  Serial.println(config.delay);

  if (loadConfig()) {
    singleYellow();
  }
  else {
    singleRed();
  }
  delay(500);
  allBlack();

  if (WiFi.status() != WL_CONNECTED) {
    singleRed();
  } else {
    singleGreen();
  }

  int CONFIG_TIMEOUT = 10;
  
  Serial.println("starting WiFiManager");
  wifiManager.setDarkMode(true);
  wifiManager.setConfigPortalBlocking(false);
  wifiManager.setConfigPortalTimeout(CONFIG_TIMEOUT);

  //if this is set, it will exit after config, even if connection is unsuccessful.
  // TODO: what does that mean?
  // wifiManager.setBreakAfterConfig(true);
    
  // wifiManager.resetSettings();
  WiFi.hostname(host);
    
  wifiManager.setAPStaticIPConfig(IPAddress(192, 168, 1, 1), IPAddress(192, 168, 1, 1), IPAddress(255, 255, 255, 0));
  wifiManager.autoConnect(host);       
  
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("connected");    
    singleGreen();
  } 
  else {
    Serial.println("NOT connected");    
    singleRed();
  }

  //if (WiFi.status() == WL_CONNECTED) 
  {
    ArduinoOTA.setHostname("ArtnetDmxOTA");
    ArduinoOTA.begin();
  }


#ifdef ENABLE_WEBINTERFACE
  // this serves all URIs that can be resolved to a file on the SPIFFS filesystem
  server.onNotFound(handleNotFound);

  server.on("/", HTTP_GET, []() {
    tic_web = millis();
    handleRedirect("/index");
  });

  server.on("/index", HTTP_GET, []() {
    tic_web = millis();
    handleStaticFile("/index.html");
  });

  server.on("/defaults", HTTP_GET, []() {
    tic_web = millis();
    Serial.println("handleDefaults");
    handleStaticFile("/reload_success.html");
    delay(2000);
    singleRed();
    initialConfig();
    saveConfig();
    WiFiManager wifiManager;
    wifiManager.resetSettings();
    WiFi.hostname(host);
    ESP.restart();
  });

  server.on("/reconnect", HTTP_GET, []() {
    tic_web = millis();
    Serial.println("handleReconnect");
    handleStaticFile("/reload_success.html");
    delay(2000);
    singleRed();
    WiFiManager wifiManager;
    wifiManager.setAPStaticIPConfig(IPAddress(192, 168, 1, 1), IPAddress(192, 168, 1, 1), IPAddress(255, 255, 255, 0));
    wifiManager.startConfigPortal(host);
    Serial.println("connected");
    if (WiFi.status() == WL_CONNECTED)
      singleGreen();
  });

  server.on("/reset", HTTP_GET, []() {
    tic_web = millis();
    Serial.println("handleReset");
    handleStaticFile("/reload_success.html");
    delay(2000);
    singleRed();
    ESP.restart();
  });

  server.on("/monitor", HTTP_GET, [] {
    tic_web = millis();
    handleStaticFile("/monitor.html");
  });

  server.on("/hello", HTTP_GET, [] {
    tic_web = millis();
    handleStaticFile("/hello.html");
  });

  server.on("/settings", HTTP_GET, [] {
    tic_web = millis();
    handleStaticFile("/settings.html");
  });

  server.on("/dir", HTTP_GET, [] {
    tic_web = millis();
    handleDirList();
  });

  server.on("/json", HTTP_PUT, [] {
    tic_web = millis();
    handleJSON();
  });

  server.on("/json", HTTP_POST, [] {
    tic_web = millis();
    handleJSON();
  });

  server.on("/json", HTTP_GET, [] {
    tic_web = millis();
    DynamicJsonDocument root(300);
    CONFIG_TO_JSON(universe, "universe");
    CONFIG_TO_JSON(channels, "channels");
    CONFIG_TO_JSON(delay, "delay");
    root["version"] = version;
    root["uptime"]  = long(millis() / 1000);
    root["packets"] = framesReceived;
    root["fps_out"] = fps_out;
    root["fps_in"] = fps_in;
    String str;
    serializeJson(root, str);
    server.send(200, "application/json", str);
  });

  server.on("/update", HTTP_GET, [] {
    tic_web = millis();
    handleStaticFile("/update.html");
  });

  server.on("/update", HTTP_POST, handleUpdate1, handleUpdate2);

  // start the web server
  server.begin();
#endif

  // announce the hostname and web server through zeroconf
#ifdef ENABLE_MDNS
  MDNS.begin(host);
  MDNS.addService("http", "tcp", 80);
#endif

  artnet.begin();
  artnet.setArtDmxCallback(onDmxPacket);
  Serial.println("artnet started");

  // initialize all timers
  tic_loop   = millis();
  tic_packet = millis();
  tic_fps    = millis();
  tic_web    = 0;

  Serial.println("setup done");
} // setup


int loops = 0;
boolean firstConnect = true;
boolean handleServer = true;

long lastLoop = millis();

void loop() {

  long currLoop = millis();
  long loopDelta = currLoop-lastLoop;
  if (loopDelta>8) {
    Serial.print("loopDelta=");
    Serial.println(loopDelta);
  }
  lastLoop = currLoop;

  long t0 = millis();
  
  allBlack();

  boolean connected = false;
  if (handleServer) {        
    
    wifiManager.process();
  
    if (WiFi.status() == WL_CONNECTED) {
      connected = true;
      if (firstConnect) {
        Serial.println("**** CONNECTED! ****");  
      }
      firstConnect = false;
    } 
    
    ArduinoOTA.handle();
    server.handleClient();
  }

  loops++;
  
  //else  
  // execute always (when device is working as a standalone access point)
  {
    artnet.read();

    // this section gets executed at a maximum rate of around 40Hz
    if ((millis() - tic_loop) >= config.delay) {
      tic_loop = millis();
      
      totalFramesSent++;
      framesSent++;

      int strobe = 0;
      if (strobeInterval > 0) {

        int divisor = (256 - strobeInterval) / 20;
        divisor++; // avoid division by zero

        if (0 == totalFramesSent % divisor) {
          strobe = 255;
        }
        for (int i = 0; i < 17 * 3; i++) {
          global.data[i] = strobe;
        }
      }

      sendBreak();

      Serial1.write(0); // Start-Byte
      // send out the value of the selected channels (up to 512)
      for (int i = 0; i < MIN(global.length, config.channels); i++) {
        Serial1.write(global.data[i]);
      }

      if (strobe > 0) {
        for (int i = 0; i < 17 * 3; i++) {
          global.data[i] = 0;
        }

        sendBreak();

        Serial1.write(0); // Start-Byte
        // send out the value of the selected channels (up to 512)
        for (int i = 0; i < MIN(global.length, config.channels); i++) {
          Serial1.write(global.data[i]);
        }
      }

    }
  }

  if (packetReceived) {    
    printStats();

    if (handleServer) {
      Serial.println("First DMX packet received. Stop handling server");
      handleServer = false;

      server.stop();
      //wifiManager.stopWebPortal();
      //wifiManager.stopConfigPortal();
    }
    if (connected) {
      singleBlue();
    } else {
      singleYellow();
    }
  }
  delay(1);
  packetReceived = false;

  long t1 = millis();
  long delta = t1-t0;
  if (delta>10) {
    Serial.print("delta=");
    Serial.println(delta);
  }
  
} // loop


#ifdef COMMON_ANODE

void singleRed() {
  digitalWrite(LED_R, LOW);
  digitalWrite(LED_G, HIGH);
  digitalWrite(LED_B, HIGH);
}

void singleGreen() {
  digitalWrite(LED_R, HIGH);
  digitalWrite(LED_G, LOW);
  digitalWrite(LED_B, HIGH);
}

void singleBlue() {
  digitalWrite(LED_R, HIGH);
  digitalWrite(LED_G, HIGH);
  digitalWrite(LED_B, LOW);
}

void singleYellow() {
  digitalWrite(LED_R, LOW);
  digitalWrite(LED_G, LOW);
  digitalWrite(LED_B, HIGH);
}

void allBlack() {
  digitalWrite(LED_R, HIGH);
  digitalWrite(LED_G, HIGH);
  digitalWrite(LED_B, HIGH);
}

#else

void singleWhite() {
  digitalWrite(LED_R, HIGH);
  digitalWrite(LED_G, HIGH);
  digitalWrite(LED_B, HIGH);
}

void singleRed() {
  digitalWrite(LED_R, HIGH);
  digitalWrite(LED_G, LOW);
  digitalWrite(LED_B, LOW);
}

void redOn() {
  digitalWrite(LED_R, HIGH);
}

void redOff() {
  digitalWrite(LED_R, HIGH);
}

void greenOn() {
  digitalWrite(LED_G, HIGH);
}

void greenOff() {
  digitalWrite(LED_G, HIGH);
}

void singleGreen() {
  digitalWrite(LED_R, LOW);
  digitalWrite(LED_G, HIGH);
  digitalWrite(LED_B, LOW);
}

void singleBlue() {
  digitalWrite(LED_R, LOW);
  digitalWrite(LED_G, LOW);
  digitalWrite(LED_B, HIGH);
}

void singleYellow() {
  digitalWrite(LED_R, HIGH);
  digitalWrite(LED_G, HIGH);
  digitalWrite(LED_B, LOW);
}

void allBlack() {
  digitalWrite(LED_R, LOW);
  digitalWrite(LED_G, LOW);
  digitalWrite(LED_B, LOW);
}

#endif
