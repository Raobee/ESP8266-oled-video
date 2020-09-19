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
      if (json.containsKey("APssid"))
      {
        strcpy(STApassword, json["STApassword"].as<char *>());
      }
    }
    jsonBuffer.clear(); //Clear json buffer
    wifiConfigFile.close();
    Serial.println("Read wifi-config successfully.");
    u8g2log.println("Read wifi-config successfully");
    u8g2log.println("File:/wifi-config.json");
  }
  else
  {
    Serial.println("Read wifi-config failed. Use default.");
    u8g2log.println("Read failed.Use default.");
    u8g2log.println("File:/wifi-config.json");
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
  Serial.print("Wifi set done! Mode:");
  Serial.println(wifi_mode);
  Serial.print("AP_SSID:");
  Serial.println(APssid);
  Serial.print("AP_Password:");
  Serial.println(APpassword);
  Serial.print("STA_SSID:");
  Serial.println(STAssid);
  u8g2log.print("Wifi set done! Mode:");
  u8g2log.println(wifi_mode);
  u8g2log.print("AP_SSID:");
  u8g2log.println(APssid);
  u8g2log.print("AP_Password:");
  u8g2log.println(APpassword);
}

void handleRoot()
{
  u8g2log.println((String)"HTTP Request on "+(String)server.uri());
  char temp[1024];
  char checked[][8]={"","",""};
  strcpy(checked[wifi_mode-1],"checked");
  snprintf(temp, 1024, "<!DOCTYPE html><html><head><meta charset=\"utf-8\"><title>ESP8266 Config Page</title></head><body>\
<form><h2>WiFi Mode Select</h2><input type=\"radio\" name=\"mode\" value=\"1\" %s>STA</br>\
<input type=\"radio\" name=\"mode\" value=\"2\" %s>AP</br>\
<input type=\"radio\" name=\"mode\" value=\"3\" %s>STA+AP</br>\
<h3>STA Mode Configuration</h3></br>SSID: <input type=\"text\" name=\"STASSID\" value=\"%s\"></br>\
Password: <input type=\"text\" name=\"STApassword\" value=\"%s\"></br>\
<h3>AP Mode Configuration</h3></br>SSID: <input type=\"text\" name=\"APSSID\" value=\"%s\"></br>\
Password: <input type=\"text\" name=\"APpassword\" value=\"%s\"></br>\
<h3>Server Configuration</h3>\
Server: <input type=\"text\" name=\"Server\"></br>Port: <input type=\"text\" name=\"Port\">\
</form></body></html>\
",
           checked[0],checked[1],checked[2],STAssid, STApassword,APssid, APpassword);
  server.send(200, "text/html", temp);
}

void handleNotFound()
{
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
  if (!fsOK)
  {
    return replyServerError(FPSTR(FS_INIT_ERROR));
  }
  if (server.uri() != "/edit")
  {
    return;
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
  fileSystemConfig.setAutoFormat(false);
  fileSystem->setConfig(fileSystemConfig);
  fsOK = fileSystem->begin();
  DBG_OUTPUT_PORT.println(fsOK ? F("Filesystem initialized.") : F("Filesystem init failed!"));

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
  // Upload file
  // - first callback is called after the request has ended with all parsed arguments
  // - second callback handles file upload at that location
  server.on("/edit", HTTP_POST, replyOK, handleFileUpload);
  server.onNotFound(handleNotFound);
  server.begin();
  Serial.println("HTTP server started");
}

void loop()
{
  server.handleClient();
  MDNS.update();
}