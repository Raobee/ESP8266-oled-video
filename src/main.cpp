#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <ESP8266mDNS.h>
#include <ESP8266WebServer.h>
#include <ArduinoJson.h>
#include <U8g2lib.h>

// Display lib U8G2 settings, default with hardware I2C
U8G2_SSD1306_128X64_NONAME_1_HW_I2C u8g2(U8G2_R0, /* reset=*/U8X8_PIN_NONE);
//U8G2_SSD1306_128X64_ALT0_1_HW_I2C u8g2(U8G2_R0, /* reset=*/U8X8_PIN_NONE); // same as the NONAME variant, but may solve the "every 2nd line skipped" problem
//U8G2_SSD1306_128X64_NONAME_1_SW_I2C u8g2(U8G2_R0, /* clock=*/SCL, /* data=*/SDA, /* reset=*/U8X8_PIN_NONE);

#define U8LOG_WIDTH 20
#define U8LOG_HEIGHT 6
uint8_t u8log_buffer[U8LOG_WIDTH * U8LOG_HEIGHT];
U8G2LOG u8g2log;

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

#define DBG_OUTPUT_PORT Serial
static bool fsOK;
String unsupportedFiles = String();
File uploadFile;
static const char TEXT_PLAIN[] PROGMEM = "text/plain";
static const char FS_INIT_ERROR[] PROGMEM = "FS INIT ERROR";
static const char FILE_NOT_FOUND[] PROGMEM = "FileNotFound";

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

ESP8266WebServer server(80);

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

WiFiMode_t wifi_mode = WIFI_AP; //default work in AP mode
char APssid[64] = "esp8266AP";
char APpassword[64] = "12345678";
char STAssid[64] = "your-wifi";
char STApassword[64] = "";

void initWifi()
{
  delay(100);
  if (!fileSystem->exists("/wifi-config.json"))
  {
    Serial.println("Can't open wifi-config，will auto generate.");
    File fp1 = fileSystem->open("/wifi-config.json", "w+");
    fp1.close(); //关闭释放指针
  }
  File wifiConfigFile = fileSystem->open("/wifi-config.json", "r");
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
    wifiConfigFile.close();
    Serial.println("Read wifi-config successfully.");
    u8g2log.println("Read wifi-config successfully");
    u8g2log.println("/wifi-config.json");
  }
  else
  {
    Serial.println("Read wifi-config failed. Use default.");
    u8g2log.println("Read failed.Use default.");
    u8g2log.println("/wifi-config.json");
  }
  WiFi.mode(wifi_mode);
  if (wifi_mode > 1)
  {
    WiFi.softAP(APssid, APpassword);
  }
  if (wifi_mode != 2)
  {
    WiFi.begin(STAssid, STApassword);
  }
  DBG_OUTPUT_PORT.println(String("Wifi set done!Mode:") + String(wifi_mode) + String("\nAP_SSID:") + String(APssid) + String("\nAP_Password") + String(APpassword) + String("\nSTA_SSID:") + String(STAssid));
  u8g2log.println(String("Wifi set done!Mode:") + String(wifi_mode) + String("\nAP_SSID:") + String(APssid) + String("\nAP_Password") + String(APpassword) + String("\nSTA_SSID:") + String(STAssid));
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
  char checked[][8] = {"", "", ""};
  strcpy(checked[wifi_mode - 1], "checked");
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
<h2>Player Mode Select</h2><label><input type=\"radio\" name=\"Playermode\" value=\"1\" %s>Local</label></br>\
<label><input type=\"radio\" name=\"Playermode\" value=\"2\" %s>Client</label></br>\
<label><input type=\"radio\" name=\"Playermode\" value=\"3\" %s>Server</label></br>\
<h3>Client-mode Configuration</h3>Server address:<input type=\"text\" name=\"Player_Client_Server\"></br>\
Port: <input type=\"text\" name=\"Player_Client_Port\"></br>\
<input type=\"submit\" value=\"Submit\">\
</form></body></html>\
",
           checked[0], checked[1], checked[2], STAssid, STApassword, APssid, APpassword);
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
        wifi_mode = WIFI_AP; //default work in AP mode
        strcpy(STAssid, postargs["STASSID"].as<String>().c_str());
        strcpy(STApassword, postargs["STApassword"].as<String>().c_str());
        strcpy(APssid, postargs["APSSID"].as<String>().c_str());
        strcpy(APpassword, postargs["APpassword"].as<String>().c_str());
        DynamicJsonBuffer WIFIconfbuf;
        JsonObject &wificonfjson = WIFIconfbuf.createObject();
        wificonfjson.set("mode", postargs["WIFImode"].as<String>());
        wificonfjson.set("STAssid", STAssid);
        wificonfjson.set("STApassword", STApassword);
        wificonfjson.set("APssid", APssid);
        wificonfjson.set("APpassword", APpassword);
        String WIFIjsonfile;
        wificonfjson.printTo(WIFIjsonfile);
        File wificonffilep = fileSystem->open("/wifi-config.json", "w");
        wificonffilep.write(WIFIjsonfile.c_str());
        wificonffilep.close();
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
  initWifi(); /*
  while (WiFi.status() != WL_CONNECTED)
  {
    delay(500);
    Serial.print(".");
  }*/
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

void loop()
{
  server.handleClient();
  MDNS.update();
}