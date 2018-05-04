/*
 * -------------------------------------------------------------------
 * EmonESP Serial to Emoncms gateway
 * -------------------------------------------------------------------
 * Adaptation of Chris Howells OpenEVSE ESP Wifi
 * by Trystan Lea, Glyn Hudson, OpenEnergyMonitor
 * All adaptation GNU General Public License as below.
 *
 * -------------------------------------------------------------------
 *
 * This file is part of OpenEnergyMonitor.org project.
 * EmonESP is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3, or (at your option)
 * any later version.
 * EmonESP is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * You should have received a copy of the GNU General Public License
 * along with EmonESP; see the file COPYING.  If not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 */

#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <FS.h>                       // SPIFFS file-system: store web server html, CSS etc.

#include "emonesp.h"
#include "web_server.h"
#include "config.h"
#include "wifi.h"
#include "mqtt.h"
#include "emoncms.h"
#include "ota.h"
#include "debug.h"
#include "measure.h"
#include "relay.h"
#include "RTCDS1307.h"
#include "SDcard.h"

AsyncWebServer server(80);          //Create class for Web server

bool enableCors = true;
bool scan = false;
// Event timeouts
unsigned long wifiRestartTime = 0;
unsigned long mqttRestartTime = 0;
unsigned long systemRestartTime = 0;
unsigned long systemRebootTime = 0;

// Get running firmware version from build tag environment variable
#define TEXTIFY(A) #A
#define ESCAPEQUOTE(A) TEXTIFY(A)
String currentfirmware = ESCAPEQUOTE(BUILD_TAG);

// -------------------------------------------------------------------
// Helper function to perform the standard operations on a request
// -------------------------------------------------------------------
bool requestPreProcess(AsyncWebServerRequest *request, AsyncResponseStream *&response, const char *contentType = "application/json")
{
  if(www_username!="" && !request->authenticate(www_username.c_str(), www_password.c_str())) {
    request->requestAuthentication();
    return false;
  }

  response = request->beginResponseStream(contentType);
  if(enableCors) {
    response->addHeader("Access-Control-Allow-Origin", "*");
  }

  return true;
}

// -------------------------------------------------------------------
// Load Home page
// url: /
// -------------------------------------------------------------------
void handleHome(AsyncWebServerRequest *request) {
  if (www_username != ""
      && !request->authenticate(www_username.c_str(),www_password.c_str())
      && wifi_mode == WIFI_MODE_STA) {
    return request->requestAuthentication();
  }

  if (SPIFFS.exists("/home.html")) {
    request->send(SPIFFS, "/home.html");
  } else {
    request->send(200, "text/plain",
                  "/home.html not found, have you flashed the SPIFFS?");
  }
}

// -------------------------------------------------------------------
// Wifi scan /scan not currently used
// url: /scan
//
// First request will return 0 results unless you start scan from somewhere else (loop/setup)
// Do not request more often than 3-5 seconds
// -------------------------------------------------------------------
void handleScan(AsyncWebServerRequest *request) {
  AsyncResponseStream *response;
  if(false == requestPreProcess(request, response)) {
    return;
  }
  String json = "[";
  int n = WiFi.scanComplete();
  if(n == -2) {
    WiFi.scanNetworks(true);
  } else if(n) {
    for (int i = 0; i < n; ++i) {
      if(i) json += ",";
      json += "{";
      json += "\"rssi\":"+String(WiFi.RSSI(i));
      json += ",\"ssid\":\""+WiFi.SSID(i)+"\"";
      json += ",\"bssid\":\""+WiFi.BSSIDstr(i)+"\"";
      json += ",\"channel\":"+String(WiFi.channel(i));
      json += ",\"secure\":"+String(WiFi.encryptionType(i));
      json += ",\"hidden\":"+String(WiFi.isHidden(i)?"true":"false");
      json += "}";
    }
    WiFi.scanDelete();
    if(WiFi.scanComplete() == -2){
      WiFi.scanNetworks(true);
    }
    json += "]";
  }
  scan=true;
  request->send(200, "text/json", json);
}

// -------------------------------------------------------------------
// Handle turning Access point off
// url: /apoff
// -------------------------------------------------------------------
void handleAPOff(AsyncWebServerRequest *request) {
  AsyncResponseStream *response;
  if(false == requestPreProcess(request, response, "text/plain")) {
    return;
  }
  response->setCode(200);
  response->print("Turning AP Off");
  request->send(response);
  DBUGLN("Turning AP Off");
  systemRebootTime = millis() + 1000;
}

// -------------------------------------------------------------------
// Save selected network to EEPROM and attempt connection
// url: /savenetwork
// -------------------------------------------------------------------
void handleSaveNetwork(AsyncWebServerRequest *request) {
  AsyncResponseStream *response;
  if(false == requestPreProcess(request, response, "text/plain")) {
    return;
  }

  String qsid = request->arg("ssid");
  String qpass = request->arg("pass");

  if (qsid != 0) {
    config_save_wifi(qsid, qpass);

    response->setCode(200);
    response->print("saved");
    wifiRestartTime = millis() + 2000;
  } else {
    response->setCode(400);
    response->print("No SSID");
  }

  request->send(response);
}

// -------------------------------------------------------------------
// Save Emoncms
// url: /saveemoncms
// -------------------------------------------------------------------
void handleSaveEmoncms(AsyncWebServerRequest *request) {
  AsyncResponseStream *response;
  if(false == requestPreProcess(request, response, "text/plain")) {
    return;
  }

  config_save_emoncms(request->arg("server"),
                      request->arg("node"),
                      request->arg("apikey"),
                      request->arg("fingerprint"));

  char tmpStr[200];
  snprintf(tmpStr, sizeof(tmpStr), "Saved: %s %s %s %s",
           emoncms_server.c_str(),
           emoncms_node.c_str(),
           emoncms_apikey.c_str(),
           emoncms_fingerprint.c_str());
  DBUGLN(tmpStr);

  response->setCode(200);
  response->print(tmpStr);
  request->send(response);
}

// -------------------------------------------------------------------
// Save MQTT Config
// url: /savemqtt
// -------------------------------------------------------------------
void handleSaveMqtt(AsyncWebServerRequest *request) {
  AsyncResponseStream *response;
  if(false == requestPreProcess(request, response, "text/plain")) {
    return;
  }

  config_save_mqtt(request->arg("server"),
                   request->arg("topic"),
                   request->arg("prefix"),
                   request->arg("user"),
                   request->arg("pass"));

  char tmpStr[200];
  snprintf(tmpStr, sizeof(tmpStr), "Saved: %s %s %s %s %s", mqtt_server.c_str(),
           mqtt_topic.c_str(), mqtt_feed_prefix.c_str(), mqtt_user.c_str(), mqtt_pass.c_str());
  DBUGLN(tmpStr);

  response->setCode(200);
  response->print(tmpStr);
  request->send(response);

  // If connected disconnect MQTT to trigger re-connect with new details
  mqttRestartTime = millis();
}

// -------------------------------------------------------------------
// Save the web site user/pass
// url: /saveadmin
// -------------------------------------------------------------------
void handleSaveAdmin(AsyncWebServerRequest *request) {
  AsyncResponseStream *response;
  if(false == requestPreProcess(request, response, "text/plain")) {
    return;
  }
  String quser = request->arg("user");
  String qpass = request->arg("pass");
  config_save_admin(quser, qpass);
  response->setCode(200);
  response->print("saved");
  request->send(response);
}

void handleSaveScheduler(AsyncWebServerRequest *request) {
  AsyncResponseStream *response;
  if(false == requestPreProcess(request, response, "text/plain")) {
    return;
  }
  config_save_scheduler(request->arg("datestart"),
                   request->arg("timestart"),
                   request->arg("datestop"),
                   request->arg("timestop"));
  Serial.print("Date Start : ");Serial.println(sch_datestart);
  Serial.print("Time Start : ");Serial.println(sch_timestart);
  Serial.print("Date Stop : ");Serial.println(sch_datestop);
  Serial.print("Time Stop : ");Serial.println(sch_timestop);
  response->setCode(200);
  response->print("saved");
  request->send(response);
}
// -------------------------------------------------------------------
// Last values on atmega serial
// url: /lastvalues
// -------------------------------------------------------------------
void handleLastValues(AsyncWebServerRequest *request) {
  AsyncResponseStream *response;
  if(false == requestPreProcess(request, response, "text/plain")) {
    return;
  }
  response->setCode(200);
  response->print(displaydatavalues);
  request->send(response);
}

// -------------------------------------------------------------------
// Returns status json
// url: /status
// -------------------------------------------------------------------
void handleStatus(AsyncWebServerRequest *request) {
  AsyncResponseStream *response;
  if(false == requestPreProcess(request, response)) {
    return;
  }
  String s = "{";
  s += "\"chip_id\":\""+String(ESP.getChipId())+"\",";
  s += "\"CurrentTime\":\""+RTCTime+"\",";
  s += "\"TimeOperation\":\""+String(millis())+"\",";
  s += "\"timemqtt\":\""+String(timeMQTT)+"\",";
  s += "\"count_mqttdisconnect\":\""+String(count_mqttdisconnect)+"\",";
  if (wifi_mode==WIFI_MODE_STA) {
    s += "\"mode\":\"STA\",";
  } else if (wifi_mode == WIFI_MODE_AP_STA_RETRY
             || wifi_mode == WIFI_MODE_AP_ONLY) {
    s += "\"mode\":\"AP\",";
  } else if (wifi_mode==WIFI_MODE_AP_AND_STA) {
    s += "\"mode\":\"STA+AP\",";
  }
  s += "\"networks\":["+st+"],";
  s += "\"rssi\":["+rssi+"],";

  s += "\"srssi\":\""+String(WiFi.RSSI())+"\",";
  s += "\"ipaddress\":\""+ipaddress+"\",";
  s += "\"emoncms_connected\":\""+String(emoncms_connected)+"\",";
  s += "\"packets_sent\":\""+String(packets_sent)+"\",";
  s += "\"packets_success\":\""+String(packets_success)+"\",";
  s += "\"mqtt_connected\":\""+String(mqtt_connected())+"\",";
  s += "\"free_heap\":\"" + String(ESP.getFreeHeap()) + "\",";

  s += "\"CurrentTime\":\""+RTCTime+"\",";
  s += "\"sch_datestart\":\""+sch_datestart+"\",";
  s += "\"sch_timestart\":\""+sch_timestart+"\",";
  s += "\"sch_datestop\":\""+sch_datestop+"\",";
  s += "\"sch_timestop\":\""+sch_timestop+"\",";
  s += "\"sch_state\":\""+sch_state+"\",";
  s += "\"state_load\":\""+String(state_load)+"\",";
  s += "\"sdcard_state\":\""+SDcard_State+"\",";
  s += "\"sdcard_init\":\""+String(initsdcard)+"\"";
#ifdef ENABLE_LEGACY_API
  s += ",\"version\":\"" + currentfirmware + "\"";
  s += ",\"ssid\":\"" + esid + "\"";
  //s += ",\"pass\":\""+epass+"\""; security risk: DONT RETURN PASSWORDS
  s += ",\"emoncms_server\":\"" + emoncms_server + "\"";
  s += ",\"emoncms_node\":\"" + emoncms_node + "\"";
  //s += ",\"emoncms_apikey\":\""+emoncms_apikey+"\""; security risk: DONT RETURN APIKEY
  s += ",\"emoncms_fingerprint\":\"" + emoncms_fingerprint + "\"";
  s += ",\"mqtt_server\":\"" + mqtt_server + "\"";
  s += ",\"mqtt_topic\":\"" + mqtt_topic + "\"";
  s += ",\"mqtt_user\":\"" + mqtt_user + "\"";
  //s += ",\"mqtt_pass\":\""+mqtt_pass+"\""; security risk: DONT RETURN PASSWORDS
  s += ",\"mqtt_feed_prefix\":\""+mqtt_feed_prefix+"\"";
  s += ",\"www_username\":\"" + www_username + "\"";
  //s += ",\"www_password\":\""+www_password+"\""; security risk: DONT RETURN PASSWORDS
#endif
  s += "}";

  response->setCode(200);
  response->print(s);
  request->send(response);
}

// -------------------------------------------------------------------
// Returns OpenEVSE Config json
// url: /config
// -------------------------------------------------------------------
void handleConfig(AsyncWebServerRequest *request) {
  AsyncResponseStream *response;
  if(false == requestPreProcess(request, response)) {
    return;
  }
  String s = "{";
  s += "\"espflash\":\"" + String(ESP.getFlashChipSize()) + "\",";
  s += "\"version\":\"" + currentfirmware + "\",";

  s += "\"ssid\":\"" + esid + "\",";
  //s += "\"pass\":\""+epass+"\","; security risk: DONT RETURN PASSWORDS
  s += "\"emoncms_server\":\"" + emoncms_server + "\",";
  s += "\"emoncms_node\":\"" + emoncms_node + "\",";
  // s += "\"emoncms_apikey\":\""+emoncms_apikey+"\","; security risk: DONT RETURN APIKEY
  s += "\"emoncms_fingerprint\":\"" + emoncms_fingerprint + "\",";
  s += "\"mqtt_server\":\"" + mqtt_server + "\",";
  s += "\"mqtt_topic\":\"" + mqtt_topic + "\",";
  s += "\"mqtt_feed_prefix\":\"" + mqtt_feed_prefix + "\",";
  s += "\"mqtt_user\":\"" + mqtt_user + "\",";
  s += "\"schdatestart\":\""+sch_datestart+"\",";
  s += "\"schtimestart\":\""+sch_timestart+"\",";
  s += "\"schdatestop\":\""+sch_datestop+"\",";
  s += "\"schtimestop\":\""+sch_timestop+"\",";
  //s += "\"mqtt_pass\":\""+mqtt_pass+"\","; security risk: DONT RETURN PASSWORDS
  s += "\"www_username\":\"" + www_username + "\"";
  //s += "\"www_password\":\""+www_password+"\","; security risk: DONT RETURN PASSWORDS
  s += "}";

  response->setCode(200);
  response->print(s);
  request->send(response);
}

// -------------------------------------------------------------------
// Reset config and reboot
// url: /reset
// -------------------------------------------------------------------
void handleRst(AsyncWebServerRequest *request) {
  AsyncResponseStream *response;
  if(false == requestPreProcess(request, response, "text/plain")) {
    return;
  }
  config_reset();
  ESP.eraseConfig();
  response->setCode(200);
  response->print("1");
  request->send(response);
  systemRebootTime = millis() + 1000;
}

// -------------------------------------------------------------------
// Restart (Reboot)
// url: /restart
// -------------------------------------------------------------------
void handleRestart(AsyncWebServerRequest *request) {
  AsyncResponseStream *response;
  if(false == requestPreProcess(request, response, "text/plain")) {
    return;
}

  response->setCode(200);
  response->print("1");
  request->send(response);
  systemRestartTime = millis() + 1000;
}

// -------------------------------------------------------------------
// Check for updates and display current version
// url: /firmware
// -------------------------------------------------------------------
void handleUpdateCheck(AsyncWebServerRequest *request) {
  AsyncResponseStream *response;
  if(false == requestPreProcess(request, response)) {
    return;
  }
  DBUGLN("Running: " + currentfirmware);
  // Get latest firmware version number
  // BUG/HACK/TODO: This will block, should be done in the loop call
  String latestfirmware = ota_get_latest_version();
  DBUGLN("Latest: " + latestfirmware);
  // Update web interface with firmware version(s)
  String s = "{";
  s += "\"current\":\""+currentfirmware+"\",";
  s += "\"latest\":\""+latestfirmware+"\"";
  s += "}";
  response->setCode(200);
  response->print(s);
  request->send(response);
}

void handleNotFound(AsyncWebServerRequest *request)
{
  DBUG("NOT_FOUND: ");
  if(request->method() == HTTP_GET) {
    DBUGF("GET");
  } else if(request->method() == HTTP_POST) {
    DBUGF("POST");
  } else if(request->method() == HTTP_DELETE) {
    DBUGF("DELETE");
  } else if(request->method() == HTTP_PUT) {
    DBUGF("PUT");
  } else if(request->method() == HTTP_PATCH) {
    DBUGF("PATCH");
  } else if(request->method() == HTTP_HEAD) {
    DBUGF("HEAD");
  } else if(request->method() == HTTP_OPTIONS) {
    DBUGF("OPTIONS");
  } else {
    DBUGF("UNKNOWN");
  }
  DBUGF(" http://%s%s", request->host().c_str(), request->url().c_str());

  if(request->contentLength()){
    DBUGF("_CONTENT_TYPE: %s", request->contentType().c_str());
    DBUGF("_CONTENT_LENGTH: %u", request->contentLength());
  }

  int headers = request->headers();
  int i;
  for(i=0; i<headers; i++) {
    AsyncWebHeader* h = request->getHeader(i);
    DBUGF("_HEADER[%s]: %s", h->name().c_str(), h->value().c_str());
  }

  int params = request->params();
  for(i = 0; i < params; i++) {
    AsyncWebParameter* p = request->getParam(i);
    if(p->isFile()){
      DBUGF("_FILE[%s]: %s, size: %u", p->name().c_str(), p->value().c_str(), p->size());
    } else if(p->isPost()){
      DBUGF("_POST[%s]: %s", p->name().c_str(), p->value().c_str());
    } else {
      DBUGF("_GET[%s]: %s", p->name().c_str(), p->value().c_str());
    }
  }

  request->send(404);
}

void handleLoadON(AsyncWebServerRequest *request) {
  AsyncResponseStream *response;
  if(false == requestPreProcess(request, response)) {
    return;
  }
  TurnONLoad();
  response->setCode(200);
  response->print("1");
  request->send(response);
  if(mqtt_connected()){
    mqtt_publish(mqtt_pub_loadstate, String(state_load));
  }
}
void handleLoadOFF(AsyncWebServerRequest *request) {
  AsyncResponseStream *response;
  if(false == requestPreProcess(request, response)) {
    return;
  }
  TurnOFFLoad();
  response->setCode(200);
  response->print("1");
  request->send(response);
  if(mqtt_connected()){
    mqtt_publish(mqtt_pub_loadstate, String(state_load));
  }
}
void handle_sch_TurnON(AsyncWebServerRequest *request) {
  AsyncResponseStream *response;
  if(false == requestPreProcess(request, response)) {
    return;
  }
  config_save_state_scheduler("1");
  response->setCode(200);
  response->print("1");
  request->send(response);
}

void handle_sch_TurnOFF(AsyncWebServerRequest *request) {
  AsyncResponseStream *response;
  if(false == requestPreProcess(request, response)) {
    return;
  }
  config_save_state_scheduler("0");
  response->setCode(200);
  response->print("1");
  request->send(response);
}

void handle_SDactive(AsyncWebServerRequest *request) {
  AsyncResponseStream *response;
  if(false == requestPreProcess(request, response)) {
    return;
  }
  config_save_state_SDcard("1");
  response->setCode(200);
  response->print("1");
  request->send(response);
}
void handle_SDdeactive(AsyncWebServerRequest *request) {
  AsyncResponseStream *response;
  if(false == requestPreProcess(request, response)) {
    return;
  }
  config_save_state_SDcard("0");
  response->setCode(200);
  response->print("1");
  request->send(response);
}
void handle_SDreset(AsyncWebServerRequest *request) {
  AsyncResponseStream *response;
  if(false == requestPreProcess(request, response)) {
    return;
  }
  SD_delete("database.txt");
  response->setCode(200);
  response->print("1");
  request->send(response);
}

void web_server_setup()
{
  SPIFFS.begin(); // mount the fs
  // Setup the static files
  server.serveStatic("/", SPIFFS, "/")
    .setDefaultFile("index.html")
    .setAuthentication(www_username.c_str(), www_password.c_str());
  // Start server & server root html /
  server.on("/",handleHome);
  // Handle HTTP web interface button presses
  server.on("/generate_204", handleHome);  //Android captive portal. Maybe not needed. Might be handled by notFound
  server.on("/fwlink", handleHome);  //Microsoft captive portal. Maybe not needed. Might be handled by notFound
  server.on("/status", handleStatus);
  server.on("/config", handleConfig);
  server.on("/savenetwork", handleSaveNetwork);
  server.on("/saveemoncms", handleSaveEmoncms);
  server.on("/savemqtt", handleSaveMqtt);
  server.on("/saveadmin", handleSaveAdmin);
  server.on("/savescheduler", handleSaveScheduler);
  server.on("/reset", handleRst);
  server.on("/restart", handleRestart);
  server.on("/scan", handleScan);
  server.on("/apoff", handleAPOff);
  server.on("/lastvalues", handleLastValues);
  server.on("/load_on", handleLoadON);
  server.on("/load_off", handleLoadOFF);
  server.on("/schTurnON", handle_sch_TurnON);
  server.on("/schTurnOFF", handle_sch_TurnOFF);
  server.on("/SDactive", handle_SDactive);
  server.on("/SDdeactive", handle_SDdeactive);
  server.on("/SDreset", handle_SDreset);
  server.onNotFound(handleNotFound);
  server.begin();
}

void
web_server_loop() {
  if(scan){scanWiFi();scan=false;}
  // Do we need to restart the WiFi?
  if(wifiRestartTime > 0 && millis() > wifiRestartTime) {
    wifiRestartTime = 0;
    wifi_restart();
  }
  // Do we need to restart MQTT?
  if(mqttRestartTime > 0 && millis() > mqttRestartTime) {
    mqttRestartTime = 0;
    mqtt_restart();
  }
  // Do we need to restart the system?
  if(systemRestartTime > 0 && millis() > systemRestartTime) {
    systemRestartTime = 0;
    wifi_disconnect();
    ESP.restart();
  }
  // Do we need to reboot the system?
  if(systemRebootTime > 0 && millis() > systemRebootTime) {
    systemRebootTime = 0;
    wifi_disconnect();
    ESP.reset();
  }
}
