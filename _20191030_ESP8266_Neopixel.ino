/*
  ####
  ## ESP8266 Neopixel Controller
  ####
  I have designed it for ESP-01S module, but you can use any ESP8266 board.
  ESP-01S Arduino IDE setting:
    Board: Generic ESP8266 Module
    Flash size: 1M (128k SPIFFS)
    Flash Mode: QIO
    Reset method: no dtr, no_sync
  When you want to erase EEPROM: Erase Flash: All Flash Contents

  upload code and then upload files

  use ESP8266 Sketch Data Upload to upload files to ESP8266 SPIFFS: // https://github.com/esp8266/arduino-esp8266fs-plugin
    data\bootstrap.min.css.gz
    data\bootstrap.min.js.gz
    data\favicon.ico
    data\index.html.gz
    data\iro.min.js.gz
    data\jquery-3.4.1.min.js.gz

  Tested with versions:
    Arduino IDE 1.8.10
    ESP8266 core 2.6.0

  Libraries:
    ArduinoJSON 6.12.0
    ESP_EEPROM 2.0.0
    FastLed 3.3
    WifiManages 0.15.0-beta
    PubSubClient 2.7
    you should increase MQTT_MAX_PACKET_SIZE in PubSubClient.h for 10 colors is 512 sufficient
      #define MQTT_MAX_PACKET_SIZE 512

  REST API:
   GET:
    /api?color=RRGGBB&brightness=xx   - set color of the Neopixels, returns JSON with information about success or error
    /api?all=1                        - returns JSON with saved colors
    /api?off=1                        - turn Neopixels off, returns JSON with information about success or error
    /api?effect=effect_name           - display desired effect, returns JSON with information about success or error, effect names in variable char Effects[][10]
   POST:
    /api?save=1&color=RRGGBB&brightness=xx   - save new color to EEPROM, returns JSON with information about success or error
    /api?delete=1&color=RRGGBB&brightness=xx - delete color from EEPROM, returns JSON with information about success or error

  MQTT API:
   Commands:
    /cmd/color ; payload: RRGGBB/xx
    /cmd/all
    /cmd/off
    /cmd/effect ; payload: effect_name
   Returns JSON in:
    /rspn      ; payload: JSON

  BUTTON actions:
    click: next saved color
    double click: next effect
    long click: neopixels off
*/

#include <Arduino.h>
#include <FS.h>
#include <ESP8266WiFi.h>          //ESP8266 Core WiFi Library
#include <DNSServer.h>            //Local DNS Server used for redirecting all requests to the configuration portal - needed for WifiManager
#include <ESP8266WebServer.h>     //Local WebServer used to serve the configuration portal

#include <WiFiManager.h>          // https://github.com/tzapu/WiFiManager WiFi Configuration Magic
#include <ESP_EEPROM.h>           // https://github.com/jwrw/ESP_EEPROM
#include <PinButton.h>            // https://github.com/poelstra/arduino-multi-button
#include <ArduinoJson.h>          // https://github.com/bblanchon/ArduinoJson

#include <FastLED.h>              // https://github.com/FastLED/FastLED

/*
  ## WIFI Manager function
  When your ESP starts up, it sets it up in Station mode and tries to connect to a previously saved Access Point
  if this is unsuccessful (or no previous network saved) it moves the ESP into Access Point mode and spins up a DNS and WebServer (ip 192.168.0.1)
  using any wifi enabled device with a browser (computer, phone, tablet) connect to the newly created Access Point
  because of the Captive Portal and the DNS server you will either get a 'Join to network' type of popup or get any domain you try to access redirected to the configuration portal
  choose one of the access points scanned, enter password, click save
  ESP will try to connect. If successful, it relinquishes control back to your app. If not, reconnect to AP and reconfigure.
*/


// ==================== Settings ====================
#define ESP_HOSTNAME    "NPCTRL001"
#define KBAUDRATE       115200         // The Serial connection baud rate.
#define COLOR_COUNT     10             // maximum number of saved colors

#define NUM_LEDS                   16
//#define LED_DATA_PIN               D2  // for Wemos D1 Mini
#define LED_DATA_PIN               2  // for ESP-01S with WS2812B adapter
#define LED_TYPE                   WS2812B
#define COLOR_ORDER                GRB
#define EFFECT_BRIGHTNESS          96
#define EFFECT_FRAMES_PER_SECOND   30

//#define BUTTON_PIN                 D3  // Pin where the button is connected on Wemos D1 Mini
#define BUTTON_PIN                 3  // for ESP-01S - RX pin on ESP-01S - Pin where the button is connected

// MQTT API - comment out, if you do not need it
#define USE_MQTT

#ifdef USE_MQTT
  #include <PubSubClient.h>                     // https://github.com/knolleary/pubsubclient
  #define MQTT_SERVER             "192.168.1.100"
  #define MQTT_USERNAME           "username"
  #define MQTT_PASSWORD           "password"
  #define MQTT_TOPIC              ESP_HOSTNAME
  #define MQTT_CMD_TOPIC          MQTT_TOPIC "/cmd"
  #define MQTT_RSPN_TOPIC         MQTT_TOPIC "/rspn"
  #define MQTT_LAST_WILL_TOPIC    MQTT_TOPIC "/live"
  #define MQTT_LAST_WILL          "OFF"
  #define MQTT_MAX_PAYLOAD_LENGTH 15
#endif


// ==================== Constants and helper macros ====================
#define ARRAY_SIZE(A)             (sizeof(A) / sizeof((A)[0]))

#define BUTTON_NO_CLICK 0
#define BUTTON_SINGLE_CLICK 1
#define BUTTON_DOUBLE_CLICK 2
#define BUTTON_LONG_CLICK   3


// ==================== Types definition ====================
enum TProgram_States {NEOPIXEL_OFF, NEOPIXEL_COLOR, NEOPIXEL_EFFECT};

// Effects - each is defined as a separate function below.
typedef void (*effectFunctionType)();

typedef struct {
  char effectName[15];
  effectFunctionType effectFunction;
  int16_t effectFrames;
} effectType;

// structure to hold information about a color and brightness of Neopixels
typedef struct {
  uint8_t rgb[3];                           // RGB color: rbg[0] - R, rbg[1] - G, rbg[2] - B
  uint8_t brtns;                            // brightness of the color
} colorType;

// Settings
typedef struct {
  uint8_t colorCount;                  // 1 byte, Count of saved colors
  TProgram_States programState;        // Actual state of running program
  uint8_t activeColor;                 // index of saved color that is displayed
  uint8_t activeEffect;                // index of effect that is displayed
} npControllerSettingType;

typedef struct {                        // structure, that is written into the EEPROM memory
  uint32_t                crc32;        // 4 bytes
  npControllerSettingType npControllerSetting;
  colorType               colors[COLOR_COUNT+1]; // index COLOR_COUNT is for new color, not yet saved, but requested
} npControllerEEPROMType;


// ==================== Global variables ====================
void rainbow();
void rainbowWithGlitter();
void confetti();
void sinelon();
void bpm();
void juggle();
void fire2012();
void cylon();
void sparkle();
void snowsparkle();
void meteor();
void theatre();

effectType effects[] = {
  {"rainbow",   rainbow,            EFFECT_FRAMES_PER_SECOND * 5},
  {"rainbowGl", rainbowWithGlitter, EFFECT_FRAMES_PER_SECOND * 5},
  {"confetti",  confetti,           EFFECT_FRAMES_PER_SECOND * 5},
  {"sinelon",   sinelon,            EFFECT_FRAMES_PER_SECOND * 5},
  {"bpm",       bpm,                EFFECT_FRAMES_PER_SECOND * 5},
  {"juggle",    juggle,             EFFECT_FRAMES_PER_SECOND},
  {"fire2012",  fire2012,           EFFECT_FRAMES_PER_SECOND},
  {"cylon",     cylon,              EFFECT_FRAMES_PER_SECOND * 4},
  {"sparkle",   sparkle,            EFFECT_FRAMES_PER_SECOND},
  {"snowspark", snowsparkle,        EFFECT_FRAMES_PER_SECOND},
  {"meteor",    meteor,             EFFECT_FRAMES_PER_SECOND * 2},
  {"theatre",   theatre,            EFFECT_FRAMES_PER_SECOND/2}};

int16_t actualFrame;                              // contains the number of actual Frame for chosen effect
npControllerEEPROMType    npControllerEEPROM;     // contains all saved colors, CRC32 and other informations
colorType                 *colors;                // saved colors the application operates with
npControllerSettingType   *npControllerSetting;   // Will containt info about active color/effect, number of saved colors...
uint32_t lastNeopixelRefresh;                     // actual time in program in ms
uint8_t  changeNeopixel;                          // 1 if effect or color should be changed

// memory structure to store the state of individual LEDs
CRGB leds[NUM_LEDS];

// Strings for storing JSON
String strOutput;

// Web serever will run on port 80
ESP8266WebServer server(80);

// Create a new button object, listening on pin BUTTON_PIN
PinButton myButton(BUTTON_PIN);

#ifdef USE_MQTT
  WiFiClient espWifiClient;
  PubSubClient mqtt_client(espWifiClient);
#endif


// ==================== JSON responses ====================
void allResponse () {
  // JSON:{"rsT":"all", "rsE":"", "rsV":[{"rgb":"aabbcc","brtns":50},{"rgb":"994422","brtns":100}]}
  const uint16_t capacityResponseJSON = JSON_ARRAY_SIZE(COLOR_COUNT) + COLOR_COUNT*JSON_OBJECT_SIZE(2) + JSON_OBJECT_SIZE(3) + COLOR_COUNT * 30;
  StaticJsonDocument<capacityResponseJSON> responseJSON;
  responseJSON["rsT"] = "all";
  responseJSON["rsE"] = "";
  strOutput = "";
  JsonArray rsV = responseJSON.createNestedArray("rsV");
  for (uint8_t i = 0; i < npControllerSetting->colorCount; i++) {
    JsonObject colorElem = rsV.createNestedObject();
    char rgbStr[7];
    sprintf(rgbStr, "%02X%02X%02X", colors[i].rgb[0], colors[i].rgb[1], colors[i].rgb[2]);
    colorElem["rgb"] = rgbStr;
    colorElem["brtns"] = colors[i].brtns;
  }
  serializeJson(responseJSON, strOutput);
}

void colorResponse (uint8_t colorResult) {
  // JSON: {"rsT":"color", "rsE":"", "rsV":{"rgb":"aabbcc","brtns":50}}
  const uint16_t capacityResponseJSON = JSON_OBJECT_SIZE(2) + JSON_OBJECT_SIZE(3) + 40;
  StaticJsonDocument<capacityResponseJSON> responseJSON;
  responseJSON["rsT"] = "color";
  responseJSON["rsE"] = "";
  strOutput = "";
  if (colorResult != 1) {
    responseJSON["rsE"] = "Wrong format of color/brightness.";
  } else {
    JsonObject rsV = responseJSON.createNestedObject("rsV");
    char rgbStr[7];
    uint8_t colorIndex = npControllerSetting->activeColor;
    sprintf(rgbStr, "%02X%02X%02X", colors[colorIndex].rgb[0], colors[colorIndex].rgb[1], colors[colorIndex].rgb[2]);
    rsV["rgb"] = rgbStr;
    rsV["brtns"] = colors[colorIndex].brtns;
  }
  serializeJson(responseJSON, strOutput);
}

void offResponse () {
  // JSON: {"rsT":"off", "rsE":"", "rsV":1}
  const int capacityResponseJSON = JSON_OBJECT_SIZE(3) + 20;
  StaticJsonDocument<capacityResponseJSON> responseJSON;
  responseJSON["rsT"] = "off";
  responseJSON["rsE"] = "";
  strOutput = "";
  responseJSON["rsV"] = 1;
  serializeJson(responseJSON, strOutput);
}

void effectResponse (uint8_t effectResult) {
  // JSON: {"rsT":"effect", "rsE":"", "rsV":"EFFECT_NAME"}
  const int capacityResponseJSON = JSON_OBJECT_SIZE(3) + 40;
  StaticJsonDocument<capacityResponseJSON> responseJSON;
  responseJSON["rsT"] = "effect";
  responseJSON["rsE"] = "";
  strOutput = "";
  if (effectResult != 1) {
    responseJSON["rsE"] = "Wrong name of effect.";
  } else {
    responseJSON["rsV"] = effects[npControllerSetting->activeEffect].effectName;
  }
  serializeJson(responseJSON, strOutput);
}

void saveResponse (uint8_t saveResult) {
  // JSON: {"rsT":"save", "rsE":"", "rsV":{"rgb":"aabbcc","brtns":50}}
  const int capacityResponseJSON = JSON_OBJECT_SIZE(2) + JSON_OBJECT_SIZE(3) + 40;
  StaticJsonDocument<capacityResponseJSON> responseJSON;
  responseJSON["rsT"] = "save";
  responseJSON["rsE"] = "";
  strOutput = "";
  if (saveResult != 1) {
    if (saveResult == 0) responseJSON["rsE"] = "Could not save the new color to EEPROM.";
    else if (saveResult == 2) responseJSON["rsE"] = "Wrong format for color/brightness.";
    else if (saveResult == 3) responseJSON["rsE"] = "Color is already saved.";
    else responseJSON["rsE"] = "Maximum number of colors saved.";
  } else {
    JsonObject rsV = responseJSON.createNestedObject("rsV");
    char rgbStr[7];
    uint8_t colorIndex = npControllerSetting->colorCount - 1;
    sprintf(rgbStr, "%02X%02X%02X", colors[colorIndex].rgb[0], colors[colorIndex].rgb[1], colors[colorIndex].rgb[2]);
    rsV["rgb"] = rgbStr;
    rsV["brtns"] = colors[colorIndex].brtns;
  }
  serializeJson(responseJSON, strOutput);
}

void deleteResponse (uint8_t deleteResult, char rgbHex[], uint8_t brightness) {
  // JSON: {"rsT":"delete", "rsE":"","rsV":{"rgb":"aabbcc","brtns":50}}
  const int capacityResponseJSON = JSON_OBJECT_SIZE(2) + JSON_OBJECT_SIZE(3) + 50;
  StaticJsonDocument<capacityResponseJSON> responseJSON;
  responseJSON["rsT"] = "delete";
  responseJSON["rsE"] = "";
  strOutput = "";
  if (deleteResult != 1) {
    if (deleteResult == 0) responseJSON["rsE"] = "Error during saving to EEPROM.";
    else if (deleteResult == 2) responseJSON["rsE"] = "Wrong format for color/brightness.";
    else responseJSON["rsE"] = "Color not found.";
  } else {
    JsonObject rsV = responseJSON.createNestedObject("rsV");
    rsV["rgb"] = rgbHex;
    rsV["brtns"] = brightness;
  }
  serializeJson(responseJSON, strOutput);
}


// ==================== Requests processing ====================
// set color of the Neopixels
uint8_t colorRequest(char *rgbHex, uint8_t brightness) {
  uint8_t result = checkValidColor(rgbHex, brightness);

  if (result) {
    npControllerSetting->programState = NEOPIXEL_COLOR;
    changeNeopixel = 1;

    // check if the color is stored or it is a new one
    uint8_t rgb[3];
    parseHexColor(rgbHex, rgb);
    uint8_t colorIndex = findColor(rgb, brightness);
    colors[colorIndex].brtns = brightness;
    colors[colorIndex].rgb[0] = rgb[0];
    colors[colorIndex].rgb[1] = rgb[1];
    colors[colorIndex].rgb[2] = rgb[2];

    npControllerSetting->activeColor = colorIndex;
  }
  return result;
}

uint8_t offRequest() {
  Serial.println(F("Set Neopixels off."));

  npControllerSetting->programState = NEOPIXEL_OFF;
  changeNeopixel = 1;

  return 1;
}

uint8_t effectRequest(char *effectName) {
  uint8_t effectIndex = checkValidEffect(effectName);

  if (effectIndex < ARRAY_SIZE(effects)) {
    npControllerSetting->programState = NEOPIXEL_EFFECT;
    changeNeopixel = 1;
    npControllerSetting->activeEffect = effectIndex;
    return 1;
  }
  return 0;
}

// Learn (save) new color
uint8_t saveRequest(char *rgbHex, uint8_t brightness) {
  if (npControllerSetting->colorCount == COLOR_COUNT) return 4;  // no more space for saving

  if (checkValidColor(rgbHex, brightness)) {
    // check if the color is stored
    uint8_t rgb[3];
    parseHexColor(rgbHex, rgb);
    uint8_t colorIndex = findColor(rgb, brightness);

    if (colorIndex < COLOR_COUNT) return 3;  // already saved color

    colors[npControllerSetting->colorCount].brtns = brightness;
    colors[npControllerSetting->colorCount].rgb[0] = rgb[0];
    colors[npControllerSetting->colorCount].rgb[1] = rgb[1];
    colors[npControllerSetting->colorCount].rgb[2] = rgb[2];
    npControllerSetting->colorCount++;

    return saveEEPROM();   // 0 - error during saving, 1 - OK
  }
  else {
    return 2;   // invalid input color
  }
}

// Delete saved color
uint8_t deleteRequest(char *rgbHex, uint8_t brightness) {
  if (checkValidColor(rgbHex, brightness)) {
    // check if the color is stored
    uint8_t rgb[3];
    parseHexColor(rgbHex, rgb);
    uint8_t colorIndex = findColor(rgb, brightness);

    if (colorIndex == COLOR_COUNT) return 3;   // color is not saved

    resetColor(&colors[colorIndex]);
    for (uint8_t i = colorIndex+1; i < npControllerSetting->colorCount; i++) {
      uint8_t prevIndex = i-1;
      colors[prevIndex].brtns = colors[i].brtns;
      colors[prevIndex].rgb[0] = colors[i].rgb[0];
      colors[prevIndex].rgb[1] = colors[i].rgb[1];
      colors[prevIndex].rgb[2] = colors[i].rgb[2];
    }

    if (npControllerSetting->activeColor == colorIndex) npControllerSetting->activeColor = 0;
    else if (npControllerSetting->activeColor > colorIndex) npControllerSetting->activeColor--;

    npControllerSetting->colorCount--;

    return saveEEPROM();   // 0 - error during saving, 1 - OK
  }
  else {
    return 2;   // invalid input color
  }
}

// ==================== Color print and help functions ====================
void resetColor(colorType * color) {
  for (uint8_t i = 0; i < 3; i++) color->rgb[i] = 0;
  color->brtns = 0;
}

void parseHexColor(const char* rgbHex, uint8_t rgb[]) {
  uint32_t rgb32;
  sscanf(rgbHex, "%x", &rgb32);
  rgb[0] = (rgb32 & 0x00ff0000)>>16;
  rgb[1] = (rgb32 & 0x0000ff00)>>8;
  rgb[2] = rgb32 & 0x000000ff;
}

// Return true/false if color is valid
uint8_t checkValidColor(char* rgbHex, uint8_t brightness) {
  if (strlen(rgbHex) != 6) return 0;
  for (uint8_t i = 0; i < 6; i++) if (!isxdigit(rgbHex[i])) return 0;
  uint8_t rgb[3];
  parseHexColor(rgbHex, rgb);
  if ((rgb[0]==0)&&(rgb[1]==0)&&(rgb[2]==0)) return 0;
  if (brightness > 100) return 0;

  return 1;
}

// Return true/false if effect is valid
uint8_t checkValidEffect(char* effectName) {
  for (uint8_t i = 0; i < ARRAY_SIZE(effects); i++) {
    if (strcmp(effects[i].effectName, effectName) == 0) return i;
  }
  return ARRAY_SIZE(effects);
}

// find, if color is stored in EEPROM
uint8_t findColor(uint8_t rgb[], uint8_t brightness) {
  uint8_t colorIndex = COLOR_COUNT;
  if (npControllerSetting->colorCount > 0) {
    for (uint8_t i = 0; i < npControllerSetting->colorCount; i++) {
      if ( (colors[i].brtns == brightness) &&
           (colors[i].rgb[0] == rgb[0]) &&
           (colors[i].rgb[1] == rgb[1]) &&
           (colors[i].rgb[2] == rgb[2])) return i;
    }
  }
  return colorIndex;
}

// Write information about a color to serial
void dumpColor(const colorType * color, bool fullColor) {
  Serial.printf("Color : #%02X%02X%02X\n", color->rgb[0], color->rgb[1], color->rgb[2]);
  if (fullColor) {
    Serial.printf("RGB : %i, %i, %i\n", color->rgb[0], color->rgb[1], color->rgb[2]);
    CRGB rgb = CRGB(color->rgb[0], color->rgb[1], color->rgb[2]);
    CHSV hsv = rgb2hsv_approximate(rgb);
    Serial.printf("HSV : %i, %i, %i\n", hsv.h, hsv.s, hsv.v);
  }
  Serial.printf("Brightness : %i%%\n\n", color->brtns);
}

// Write information about all saved colors to serial
void dumpStoredColors (bool fullColor) {
  Serial.println(F("Dump of all stored colors"));
  uint8_t i = 0;
  for (uint8_t i = 0; i < npControllerSetting->colorCount; i++) {
    Serial.print(F("Color id: "));
    Serial.println(i);
    dumpColor(&colors[i], fullColor);
  }
}


// ==================== Button click ====================
void processClick(uint8_t clickType) {
  changeNeopixel = 1;
  TProgram_States oldState = npControllerSetting->programState;

  if (clickType == BUTTON_LONG_CLICK) {
    Serial.println(F("Recevice *long* CLICK request."));
    npControllerSetting->programState = NEOPIXEL_OFF;

  } else if (clickType == BUTTON_SINGLE_CLICK) {
    Serial.println(F("Recevice *single* CLICK request."));
    if (npControllerSetting->colorCount == 0) return;

    npControllerSetting->programState = NEOPIXEL_COLOR;

    if (npControllerSetting->activeColor == COLOR_COUNT) {   // if the last shown color was not saved in EEPROM, we start from the beginning
      npControllerSetting->activeColor = 0;
      return;
    }

    if ((oldState == NEOPIXEL_OFF) || (oldState == NEOPIXEL_EFFECT)) return; // we use last chosen color

    if (npControllerSetting->activeColor == (npControllerSetting->colorCount - 1)) {
      npControllerSetting->programState = NEOPIXEL_OFF;
      npControllerSetting->activeColor = 0;
    }
    else {
      npControllerSetting->activeColor++;
    }

  } else if (clickType == BUTTON_DOUBLE_CLICK) {
    Serial.println(F("Recevice *double* CLICK request."));
    npControllerSetting->programState = NEOPIXEL_EFFECT;

    if ((oldState == NEOPIXEL_OFF) || (oldState == NEOPIXEL_COLOR)) return; // we use last chosen effect
    if (npControllerSetting->activeEffect == (ARRAY_SIZE(effects) - 1)) {
      npControllerSetting->programState = NEOPIXEL_OFF;
      npControllerSetting->activeEffect = 0;
    } else {
      npControllerSetting->activeEffect++;
    }
  }
}

// ==================== Web server help functions ====================
// Return content type of a file
String getContentType(String filename) { // convert the file extension to the MIME type
  if (filename.endsWith(".html")) return "text/html";
  else if (filename.endsWith(".css")) return "text/css";
  else if (filename.endsWith(".js")) return "application/javascript";
  else if (filename.endsWith(".ico")) return "image/x-icon";
  else if (filename.endsWith(".gz")) return "application/x-gzip";
  return "text/plain";
}

// Load file from SPIFFS and return it through the web server
bool handleFileRead(String path) { // send the right file to the client (if it exists)
  Serial.println("handleFileRead: " + path);
  if (path.endsWith("/")) path += "index.html";          // If a folder is requested, send the index file
  String contentType = getContentType(path);             // Get the MIME type
  String pathWithGz = path + ".gz";
  if (SPIFFS.exists(pathWithGz) || SPIFFS.exists(path)) { // If the file exists, either as a compressed archive, or normal
    if (SPIFFS.exists(pathWithGz))                         // If there's a compressed version available
      path += ".gz";                                         // Use the compressed version
    //yield();
    File file = SPIFFS.open(path, "r");                    // Open the file
    size_t sent = server.streamFile(file, contentType);    // Send it to the client
    file.close();                                          // Close the file again
    Serial.println(String("\tSent file: ") + path);
    return true;
  }
  Serial.println(String("\tFile Not Found: ") + path);
  return false;                                          // If the file doesn't exist, return false
}


// ==================== MQTT API ====================
#ifdef USE_MQTT

// Returns last index of x if it is present.
// Else returns -1.
uint8_t findLastIndex(char *str, char x) {
  uint8_t len = strlen(str);
  // Traverse from right
  for (uint8_t i = len - 1; i >= 0; i--)
    if (str[i] == x) return i;
  return -1;
}

void mqttCallback(char* topic, byte* payload, unsigned int length) {
  Serial.printf("MQTT message arrived, topic [%s], payload [", topic);
  // payload is without ending \0
  char payloadMessage[MQTT_MAX_PAYLOAD_LENGTH];
  uint8_t payloadLength = (length > (MQTT_MAX_PAYLOAD_LENGTH-1)) ? (MQTT_MAX_PAYLOAD_LENGTH-1) : length;
  memcpy(payloadMessage, payload, payloadLength);
  payloadMessage[payloadLength] = '\0';
  Serial.printf("%s]\n", payloadMessage);
  //yield();

  uint8_t lastToken = findLastIndex(topic, '/');
  if ((lastToken == -1) || ((strlen(topic) - (lastToken+1)) > 6)) {
    Serial.println(F("Wrong parameters for topics."));
    return;
  }
  char cmdType[7];
  strcpy(cmdType, &topic[lastToken + 1]);

  if (strcmp(cmdType, "all") == 0) {
    // ###########################
    // ## all
    // ###########################
    Serial.println(F("Received *all* MQTT request"));
    //{"rsT":"all", "rsE":"", "rsV":[{"rgb":"aabbcc","brtns":50},{"rgb":"994422","brtns":100}]}
    allResponse();
    Serial.printf("*all* MQTT topic [%s] response: %s\n", MQTT_RSPN_TOPIC, strOutput.c_str());
    mqtt_client.publish(MQTT_RSPN_TOPIC, strOutput.c_str(), false);


  } else if (strcmp(cmdType, "color") == 0) {
    // ###########################
    // ## color
    // ###########################
    Serial.println(F("Received *color* MQTT request"));
    char rgbHex[7];
    uint8_t brightness;

    strncpy(rgbHex, payloadMessage, 6);
    rgbHex[6] = '\0';
    brightness = atoi(&payloadMessage[7]);
    //{"rsT":"color", "rsE":"", "rsV":{"rgb":"aabbcc","brtns":50}}

    uint8_t colorResult = colorRequest(rgbHex, brightness);
    colorResponse(colorResult);

    Serial.printf("*color* MQTT topic [%s] response: %s\n", MQTT_RSPN_TOPIC, strOutput.c_str());
    mqtt_client.publish(MQTT_RSPN_TOPIC, strOutput.c_str(), false);

  } else if (strcmp(cmdType, "off") == 0) {
    // ###########################
    // ## off
    // ###########################
    Serial.println(F("Received *off* MQTT request"));
    //{"rsT":"off", "rsE":"", "rsV":1}

    offRequest();
    offResponse();

    Serial.printf("*off* MQTT topic [%s] response: %s\n", MQTT_RSPN_TOPIC, strOutput.c_str());
    mqtt_client.publish(MQTT_RSPN_TOPIC, strOutput.c_str(), false);

  } else if (strcmp(cmdType, "effect") == 0) {
    // ###########################
    // ## effect
    // ###########################
    Serial.printf("Received *effect* MQTT request, payload-%s\n", payloadMessage);
    //{"rsT":"effect", "rsE":"", "rsV":"EFFECT_NAME"}

    uint8_t effectResult = effectRequest(payloadMessage);
    effectResponse(effectResult);

    Serial.printf("*effect* MQTT topic [%s] response: %s\n", MQTT_RSPN_TOPIC, strOutput.c_str());
    mqtt_client.publish(MQTT_RSPN_TOPIC, strOutput.c_str(), false);

  } else {
    Serial.println(F("Received *UNKNOWN* MQTT request"));
    mqtt_client.publish(MQTT_LAST_WILL_TOPIC, "ERROR - Unknown MQTT command, supported: all; color; off; effect", false);
  }
}

void mqttConnect() {
  mqtt_client.setServer(MQTT_SERVER, 1883);
  mqtt_client.setCallback(mqttCallback);

  // Loop until we're reconnected
  uint8_t i = 0;
  while (!mqtt_client.connected()) {
    i++;
    // Attempt to connect
    //boolean connect(const char* id, const char* user, const char* pass, const char* willTopic, uint8_t willQos, boolean willRetain, const char* willMessage);
    if (!mqtt_client.connect(ESP_HOSTNAME, MQTT_USERNAME, MQTT_PASSWORD, MQTT_LAST_WILL_TOPIC, 0, 1, MQTT_LAST_WILL)) {
      Serial.printf("failed, rc=%s\n", mqtt_client.state());
      if (i <= 10) {
        Serial.println(F(" try again in 500ms"));
        delay(500); // Wait 500ms before retrying
      } else {
        ESP.reset();
      }
    }
  }

  mqtt_client.publish(MQTT_LAST_WILL_TOPIC, "ON", false);
  mqtt_client.subscribe(MQTT_CMD_TOPIC "/#");
}
#endif


// ==================== EEPROM helpers ====================
uint8_t saveEEPROM() {
  Serial.println(F("EEPROM saving"));
  npControllerEEPROM.crc32 =  calculateCRC32( ((uint8_t*)&npControllerEEPROM) + 4, sizeof( npControllerEEPROM ) - 4 );

  EEPROM.put(0, npControllerEEPROM);
  uint8_t result = EEPROM.commit();
  //yield();
  Serial.println((result) ? F("Commit OK") : F("Commit failed"));
  return result;
}

void resetEEPROM() {
  for (uint8_t i = 0; i < COLOR_COUNT; i++) {
    resetColor(&colors[i]);
  }
  npControllerSetting->colorCount = 0;
  npControllerSetting->programState = NEOPIXEL_OFF;
  npControllerSetting->activeColor = 0;
  npControllerSetting->activeEffect = 0;

  saveEEPROM();
}

uint32_t calculateCRC32( const uint8_t *data, size_t length ) {
  uint32_t crc = 0xffffffff;
  while ( length-- ) {
    uint8_t c = *data++;
    for ( uint32_t i = 0x80; i > 0; i >>= 1 ) {
      bool bit = crc & 0x80000000;
      if ( c & i ) {
        bit = !bit;
      }

      crc <<= 1;
      if ( bit ) {
        crc ^= 0x04c11db7;
      }
    }
  }
  return crc;
}


// ==================== SETUP ====================
void setup() {
  Serial.begin(KBAUDRATE, SERIAL_8N1, SERIAL_TX_ONLY);                             // RX pin is used for button on ESP01
  while (!Serial) delay(50); // Wait for the serial connection to be establised.

  Serial.printf("\n\nESP8266 Neopixel Controller - %s\n\n", ESP_HOSTNAME);

  // get possible reset reason
  Serial.printf("Start: %s\n\n", ESP.getResetReason().c_str());

  // Name and MAC address
  Serial.printf("Hostname: %s\n", ESP_HOSTNAME);
  Serial.printf("MAC address: %s\n\n", WiFi.macAddress().c_str());

  // reserve memory for long strings
  strOutput.reserve(50 + 30*COLOR_COUNT);

  Serial.print(F("Loading from EEPROM... "));
  npControllerSetting = &npControllerEEPROM.npControllerSetting;   // shotcuts to global settings
  colors = &npControllerEEPROM.colors[0];

  EEPROM.begin(sizeof(npControllerEEPROMType));
  if (EEPROM.percentUsed() >= 0) {
    Serial.printf("EEPROM has data from a previous run.%i%% of ESP flash space currently used\n", EEPROM.percentUsed());

    EEPROM.get(0, npControllerEEPROM);
    uint32_t crc = calculateCRC32( ((uint8_t*) &npControllerEEPROM) + 4, sizeof( npControllerEEPROM ) - 4 );
    if ( crc == npControllerEEPROM.crc32 ) {
      Serial.printf("loaded\n");

      Serial.printf("Count of saved colors: %i\n", npControllerSetting->colorCount);

      dumpStoredColors (true);
    } else {
      Serial.println(F("CRC32 error, resetting EEPROM"));
      resetEEPROM();
    }
  } else {
    Serial.println(F("EEPROM size changed - EEPROM data zeroed - commit() to make permanent"));
    resetEEPROM();
  }

  // Start WiFiManager, that handles connection to the WIFI network
  Serial.println(F("Connecting to WIFI"));
  WiFi.hostname(ESP_HOSTNAME);
  WiFiManager wm;
  // IP parameters of WIFI network when WIfi Manager starts in AP mode - custom ip /gateway /subnet configuration
  wm.setAPStaticIPConfig(IPAddress(192, 168, 0, 1), IPAddress(192, 168, 0, 1), IPAddress(255, 255, 255, 0)); // This will set your captive portal to a specific IP should you need/want such a feature. Add the following snippet before autoConnect()
  // set IP after logon to wifi (instead of DHCP) - custom ip /gateway /subnet configuration
  //wifiManager.setSTAStaticIPConfig(IPAddress(192,168,0,99), IPAddress(192,168,0,1), IPAddress(255,255,255,0));

  // Connect to WIFI
  // If there are no WIFI access information in the memory or the board is out of reach of saved WIFI
  // start own AP and the user must connect to this AP and sets up new WIFI connection
  if (!wm.autoConnect(ESP_HOSTNAME)) { // name of the AP WIFI, wifiManager.autoConnect("AutoConnectAP", "password")
    Serial.print(F("Could not connect to WIFI"));
    ESP.reset();                       // If something fails, restart ESP8266
    delay(1000);
  }
  Serial.print(F("Connected as: "));
  Serial.print(WiFi.localIP());
  Serial.printf("\n\n");

  // Initialize SPIFFS
  Serial.print(F("Mounting SPIFFS"));
  if (!SPIFFS.begin()) {
    Serial.println("An Error has occurred while mounting SPIFFS");
    ESP.reset();                       // If something fails, restart ESP8266
    delay(1000);
  }
  Serial.printf("... mounted\n\n");

  // Start web server that will send the client documents from SPIFFS
  Serial.print(F("Starting webserver"));

  // ==================== REST API ====================
  server.on("/api", HTTP_GET, []() {
    if (server.args() > 0) {
      int16_t response_code = 200;
      // 200 OK
      // 204 No Content
      // 400 Bad Request

      if (server.hasArg("all") && (server.arg("all").toInt() == 1)) {
        // ###########################
        // ## all
        // ###########################
        Serial.println(F("Received *all* GET request"));
        //{"rsT":"all", "rsE":"", "rsV":[{"rgb":"aabbcc","brtns":50},{"rgb":"994422","brtns":100}]}
        allResponse();
        Serial.printf("*all* GET response: %s\n", strOutput.c_str());
        server.send(response_code, "application/json", strOutput);

      } else if (server.hasArg("color") && (server.arg("color") != NULL) && server.hasArg("brightness") && (server.arg("brightness") != NULL)) {
        // ###########################
        // ## color
        // ###########################
        Serial.printf("Received *color* GET request, color %s, brightness %s\n", server.arg("color").c_str(), server.arg("brightness").c_str());
        char rgbHex[7];
        strncpy(rgbHex, server.arg("color").c_str(), 7);                 // contains ending 0
        rgbHex[6] = '\0';                                                // just to be sure, if in argument is something different than hex code
        uint8_t brightness = server.arg("brightness").toInt();
        //{"rsT":"color", "rsE":"", "rsV":{"rgb":"aabbcc","brtns":50}}

        uint8_t colorResult = colorRequest(rgbHex, brightness);
        colorResponse(colorResult);

        Serial.printf("*color* GET response: %s\n", strOutput.c_str());
        server.send(response_code, "application/json", strOutput);

      } else if (server.hasArg("off") && (server.arg("off").toInt() == 1)) {
        // ###########################
        // ## off
        // ###########################
        Serial.println(F("Received *off* GET request"));
        //{"rsT":"off", "rsE":"", "rsV":1}

        offRequest();
        offResponse();

        Serial.printf("*off* GET response: %s\n", strOutput.c_str());
        server.send(response_code, "application/json", strOutput);

      } else if (server.hasArg("effect") && (server.arg("effect") != NULL)) {
        // ###########################
        // ## effect
        // ###########################
        Serial.printf("Received *effect* GET request, effect name %s.\n", server.arg("effect").c_str());
        char effectName[10];
        strncpy(effectName, server.arg("effect").c_str(), 10);
        effectName[9] = '\0';
        //{"rsT":"effect", "rsE":"", "rsV":"EFFECT_NAME"}

        uint8_t effectResult = effectRequest(effectName);
        effectResponse(effectResult);

        Serial.printf("*effect* GET response: %s\n", strOutput.c_str());
        server.send(response_code, "application/json", strOutput);

      } else {
        Serial.println(F("Received *UNKNOWN* GET request"));
        response_code = 400;
        server.send(response_code, "text/html", "ERROR - Unknown API command"); // otherwise, respond with a 404 (Not Found) error
      }
    }
  });

  server.on("/api", HTTP_POST, []() {
    if (server.headers() > 0) {
      int16_t response_code = 200;
      // 200 OK
      // 204 No Content
      // 400 Bad Request

      if (server.hasHeader("save") && (server.header("save").toInt() == 1) && server.hasHeader("color") && (server.header("color") != NULL) && server.hasHeader("brightness") && (server.header("brightness") != NULL)) {
       // ###########################
       // ## save
       // ###########################
       Serial.printf("Received *save* POST request, color %s, brightness %s\n", server.header("color").c_str(), server.header("brightness").c_str());
       char rgbHex[7];
       strncpy(rgbHex, server.header("color").c_str(), 7);
       rgbHex[6] = '\0';
       uint8_t brightness = server.header("brightness").toInt();
       //{"rsT":"save", "rsE":"", "rsV":{"rgb":"aabbcc","brtns":50}}

       uint8_t saveResult = saveRequest(rgbHex, brightness);
       saveResponse(saveResult);

       Serial.printf("*save* POST response: %s\n", strOutput.c_str());
       server.send(response_code, "application/json", strOutput);

      } else if (server.hasHeader("delete") && (server.header("delete").toInt() == 1) && server.hasHeader("color") && (server.header("color") != NULL) && server.hasHeader("brightness") && (server.header("brightness") != NULL)) {
       // ###########################
       // ## delete
       // ###########################
       Serial.printf("Received *delete* POST request, color %s, brightness %s\n", server.header("color").c_str(), server.header("brightness").c_str());
       char rgbHex[7];
       strncpy(rgbHex, server.header("color").c_str(), 7);
       rgbHex[6] = '\0';
       uint8_t brightness = server.header("brightness").toInt();
       //{"rsT":"delete", "rsE":"","rsV":{"rgb":"aabbcc","brtns":50}}

       uint8_t deleteResult = deleteRequest(rgbHex, brightness);
       deleteResponse(deleteResult, rgbHex, brightness);

       Serial.printf("*delete* POST response: %s\n", strOutput.c_str());
       server.send(response_code, "application/json", strOutput);

      } else {
       Serial.println(F("Received *UNKNOWN* GET request"));
       response_code = 400;
       server.send(response_code, "text/html", "ERROR - Unknown API command"); // otherwise, respond with a 404 (Not Found) error
      }
    }
  });

  server.onNotFound([]() {                              // If the client requests any URI
    if (!handleFileRead(server.uri()))                  // send it if it exists
      server.send(404, "text/plain", "404: Not Found"); // otherwise, respond with a 404 (Not Found) error
  });

  const char * headerkeys[] = {"save", "color", "brightness", "delete"};   // list of headers key, we want to collect for POST requests
  size_t headerkeyssize = sizeof(headerkeys) / sizeof(char*);
  //ask server to track these headers
  server.collectHeaders(headerkeys, headerkeyssize);

  server.begin();
  Serial.printf("... started\n\n");

#ifdef USE_MQTT
  Serial.print(F("MQTT starting"));
  mqttConnect();
  Serial.printf("... started\n\n");
#endif


  Serial.print(F("Set up Neopixel"));
  FastLED.addLeds<LED_TYPE,LED_DATA_PIN,COLOR_ORDER>(leds, NUM_LEDS).setCorrection(TypicalLEDStrip);
  FastLED.clear(true);
  Serial.printf(" ... done\n\n");
}

// ==================== LOOP ====================
void loop() {
  // Handle HTTP communication
  server.handleClient();

#ifdef USE_MQTT
  if (!mqtt_client.connected()) {
    mqttConnect();
  }
  mqtt_client.loop();
#endif

  // Read hardware pin, convert to click events
  myButton.update();
  if      (myButton.isSingleClick()) processClick(BUTTON_SINGLE_CLICK);
  else if (myButton.isDoubleClick()) processClick(BUTTON_DOUBLE_CLICK);
  else if (myButton.isLongClick())   processClick(BUTTON_LONG_CLICK);

  // Handle Neopixels
  effectType *activeEffect;
  switch (npControllerSetting->programState) {
    case NEOPIXEL_COLOR:
      if (changeNeopixel) {
        uint8_t activeColor = npControllerSetting->activeColor;
        Serial.printf("New color: #%02X%02X%02X/%i\n", colors[activeColor].rgb[0], colors[activeColor].rgb[1], colors[activeColor].rgb[2], colors[activeColor].brtns);
        CRGB color = CRGB( colors[activeColor].rgb[0], colors[activeColor].rgb[1], colors[activeColor].rgb[2]);
        uint8_t brightness = map(colors[activeColor].brtns, 0, 100, 0, 255);
        FastLED.showColor(color, brightness);
      }
      break;

    case NEOPIXEL_EFFECT:
      activeEffect = &effects[npControllerSetting->activeEffect];
      if (changeNeopixel) {
        Serial.printf("New effect: %s\n", activeEffect->effectName);
        FastLED.setBrightness(EFFECT_BRIGHTNESS);
        lastNeopixelRefresh = 0;
        actualFrame = 0;
      }

      if ((lastNeopixelRefresh + (1000/EFFECT_FRAMES_PER_SECOND)) <= millis()) {  // it is time for new frame
        // Call the current pattern function once, updating the 'leds' array

        (activeEffect->effectFunction)();

        actualFrame++;
        if (actualFrame >= activeEffect->effectFrames) {
          actualFrame = 0;
        }

        // send the 'leds' array out to the actual LED strip
        FastLED.show();

        lastNeopixelRefresh = millis();
      }
      break;

    case NEOPIXEL_OFF:
      if (changeNeopixel) {
        Serial.println(F("Neopixels off"));
        FastLED.clear(true);
      }
      break;
  }

  if (changeNeopixel) {
    changeNeopixel = 0;
  }
}



void rainbow() {
  uint8_t effectIndex = npControllerSetting->activeEffect;
  uint8_t hue = map(actualFrame, 0, effects[effectIndex].effectFrames -1, 0, 255);
  // FastLED's built-in rainbow generator
  fill_rainbow( leds, NUM_LEDS, hue, 7);
}

void rainbowWithGlitter() {
  // built-in FastLED rainbow, plus some random sparkly glitter
  rainbow();
  addGlitter(40);
}

void addGlitter( fract8 chanceOfGlitter)
{
  if( random8() < chanceOfGlitter) {
    leds[ random16(NUM_LEDS) ] += CRGB::White;
  }
}

void confetti() {
  uint8_t effectIndex = npControllerSetting->activeEffect;
  uint8_t hue = map(actualFrame, 0, effects[effectIndex].effectFrames -1, 0, 255);

  // random colored speckles that blink in and fade smoothly
  fadeToBlackBy( leds, NUM_LEDS, 10);
  int pos = random16(NUM_LEDS);
  leds[pos] += CHSV( hue + random8(64), 200, 255);
}

void sinelon() {
  uint8_t effectIndex = npControllerSetting->activeEffect;
  uint8_t hue = map(actualFrame, 0, effects[effectIndex].effectFrames -1, 0, 255);

  // a colored dot sweeping back and forth, with fading trails
  fadeToBlackBy( leds, NUM_LEDS, 20);
  // Beat generators which return sine waves in a specified number of Beats Per Minute.
  // Sine wave beat generators can specify a low and  high range for the output.
  //   beatsin16( BPM, low16, high16) = (sine(beatphase) * (high16-low16)) + low16
  //   beatsin16( BPM, uint16_t low, uint16_t high) returns a 16-bit value
  //                    that rises and falls in a sine wave, 'BPM' times per
  //                    minute, between the values of 'low' and 'high'.
  int pos = beatsin16( 13, 0, NUM_LEDS-1 );
  leds[pos] += CHSV( hue, 255, 192);
}

void bpm() {
  uint8_t effectIndex = npControllerSetting->activeEffect;
  uint8_t hue = map(actualFrame, 0, effects[effectIndex].effectFrames -1, 0, 255);

  // colored stripes pulsing at a defined Beats-Per-Minute (BPM)
  uint8_t BeatsPerMinute = 62;
  CRGBPalette16 palette = PartyColors_p;
  uint8_t beat = beatsin8( BeatsPerMinute, 64, 255);
  for( int i = 0; i < NUM_LEDS; i++) { //9948
    leds[i] = ColorFromPalette(palette, hue+(i*2), beat-hue+(i*10));
  }
}

void juggle() {
  // eight colored dots, weaving in and out of sync with each other
  fadeToBlackBy( leds, NUM_LEDS, 20);
  byte dothue = 0;
  for( int i = 0; i < 8; i++) {
    leds[beatsin16( i+7, 0, NUM_LEDS-1 )] |= CHSV(dothue, 200, 255);
    dothue += 32;
  }
}

// Fire2012 by Mark Kriegsman, July 2012
// as part of "Five Elements" shown here: http://youtu.be/knWiGsmgycY
////
// This basic one-dimensional 'fire' simulation works roughly as follows:
// There's a underlying array of 'heat' cells, that model the temperature
// at each point along the line.  Every cycle through the simulation,
// four steps are performed:
//  1) All cells cool down a little bit, losing heat to the air
//  2) The heat from each cell drifts 'up' and diffuses a little
//  3) Sometimes randomly new 'sparks' of heat are added at the bottom
//  4) The heat from each cell is rendered as a color into the leds array
//     The heat-to-color mapping uses a black-body radiation approximation.
//
// Temperature is in arbitrary units from 0 (cold black) to 255 (white hot).
//
// This simulation scales it self a bit depending on NUM_LEDS; it should look
// "OK" on anywhere from 20 to 100 LEDs without too much tweaking.
//
// I recommend running this simulation at anywhere from 30-100 frames per second,
// meaning an interframe delay of about 10-35 milliseconds.
//
// Looks best on a high-density LED setup (60+ pixels/meter).
//
//
// There are two main parameters you can play with to control the look and
// feel of your fire: COOLING (used in step 1 above), and SPARKING (used
// in step 3 above).
//
// COOLING: How much does the air cool as it rises?
// Less cooling = taller flames.  More cooling = shorter flames.
// Default 50, suggested range 20-100
#define COOLING  55

// SPARKING: What chance (out of 255) is there that a new spark will be lit?
// Higher chance = more roaring fire.  Lower chance = more flickery fire.
// Default 120, suggested range 50-200.
#define SPARKING 120


void fire2012() {
// Array of temperature readings at each simulation cell
  static byte heat[NUM_LEDS];

  // Step 1.  Cool down every cell a little
    for( int i = 0; i < NUM_LEDS; i++) {
      heat[i] = qsub8( heat[i],  random8(0, ((COOLING * 10) / NUM_LEDS) + 2));
    }

    // Step 2.  Heat from each cell drifts 'up' and diffuses a little
    for( int k= NUM_LEDS - 1; k >= 2; k--) {
      heat[k] = (heat[k - 1] + heat[k - 2] + heat[k - 2] ) / 3;
    }

    // Step 3.  Randomly ignite new 'sparks' of heat near the bottom
    if( random8() < SPARKING ) {
      int y = random8(7);
      heat[y] = qadd8( heat[y], random8(160,255) );
    }

    // Step 4.  Map from heat cells to LED colors
    for( int j = 0; j < NUM_LEDS; j++) {
      CRGB color = HeatColor( heat[j]);
      int pixelnumber;
      pixelnumber = j;
      leds[pixelnumber] = color;
    }
}

void cylon(){
  CRGB color = CRGB(0xff0000);
  uint8_t EyeSize = 2;
  uint8_t direction = true;
  uint8_t step_;

  uint8_t effectIndex = npControllerSetting->activeEffect;

  if (actualFrame >= (effects[effectIndex].effectFrames/2)) {
    direction = false;
    step_ = map(actualFrame, effects[effectIndex].effectFrames/2, effects[effectIndex].effectFrames -1, 0, NUM_LEDS - EyeSize);
  } else {
    step_ = map(actualFrame, 0, (effects[effectIndex].effectFrames/2)-1, 0, NUM_LEDS - EyeSize);
  }

  if (direction && (step_ < NUM_LEDS - EyeSize)) leds[step_ + 2] = color/(uint8_t)5;
  else if (step_ > 2) leds[NUM_LEDS - 1 - step_ - 2] = color/(uint8_t)5;

  fadeToBlackBy( leds,NUM_LEDS, 96);

  if (direction) for (uint8_t i = 0; i < EyeSize; i++) leds[step_ + i] = color;
  else for (uint8_t i = 0; i < EyeSize; i++) leds[NUM_LEDS - 1 - step_ - i] = color;
}

void sparkle() {
  CRGB color = CRGB(0x101010);
  fadeToBlackBy( leds,NUM_LEDS, 64);
  if (random8() < 60) leds[random8(NUM_LEDS)] = color;
}

void snowsparkle() {
  CRGB colorSnow = CRGB(0x101010);
  CRGB colorWhite = CRGB(0xffffff);
  for(int i = 0; i < NUM_LEDS; i++ ) {
    leds[i] = blend( colorSnow, leds[i], 200 );
  }
  if (random8() < 60) leds[random8(NUM_LEDS)] = colorWhite;
}

void meteor() {
  static uint8_t fall = 0;
  CRGB colorWhite = CRGB(0xffffff);
  uint8_t meteorSize = 3;
  uint8_t meteorTrailDecay = 128;

  if (fall < 248) {
    fall = random8();
    FastLED.clear();
  } else {
    uint8_t effectIndex = npControllerSetting->activeEffect;
    int8_t step_ = map(actualFrame, 0, effects[effectIndex].effectFrames -1, -NUM_LEDS, NUM_LEDS);

    // fade brightness all LEDs one step
    for(int j=0; j<NUM_LEDS; j++) {
      if ( random8(10) > 5)  {        // random trail decay
        leds[j].fadeToBlackBy( meteorTrailDecay );
      }
    }

    // draw meteor
    for(int j = 0; j < meteorSize; j++) {
      if( (( j + step_ + NUM_LEDS/4) >= 0) && ( ( j + step_ + NUM_LEDS/4) < NUM_LEDS) ) {
        leds[j + step_ + NUM_LEDS/4] = colorWhite;
      }
    }
    if (step_ == (NUM_LEDS - 1)) fall = 0;
  }
}

void theatre() {
  uint8_t effectIndex = npControllerSetting->activeEffect;
  uint8_t redWindow = 3;
  uint8_t blackWindow = 2;

  uint8_t step_ = map(actualFrame, 0, effects[effectIndex].effectFrames -1, 0, (redWindow+blackWindow)*10-1);
  step_ = step_/10;

  FastLED.clear();
  for (int8_t i = -redWindow-blackWindow; i < (NUM_LEDS+redWindow); i += (redWindow+blackWindow)) {
    int8_t index = i + step_;
    for (int8_t j = 0; j < redWindow; j++) if ( ((index + j)>=0) && ((index + j)< NUM_LEDS-1)) leds[index + j] = CRGB::Red;
  }
}
