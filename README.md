# ESP8266 Neopixel Controller
I have bought a couple of ESP-01S boards without any project really in mind. But recently I have discovered cool looking WS2812B (Neopixel) adapters for these boards, so that gave me an idea for a project.
After several iterations I have developed little device, that enables to control WS2812B strips/rings either throught web interface, through MQTT commands or through REST API. It has also one button, that can switch various colors and effects. 
What can it do:
* provides web interface that enables to select any color, save it to EEPROM, start an effect
* when enabled, the WS1228B strips can be controlled via MQTT commands (color, effect, off)
* REST API is also available to set color or effect
* button click cycles through saved colors, double click cycles through available effects and long click turns Neopixels off
Project can be used in home automation systems as it provides several interfaces that could be easily integrated into for example Node-Red.
**WIFI network is required**.

## Prerequisites
### Hardware
It is possible to use any ESP8266 board, but I used specifically ESP-01S boards (non-affiliated link):
* [WS2812B adapter for ESP-01S](https://www.aliexpress.com/item/32894024068.html?spm=a2g0s.9042311.0.0.27424c4d4m1lEc)
* [ESP-01S](https://www.aliexpress.com/item/32809618395.html?spm=a2g0s.9042311.0.0.27424c4deEkjUk) - you need the "S" version as it have more falsh memory (1MB) 
* [Momentary push button](https://www.aliexpress.com/item/32812446715.html?spm=a2g0s.9042311.0.0.27424c4d1w7nAk)
* [Micro USB connector](https://www.aliexpress.com/item/32394000673.html?spm=a2g0s.9042311.0.0.27424c4dAlAlbz)
* [16x WS1228B (Neopixel) ring](https://www.aliexpress.com/item/33006920763.html?spm=a2g0s.9042311.0.0.27424c4dT2O7Fa) - you should be able to use even more Neopixels
* [Andonstar ADSM201 microscope](https://www.aliexpress.com/item/32614052280.html?spm=2114.12010612.8148356.5.1d8e66adaiGyX6)

The push button is connected between GND and GPIO3 on the ESP-01S. When using the WS2812B adapter for ESP-01S, the control pin is GPIO2.

### Software
Project was tested with the following versions:
* Arduino IDE 1.8.10
* ESP8266 core 2.6.0
* [ESP8266 Sketch Data Upload](https://github.com/esp8266/arduino-esp8266fs-plugin)

Libraries:
* [ArduinoJSON 6.12.0](https://github.com/bblanchon/ArduinoJson)
* [ESP_EEPROM 2.0.0](https://github.com/jwrw/ESP_EEPROM)
* [FastLed 3.3](https://github.com/FastLED/FastLED)
* [WifiManages 0.15.0-beta](https://github.com/tzapu/WiFiManager)
* [PubSubClient 2.7](https://github.com/knolleary/pubsubclient)
* [PinButton](https://github.com/poelstra/arduino-multi-button)

## Setup
1. Download content of this repository to your Arduino folder (probably `~\Documents\Arduino\ESP8266_Neopixel_Controller`, folder should contain one `*.ino` file and `data` folder) and then open `ESP8266_Neopixel_Controller.ino`.
1. Set pin for WS2812B input `#define LED_DATA_PIN 2` - if using the recommended adapter board, do not change
1. Set number of WS2812B LEDs `#define NUM_LEDS 16`
1. Set pin for the button `#define BUTTON_PIN 3`
1. You can set a hostname by changing `#define ESP_HOSTNAME "NPCTRL001"`
1. If you want to enable MQTT API (choosing colors with MQTT messages), uncomment `//#define USE_MQTT`
   1. In section `#ifdef USE_MQTT` set connection to your MQTT broker and topics, you want to use.
   1. You should increase `MQTT_MAX_PACKET_SIZE` in PubSubClient.h. For 10 colors is 512 sufficient - `#define MQTT_MAX_PACKET_SIZE 512`
1. If you want to assign static IP address to the board in your WIFI network, uncomment and set following line `//wifiManager.setSTAStaticIPConfig(IPAddress(192,168,0,99), IPAddress(192,168,0,1), IPAddress(255,255,255,0));`
1. Choose the right board and select Flash size with at least 128kB SPIFFS, for ESP-01S it should be "Generic ESP8266 Module" and "1M (128k SPIFFS)"
1. Upload the code.
1. Run ESP8266 Sketch Data Upload to upload content of `/data` folder to SPIFFS on ESP8266. Great article is [Chapter 11 - SPIFFS](https://tttapa.github.io/ESP8266/Chap11%20-%20SPIFFS.html). ESP8266 Sketch Data Upload plug-in to load data on SPIFFS can be downloaded from [here](https://github.com/esp8266/arduino-esp8266fs-plugin). `/data` folder contains html code, CSS styles and JavaScript code for the webserver.
   1. Before stating upload to SPIFFS make sure, that Serial monitor window is not opened, otherwise the upload will fail.
1. Now the ESP8266 will start in AP mode and creates WIFI network with the name you set as hostname. Connect to this network and you should be redirected to URL 192.168.0.1 (if not, enter this address in your browser). On this page set up WIFI connection to your network. ESP8266 will try to connect to it.
1. Either check your router or Serial monitor to find out, what IP address the ESP8266 got. Enter this address in your browser.
1. You enter a web interface with 2 tabs, 1st tab is used to set color of Neopixels or start an effect (it is empty, until you  save some colots). 2nd tab is used to choose and save new colors.

## REST API
The provided web interface itself uses this REST API and you can easily integrate it in other projects.
* GET commands
  * http://IPADDRESS/api?color=RRGGBB&brightness=xx
    * set color of the Neopixels, returns JSON with information about success or error
  * http://IPADDRESS/api?all=1
    * returns JSON with saved colors
  * http://IPADDRESS/api?off=1
    * turn Neopixels off, returns JSON with information about success or error
  * http://IPADDRESS/api?effect=effect_name
    * display desired effect, returns JSON with information about success or error, effect names in variable char Effects[][10]
* POST commands
  * http://IPADDRESS/api?save=1&color=RRGGBB&brightness=xx
    * save new color to EEPROM, returns JSON with information about success or error
  * http://IPADDRESS/api?delete=1&color=RRGGBB&brightness=xx
    * delete color from EEPROM, returns JSON with information about success or error

## MQTT API
If you enable MQTT API, then the ESP8266 will wait for commands in topic:
* MQTT_TOPIC/cmd/color
  * payload: RRGGBB/xx
  * set color and brightness of Neopixels
* MQTT_TOPIC/cmd/effect
  * payload: effect_name
  * starts chosen effect - "rainbow", "rainbowGl", "confetti", "sinelon", "bpm", "juggle", "fire2012", "cylon", "sparkle", "snowspark", "meteor", "theatre"
* MQTT_TOPIC/cmd/all
  * returns JSON with saved colors in topic MQTT_TOPIC/rspn
* MQTT_TOPIC/cmd/off
  * turns Neopixels off

## Button interface
You can use a button clicks to set color, start an effect or turn Neopixels off. The Button should be connected between GND and GPIO3 on the ESP01-S board.
* Single click - sets next saved color
* Double click - starts next effect
* Long click - turns Neopixels off

## STL for case
In folder `stl` you will find `*.stl` files for your 3D printer, so you can print nice case for this project.
More information about the case and photos can be found on [Thingiverse](https://www.thingiverse.com/thing:3930823).
