#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <ESP8266mDNS.h>
#include <ESP8266WebServer.h>
#include <ESP8266HTTPClient.h>
#include <ArduinoJson.h>
#include <U8g2lib.h>
#ifdef ESP8266
#define ESP8266_DRD_USE_RTC false //true
#define ESP_DRD_USE_LITTLEFS true //false
#endif
#define DOUBLERESETDETECTOR_DEBUG true //false
#include <ESP_DoubleResetDetector.h>

// Display lib U8G2 settings, default with hardware I2C
U8G2_SSD1306_128X64_NONAME_1_HW_I2C u8g2(U8G2_R0, /* reset=*/U8X8_PIN_NONE);
//U8G2_SSD1306_128X64_ALT0_1_HW_I2C u8g2(U8G2_R0, /* reset=*/U8X8_PIN_NONE); // same as the NONAME variant, but may solve the "every 2nd line skipped" problem
//U8G2_SSD1306_128X64_NONAME_1_SW_I2C u8g2(U8G2_R0, /* clock=*/SCL, /* data=*/SDA, /* reset=*/U8X8_PIN_NONE);

#define U8LOG_WIDTH 20
#define U8LOG_HEIGHT 6
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
uint8_t u8log_buffer[U8LOG_WIDTH * U8LOG_HEIGHT];
U8G2LOG u8g2log;
uint8_t imgmem[SCREEN_WIDTH * SCREEN_HEIGHT / 8];

// Select the FileSystem by uncommenting one of the lines below
//#define USE_SPIFFS
#define USE_LITTLEFS
//#define USE_SDFS

#if defined USE_SPIFFS
#include <FS.h>
const char *fsName = "SPIFFS";
FS *fileSystem = &SPIFFS;
SPIFFSConfig fileSystemConfig = SPIFFSConfig();
#elif defined USE_LITTLEFS
#include <LittleFS.h>
const char *fsName = "LittleFS";
FS *fileSystem = &LittleFS;
LittleFSConfig fileSystemConfig = LittleFSConfig();
#elif defined USE_SDFS
#include <SDFS.h>
const char *fsName = "SDFS";
FS *fileSystem = &SDFS;
SDFSConfig fileSystemConfig = SDFSConfig();
// fileSystemConfig.setCSPin(chipSelectPin);
#else
#error Please select a filesystem first by uncommenting one of the "#define USE_xxx" lines at the beginning of the sketch.
#endif

#define DRD_TIMEOUT 10U
// RTC Memory Address for the DoubleResetDetector to use
#define DRD_ADDRESS 0
DoubleResetDetector *drd;
#define DBG_OUTPUT_PORT Serial
static bool fsOK;
String unsupportedFiles = String();
File uploadFile;
static const char TEXT_PLAIN[] PROGMEM = "text/plain";
static const char FS_INIT_ERROR[] PROGMEM = "FS INIT ERROR";
static const char FILE_NOT_FOUND[] PROGMEM = "FileNotFound";
ESP8266WebServer server(80);
HTTPClient http;
WiFiClient client;
//Wifi settings
WiFiMode_t wifi_mode = WIFI_AP; //default work in AP mode
char APssid[64] = "esp8266AP";
char APpassword[64] = "12345678";
char STAssid[64] = "your-wifi";
char STApassword[64] = "";
bool PlayFlag = false;
uint8_t imgWidth = 0;
uint8_t imgHeight = 0;
uint8_t player_mode = 1;
char LocalFilePath[32] = "/LocalFilePath";
uint8_t LocalFileFPS = 5;            //default local player FPS
char Client_Server_Address[64] = ""; //default Client-mode Server address
int Client_Server_Port = 8002;       //default Client-mode Server port
int Server_Listen_Port = 8001;       //default Server-mode Server port

bool startFlag = true;

#ifdef USE_SPIFFS
/*
   Checks filename for character combinations that are not supported by FSBrowser (alhtough valid on SPIFFS).
   Returns an empty String if supported, or detail of error(s) if unsupported
*/
String checkForUnsupportedPath(String filename)
{
  String error = String();
  if (!filename.startsWith("/"))
  {
    error += F("!NO_LEADING_SLASH! ");
  }
  if (filename.indexOf("//") != -1)
  {
    error += F("!DOUBLE_SLASH! ");
  }
  if (filename.endsWith("/"))
  {
    error += F("!TRAILING_SLASH! ");
  }
  return error;
}
#endif

////////////////////////////////
// Utils to return HTTP codes, and determine content-type
void replyOK()
{
  server.send(200, FPSTR(TEXT_PLAIN), "");
}

void replyOKWithMsg(String msg)
{
  server.send(200, FPSTR(TEXT_PLAIN), msg);
}

void replyNotFound(String msg)
{
  server.send(404, FPSTR(TEXT_PLAIN), msg);
}

void replyBadRequest(String msg)
{
  DBG_OUTPUT_PORT.println(msg);
  server.send(400, FPSTR(TEXT_PLAIN), msg + "\r\n");
}

void replyServerError(String msg)
{
  DBG_OUTPUT_PORT.println(msg);
  server.send(500, FPSTR(TEXT_PLAIN), msg + "\r\n");
}

void readConfig()
{
  //WiFi config
  File wifiConfigFile;
  if (!fileSystem->exists("/wifi-config.json"))
  {
    Serial.println("Can't open wifi-config，will auto generate.");
    wifiConfigFile = fileSystem->open("/wifi-config.json", "w+");
    wifiConfigFile.close(); //关闭释放指针
  }
  wifiConfigFile = fileSystem->open("/wifi-config.json", "r");
  if (wifiConfigFile)
  { //  Read wifi settings
    size_t size = wifiConfigFile.size();
    // Allocate a buffer to store contents of the file.
    std::unique_ptr<char[]> buf(new char[size]);
    wifiConfigFile.readBytes(buf.get(), size);
    DynamicJsonBuffer jsonBuffer;
    JsonObject &json = jsonBuffer.parseObject(buf.get());
    if (json.success())
    {
      if (json.containsKey("mode"))
      { //mode: 1->STA , 2->WIFI_AP , 3->AP+STA
        wifi_mode = (WiFiMode_t)json["mode"].as<int>();
      }
      if (json.containsKey("APssid"))
      {
        strcpy(APssid, json["APssid"].as<char *>());
      }
      if (json.containsKey("APpassword"))
      {
        strcpy(APpassword, json["APpassword"].as<char *>());
      }
      if (json.containsKey("STAssid"))
      {
        strcpy(STAssid, json["STAssid"].as<char *>());
      }
      if (json.containsKey("STApassword"))
      {
        strcpy(STApassword, json["STApassword"].as<char *>());
      }
    }
    jsonBuffer.clear(); //Clear json buffer
    Serial.println("Read wifi-config successfully.");
    u8g2log.println("Read /wifi-config.json successfully");
  }
  else
  {
    Serial.println("Read wifi-config failed. Use default.");
    u8g2log.println("Read /wifi-config.json failed.Use default.");
  }
  wifiConfigFile.close();

  //Player config
  File playerConfigFile;
  if (!fileSystem->exists("/player-config.json"))
  {
    Serial.println("Can't open player-config，will auto generate.");
    playerConfigFile = fileSystem->open("/player-config.json", "w+");
    playerConfigFile.close(); //关闭释放指针
  }
  playerConfigFile = fileSystem->open("/player-config.json", "r");
  if (playerConfigFile)
  { //  Read wifi settings
    size_t size = playerConfigFile.size();
    // Allocate a buffer to store contents of the file.
    std::unique_ptr<char[]> buf(new char[size]);
    playerConfigFile.readBytes(buf.get(), size);
    DynamicJsonBuffer jsonBuffer;
    JsonObject &json = jsonBuffer.parseObject(buf.get());
    if (json.success())
    {
      if (json.containsKey("mode"))
      { //mode: 1->Local , 2->Client , 3->Server
        player_mode = json["mode"].as<int>();
      }
      if (json.containsKey("LocalFilePath"))
      {
        strcpy(LocalFilePath, json["LocalFilePath"].as<char *>());
      }
      if (json.containsKey("LocalFileFPS"))
      {
        LocalFileFPS = json["LocalFileFPS"].as<int>();
      }
      if (json.containsKey("Client_Server_Address"))
      {
        strcpy(Client_Server_Address, json["Client_Server_Address"].as<char *>());
      }
      if (json.containsKey("Client_Server_Port"))
      {
        Client_Server_Port = json["Client_Server_Port"].as<int>();
      }
      if (json.containsKey("Server_Listen_Port"))
      {
        Server_Listen_Port = json["Server_Listen_Port"].as<int>();
      }
    }
    jsonBuffer.clear(); //Clear json buffer
    Serial.println("Read player-config successfully.");
    u8g2log.println("Read /player-config.json successfully");
  }
  else
  {
    Serial.println("Read player-config failed. Use default.");
    u8g2log.println("Read /player-config.json failed.Use default.");
  }
  playerConfigFile.close();
}

void initWifi()
{
  delay(100);

  WiFi.mode(wifi_mode);
  if (wifi_mode > 1)
  {
    WiFi.softAP(APssid, APpassword);
  }
  if (wifi_mode != 2)
  {
    WiFi.begin(STAssid, STApassword);
  }
  DBG_OUTPUT_PORT.println(String("Wifi set done!Mode:") + String(wifi_mode) + String("\nAP_SSID:") + String(APssid) + String("\nAP_Password:") + String(APpassword) + String("\nSTA_SSID:") + String(STAssid));
  u8g2log.println(String("Wifi set done!Mode:") + String(wifi_mode) + String("\nAP_SSID:") + String(APssid) + String("\nAP_Password:") + String(APpassword) + String("\nSTA_SSID:") + String(STAssid));
}

void deleteRecursive(String path)
{
  File file = fileSystem->open(path, "r");
  bool isDir = file.isDirectory();
  file.close();

  // If it's a plain file, delete it
  if (!isDir)
  {
    fileSystem->remove(path);
    return;
  }

  // Otherwise delete its contents first
  Dir dir = fileSystem->openDir(path);

  while (dir.next())
  {
    deleteRecursive(path + '/' + dir.fileName());
  }

  // Then delete the folder itself
  fileSystem->rmdir(path);
}

void handleRoot()
{
  u8g2log.println((String) "HTTP Request on " + (String)server.uri());
  char temp[2048];
  char wifi_checked[][8] = {"", "", ""};
  char player_checked[][8] = {"", "", ""};
  strcpy(wifi_checked[wifi_mode - 1], "checked");
  strcpy(player_checked[player_mode - 1], "checked");
  snprintf(temp, 2048, "<!DOCTYPE html><html><head><meta charset=\"utf-8\"><title>ESP8266 Config Page</title></head><body>\
<form action=\"api/submit_config?config_type=WIFI\" method=\"post\">\
<h2>WiFi Mode Select</h2><label><input type=\"radio\" name=\"WIFImode\" value=\"1\" %s>STA</label></br>\
<label><input type=\"radio\" name=\"WIFImode\" value=\"2\" %s>AP</label></br>\
<label><input type=\"radio\" name=\"WIFImode\" value=\"3\" %s>STA+AP</label></br>\
<h3>STA Mode Configuration</h3>SSID: <input type=\"text\" name=\"STASSID\" value=\"%s\"></br>\
Password: <input type=\"text\" name=\"STApassword\" value=\"%s\"></br>\
<h3>AP Mode Configuration</h3>SSID: <input type=\"text\" name=\"APSSID\" value=\"%s\"></br>\
Password: <input type=\"text\" name=\"APpassword\" value=\"%s\"></br>\
<input type=\"submit\" value=\"Submit\">\
</form><form action=\"api/submit_config?config_type=Player\" method=\"post\">\
<h2>Player Configuration</h2>\
<h3>Player Mode Select</h3><label><input type=\"radio\" name=\"Playermode\" value=\"1\" %s>Local</label></br>\
<label><input type=\"radio\" name=\"Playermode\" value=\"2\" %s>Client</label></br>\
<label><input type=\"radio\" name=\"Playermode\" value=\"3\" %s>Server</label></br>\
<h3>Local-mode Configuration</h3>File Path:<input type=\"text\" name=\"Player_Local_Filepath\" value=\"%s\"></br>\
FPS: <input type=\"text\" name=\"Player_Local_FPS\" value=\"%d\"></br>\
<h3>Client-mode Configuration</h3>Server address:<input type=\"text\" name=\"Player_Client_Server\" value=\"%s\"></br>\
Port: <input type=\"text\" name=\"Player_Client_Port\" value=\"%d\"></br>\
<h3>Server-mode Configuration</h3>\
Port: <input type=\"text\" name=\"Player_Server_Port\" value=\"%d\"></br>\
<input type=\"submit\" value=\"Submit\">\
</form></body></html>\
",
           wifi_checked[0], wifi_checked[1], wifi_checked[2], STAssid, STApassword, APssid, APpassword,
           player_checked[0], player_checked[1], player_checked[2], LocalFilePath, LocalFileFPS, Client_Server_Address, Client_Server_Port, Server_Listen_Port);
  server.send(200, "text/html", temp);
}

void handleSubmitConfig()
{
  DBG_OUTPUT_PORT.println((String) "HTTP Request on " + (String)server.uri());
  u8g2log.println((String) "HTTP Request on " + (String)server.uri());
  if (!fsOK)
  {
    return replyServerError(FPSTR(FS_INIT_ERROR));
  }
  if (server.method() != HTTP_POST)
  {
    server.send(405, "text/plain", "Method Not Allowed");
  }
  else
  {
    DynamicJsonBuffer postargsbuf;
    JsonObject &postargs = postargsbuf.createObject();
    String message = "POST form was:\n";
    for (uint8_t i = 0; i < server.args(); i++)
    {
      message += " " + server.argName(i) + ": " + server.arg(i) + "\n";
      postargs.set(server.argName(i), server.arg(i));
    }
    if (postargs.containsKey("config_type"))
    {
      if (postargs["config_type"].as<String>() == String("WIFI"))
      {
        wifi_mode = (WiFiMode_t)postargs["WIFImode"].as<int>();
        strcpy(STAssid, postargs["STASSID"].as<String>().c_str());
        strcpy(STApassword, postargs["STApassword"].as<String>().c_str());
        strcpy(APssid, postargs["APSSID"].as<String>().c_str());
        strcpy(APpassword, postargs["APpassword"].as<String>().c_str());
        DynamicJsonBuffer WIFIconfbuf;
        JsonObject &wificonfjson = WIFIconfbuf.createObject();
        wificonfjson.set("mode", wifi_mode);
        wificonfjson.set("STAssid", STAssid);
        wificonfjson.set("STApassword", STApassword);
        wificonfjson.set("APssid", APssid);
        wificonfjson.set("APpassword", APpassword);
        String WIFIjsonfile;
        wificonfjson.printTo(WIFIjsonfile);
        File wificonffilep = fileSystem->open("/wifi-config.json", "w");
        wificonffilep.write(WIFIjsonfile.c_str());
        wificonffilep.close();
        WIFIconfbuf.clear();
      }
      if (postargs["config_type"].as<String>() == String("Player"))
      {
        player_mode = postargs["Playermode"].as<int>();
        strcpy(LocalFilePath, postargs["Player_Local_Filepath"].as<String>().c_str());
        LocalFileFPS = postargs["Player_Local_FPS"].as<int>();
        strcpy(Client_Server_Address, postargs["Player_Client_Server"].as<String>().c_str());
        Client_Server_Port = postargs["Player_Client_Port"].as<int>();
        Server_Listen_Port = postargs["Player_Server_Port"].as<int>();
        DynamicJsonBuffer PLAYERconfbuf;
        JsonObject &playerconfjson = PLAYERconfbuf.createObject();
        playerconfjson.set("mode", player_mode);
        playerconfjson.set("LocalFilePath", LocalFilePath);
        playerconfjson.set("LocalFileFPS", LocalFileFPS);
        playerconfjson.set("Client_Server_Address", Client_Server_Address);
        playerconfjson.set("Client_Server_Port", Client_Server_Port);
        playerconfjson.set("Server_Listen_Port", Server_Listen_Port);
        String PLAYERjsonfile;
        playerconfjson.printTo(PLAYERjsonfile);
        File playerconffilep = fileSystem->open("/player-config.json", "w");
        playerconffilep.write(PLAYERjsonfile.c_str());
        playerconffilep.close();
        PLAYERconfbuf.clear();
      }
    }
    postargsbuf.clear();
    server.send(200, "text/plain", message);
  }
}

String lastExistingParent(String path)
{
  while (!path.isEmpty() && !fileSystem->exists(path))
  {
    if (path.lastIndexOf('/') > 0)
    {
      path = path.substring(0, path.lastIndexOf('/'));
    }
    else
    {
      path = String(); // No slash => the top folder does not exist
    }
  }
  DBG_OUTPUT_PORT.println(String("Last existing parent: ") + path);
  return path;
}

void handleFileList()
{
  DBG_OUTPUT_PORT.println((String) "HTTP Request on " + (String)server.uri());
  u8g2log.println((String) "HTTP Request on " + (String)server.uri());
  if (!fsOK)
  {
    return replyServerError(FPSTR(FS_INIT_ERROR));
  }

  if (!server.hasArg("dir"))
  {
    return replyBadRequest(F("DIR ARG MISSING"));
  }

  String path = server.arg("dir");
  if (path != "/" && !fileSystem->exists(path))
  {
    return replyBadRequest("BAD PATH");
  }

  DBG_OUTPUT_PORT.println(String("handleFileList: ") + path);
  Dir dir = fileSystem->openDir(path);
  path.clear();

  // use HTTP/1.1 Chunked response to avoid building a huge temporary string
  if (!server.chunkedResponseModeStart(200, "text/json"))
  {
    server.send(505, F("text/html"), F("HTTP1.1 required"));
    return;
  }

  // use the same string for every line
  String output;
  output.reserve(64);
  while (dir.next())
  {
#ifdef USE_SPIFFS
    String error = checkForUnsupportedPath(dir.fileName());
    if (error.length() > 0)
    {
      DBG_OUTPUT_PORT.println(String("Ignoring ") + error + dir.fileName());
      continue;
    }
#endif
    if (output.length())
    {
      // send string from previous iteration
      // as an HTTP chunk
      server.sendContent(output);
      output = ',';
    }
    else
    {
      output = '[';
    }

    output += "{\"type\":\"";
    if (dir.isDirectory())
    {
      output += "dir";
    }
    else
    {
      output += F("file\",\"size\":\"");
      output += dir.fileSize();
    }

    output += F("\",\"name\":\"");
    // Always return names without leading "/"
    if (dir.fileName()[0] == '/')
    {
      output += &(dir.fileName()[1]);
    }
    else
    {
      output += dir.fileName();
    }

    output += "\"}";
  }

  // send last string
  output += "]";
  server.sendContent(output);
  server.chunkedResponseFinalize();
}

void handleFileDelete()
{
  DBG_OUTPUT_PORT.println((String) "HTTP Request on " + (String)server.uri());
  u8g2log.println((String) "HTTP Request on " + (String)server.uri());
  if (!fsOK)
  {
    return replyServerError(FPSTR(FS_INIT_ERROR));
  }
  if (server.method() != HTTP_POST)
  {
    server.send(405, "text/plain", "Method Not Allowed");
  }
  String path = server.arg(0);
  if (path.isEmpty() || path == "/")
  {
    return replyBadRequest("BAD PATH");
  }

  DBG_OUTPUT_PORT.println(String("handleFileDelete: ") + path);
  if (!fileSystem->exists(path))
  {
    return replyNotFound(FPSTR(FILE_NOT_FOUND));
  }
  deleteRecursive(path);

  replyOKWithMsg(lastExistingParent(path));
}

void handleNotFound()
{
  DBG_OUTPUT_PORT.println((String) "HTTP Request on " + (String)server.uri());
  u8g2log.println((String) "HTTP Request on " + (String)server.uri());
  String message = "File Not Found\n\n";
  message += "URI: ";
  message += server.uri();
  message += "\nMethod: ";
  message += (server.method() == HTTP_GET) ? "GET" : "POST";
  message += "\nArguments: ";
  message += server.args();
  message += "\n";

  for (uint8_t i = 0; i < server.args(); i++)
  {
    message += " " + server.argName(i) + ": " + server.arg(i) + "\n";
  }

  server.send(404, "text/plain", message);
}

/*
   Handle a file upload request
*/
void handleFileUpload()
{
  DBG_OUTPUT_PORT.println((String) "HTTP Request on " + (String)server.uri());
  u8g2log.println((String) "HTTP Request on " + (String)server.uri());
  if (!fsOK)
  {
    return replyServerError(FPSTR(FS_INIT_ERROR));
  }
  HTTPUpload &upload = server.upload();
  if (upload.status == UPLOAD_FILE_START)
  {
    String filename = upload.filename;
    // Make sure paths always start with "/"
    if (!filename.startsWith("/"))
    {
      filename = "/" + filename;
    }
    DBG_OUTPUT_PORT.println(String("handleFileUpload Name: ") + filename);
    uploadFile = fileSystem->open(filename, "w");
    if (!uploadFile)
    {
      return replyServerError(F("CREATE FAILED"));
    }
    DBG_OUTPUT_PORT.println(String("Upload: START, filename: ") + filename);
  }
  else if (upload.status == UPLOAD_FILE_WRITE)
  {
    if (uploadFile)
    {
      size_t bytesWritten = uploadFile.write(upload.buf, upload.currentSize);
      if (bytesWritten != upload.currentSize)
      {
        return replyServerError(F("WRITE FAILED"));
      }
    }
    DBG_OUTPUT_PORT.println(String("Upload: WRITE, Bytes: ") + upload.currentSize);
  }
  else if (upload.status == UPLOAD_FILE_END)
  {
    if (uploadFile)
    {
      uploadFile.close();
    }
    DBG_OUTPUT_PORT.println(String("Upload: END, Size: ") + upload.totalSize);
  }
}

int hex2byte(unsigned char bits[], char s[]) //uchar* destination bits, char* source HEX
{
  int i, n = 0;
  for (i = 0; s[i]; i += 2)
  {
    if ((s[i] >= 'A' && s[i] <= 'F') || (s[i] >= 'a' && s[i] <= 'f'))
    {
      if (s[i] <= 'F')
        bits[n] = s[i] - 'A' + 10;
      else
        bits[n] = s[i] - 'a' + 10;
    }
    else
      bits[n] = s[i] - '0';
    if ((s[i + 1] >= 'A' && s[i + 1] <= 'F') || (s[i + 1] >= 'a' && s[i + 1] <= 'f'))
    {
      if (s[i + 1] <= 'F')
        bits[n] = (bits[n] << 4) | (s[i + 1] - 'A' + 10);
      else
        bits[n] = (bits[n] << 4) | (s[i + 1] - 'a' + 10);
    }
    else
      bits[n] = (bits[n] << 4) | (s[i + 1] - '0');
    ++n;
  }
  return n;
}

void freshDisplay()
{
  u8g2.firstPage();
  do
  {
    u8g2.drawXBM((SCREEN_WIDTH - imgWidth) / 2, (SCREEN_HEIGHT - imgHeight) / 2, imgWidth, imgHeight, imgmem);
  } while (u8g2.nextPage());
}

void processRes(const char *resJson)
{
  DynamicJsonBuffer jsbf;
  //Serial.println("Start to parse");
  JsonObject &resObj = jsbf.parseObject(resJson);
  //Serial.println("Parse done");
  if (resObj.containsKey("displayHex"))
  {
    //Serial.println("PrepareToConvert");
    hex2byte(imgmem, (char *)resObj["displayHex"].as<char *>());
    //Serial.println("PrepareToFresh");
    freshDisplay();
    jsbf.clear();
  }
}

void PlayerLocalMode()
{
  return;
}

void PlayerClientMode()
{
  DBG_OUTPUT_PORT.println("PlayerClientMode");
  http.begin(client, String("http://") + String(Client_Server_Address) + String(Client_Server_Port)); //HTTP
  http.addHeader("Content-Type", "application/json");
  char postData[64] = "";
  sprintf(postData, "%s%lu%s", "{\"millis\":", millis(), "}");
  if (startFlag)
  {
    DBG_OUTPUT_PORT.println("Start to client play");
    sprintf(postData, "%s%lu%s", "{\"millis\":", millis(), ",\"start\":\"true\"}");
    startFlag = false;
  }
  int httpCode = http.POST(postData);

  // httpCode will be negative on error
  if (httpCode > 0)
  {
    // HTTP header has been send and Server response header has been handled
    Serial.printf("[HTTP] POST... code: %d\n", httpCode);
    // file found at server
    if (httpCode == HTTP_CODE_OK)
    {
      const String &payload = http.getString();
      processRes(payload.c_str());
      delay(10);
      return;
    }
  }
  http.end();
}

void PlayerServerMode()
{
  return;
}

void setup()
{

  ////////////////////////////////
  // SERIAL INIT
  DBG_OUTPUT_PORT.begin(115200);
  DBG_OUTPUT_PORT.setDebugOutput(true);
  DBG_OUTPUT_PORT.print('\n');
  u8g2.begin(); //Start to init display
  u8g2log.begin(u8g2, U8LOG_WIDTH, U8LOG_HEIGHT, u8log_buffer);
  u8g2log.setLineHeightOffset(0);   // set extra space between lines in pixel, this can be negative
  u8g2log.setRedrawMode(0);         // 0: Update screen with newline, 1: Update screen for every char            //
  u8g2.setFont(u8g2_font_t0_11_tr); // choose a suitable font
  //u8g2.enableUTF8Print();
  ////////////////////////////////
  // FILESYSTEM INIT
  fileSystemConfig.setAutoFormat(true);
  fileSystem->setConfig(fileSystemConfig);
  fsOK = fileSystem->begin();
  DBG_OUTPUT_PORT.println(fsOK ? F("Filesystem initialized.") : F("Filesystem init failed!"));
  u8g2log.println(fsOK ? F("Filesystem initialized.") : F("Filesystem init failed!"));
  drd = new DoubleResetDetector(DRD_TIMEOUT, DRD_ADDRESS);
  if (drd->detectDoubleReset())
  {
    DBG_OUTPUT_PORT.println("** Double reset boot **");
    if (fileSystem->begin())
    {
      delay(1000);
      fileSystem->remove("/wifi-config.json");
      fileSystem->remove("/player-config.json");
      fileSystem->end();
      delay(1000);
    }
    ESP.reset();
  }

  readConfig(); // Read configs
  initWifi();
  if (wifi_mode == WIFI_STA)
  {
    int count = 0;
    do
    {
      delay(500);
      count++;
      if (count >= 30)
      {
        wifi_mode = WIFI_AP;
        WiFi.mode(WIFI_OFF);
        initWifi();
        break;
      }
    } while (WiFi.status() != WL_CONNECTED);
  }
  else if (wifi_mode == WIFI_AP_STA)
  {
    int count = 0;
    do
    {
      delay(500);
      count++;
      if (count >= 30)
      {
        wifi_mode = WIFI_AP;
        WiFi.mode(WIFI_OFF);
        initWifi();
        break;
      }
    } while (WiFi.status() != WL_CONNECTED);
  }

  if (MDNS.begin("esp8266"))
  {
    Serial.println("MDNS responder started");
  }
  delay(100);

  server.on("/", handleRoot);
  server.on("/api/submit_config", handleSubmitConfig);
  server.on("/api/list", handleFileList);
  server.on("/api/delete", handleFileDelete);
  // Upload file
  // - first callback is called after the request has ended with all parsed arguments
  // - second callback handles file upload at that location
  server.on("/api/upload", HTTP_POST, replyOK, handleFileUpload);
  server.onNotFound(handleNotFound);
  server.begin();
  Serial.println("HTTP server started");
}

bool drdStopFlag = false;

void loop()
{
  if (millis() < 10000)
  {
    drd->loop();
  }else if(!drdStopFlag)
  {
    drd->stop();
    drdStopFlag = true;
  }
  
  if ((WiFi.getMode() == WIFI_STA) && (WiFi.status() != WL_CONNECTED))
  {
    DBG_OUTPUT_PORT.println("STA mode:Not connected!");
    delay(500);
    return;
  }
  server.handleClient();
  MDNS.update();

  DBG_OUTPUT_PORT.println(player_mode);
  if (player_mode == 1)
  {
    PlayerLocalMode();
  }
  else if (player_mode == 2)
  {
    PlayerClientMode();
  }
  else if (player_mode == 3)
  {
    PlayerServerMode();
  }

  delay(100);
}