
#define DBG_OUTPUT_PORT Serial



// Uncomment the following line to embed a version of the web page in the code
// (program code will be larger, but no file will have to be written to the filesystem).
// Note: the source file "extras/index_htm.h" must have been generated by "extras/reduce_index.sh"

//#define INCLUDE_FALLBACK_INDEX_HTM

////////////////////////////////
#include <WiFi.h>
#include <WiFiClient.h>
#include <WebServer.h>
#include <LEAmDNS.h>
#include <SPI.h>

#ifdef INCLUDE_FALLBACK_INDEX_HTM
#include "../extras/index_htm.h"
#endif



#define DBG_OUTPUT_PORT Serial

#ifndef STASSID
#define STASSID "your-ssid"
#define STAPSK "your-password"
#endif

const char* ssid = STASSID;
const char* password = STAPSK;
const char* host = "RP2040 hotas";

WebServer server(80);
ServerSessions serverCache(5);

String unsupportedFiles = String();

File uploadFile;
File firmware;
int check_firmware_update = millis();


static const char TEXT_PLAIN[] PROGMEM = "text/plain";
static const char FS_INIT_ERROR[] PROGMEM = "FS INIT ERROR";
static const char FILE_NOT_FOUND[] PROGMEM = "FileNotFound";

////////////////////////////////
// Utils to return HTTP codes, and determine content-type

void replyOK() {
  server.send(200, FPSTR(TEXT_PLAIN), "");
}

void replyOKWithMsg(String msg) {
  server.send(200, FPSTR(TEXT_PLAIN), msg);
}

void replyNotFound(String msg) {
  server.send(404, FPSTR(TEXT_PLAIN), msg);
}

void replyServerForbidden(String msg) {
  DBG_OUTPUT_PORT.println(msg);
  server.send(403, FPSTR(TEXT_PLAIN), msg + "\r\n");
}

void replyBadRequest(String msg) {
  DBG_OUTPUT_PORT.println(msg);
  server.send(400, FPSTR(TEXT_PLAIN), msg + "\r\n");
}

void replyServerError(String msg) {
  DBG_OUTPUT_PORT.println(msg);
  server.send(500, FPSTR(TEXT_PLAIN), msg + "\r\n");
}


#ifdef USE_SPIFFS
/*
   Checks filename for character combinations that are not supported by FSBrowser (alhtough valid on SPIFFS).
   Returns an empty String if supported, or detail of error(s) if unsupported
*/
String checkForUnsupportedPath(String filename) {
  String error = String();
  if (!filename.startsWith("/")) {
    error += F("!NO_LEADING_SLASH! ");
  }
  if (filename.indexOf("//") != -1) {
    error += F("!DOUBLE_SLASH! ");
  }
  if (filename.endsWith("/")) {
    error += F("!TRAILING_SLASH! ");
  }
  return error;
}
#endif


////////////////////////////////
// Request handlers

/*
   Return the FS type, status and size info
*/


void handleStatus() {
  DBG_OUTPUT_PORT.println("handleStatus");
  FSInfo fs_info;
  String json;
  json.reserve(128);

  json = "{\"type\":\"";
  json += fsName;
  json += "\", \"isOk\":";
  if (fsOK) {
    fileSystem->info(fs_info);
    json += F("\"true\", \"totalBytes\":\"");
    json += fs_info.totalBytes;
    json += F("\", \"usedBytes\":\"");
    json += fs_info.usedBytes;
    json += "\"";
  } else {
    json += "\"false\"";
  }
  json += F(",\"unsupportedFiles\":\"");
  json += unsupportedFiles;
  json += "\"}";

  server.send(200, "application/json", json);
}


/*
   Return the list of files in the directory specified by the "dir" query string parameter.
   Also demonstrates the use of chunked responses.
*/
void handleFileList() {
  if (!fsOK) {
    return replyServerError(FPSTR(FS_INIT_ERROR));
  }

  if (!server.hasArg("dir")) {
    return replyBadRequest(F("DIR ARG MISSING"));
  }

  String path = server.arg("dir");
  if (path != "/" && !fileSystem->exists(path)) {
    return replyBadRequest("BAD PATH");
  }

  DBG_OUTPUT_PORT.println(String("handleFileList: ") + path);
  Dir dir = fileSystem->openDir(path);
  path = "";

  // use HTTP/1.1 Chunked response to avoid building a huge temporary string
  if (!server.chunkedResponseModeStart(200, "text/json")) {
    server.send(505, F("text/html"), F("HTTP1.1 required"));
    return;
  }

  // use the same string for every line
  String output;
  output.reserve(64);
  while (dir.next()) {
#ifdef USE_SPIFFS
    String error = checkForUnsupportedPath(dir.fileName());
    if (error.length() > 0) {
      DBG_OUTPUT_PORT.println(String("Ignoring ") + error + dir.fileName());
      continue;
    }
#endif
    if (output.length()) {
      // send string from previous iteration
      // as an HTTP chunk
      DBG_OUTPUT_PORT.println(output);
      server.sendContent(output);
      output = ',';
    } else {
      output = '[';
    }

    output += "{\"type\":\"";
    if (dir.isDirectory()) {
      output += "dir";
    } else {
      output += F("file\",\"size\":\"");
      output += dir.fileSize();
    }

    output += F("\",\"name\":\"");
    // Always return names without leading "/"
    if (dir.fileName()[0] == '/') {
      output += &(dir.fileName()[1]);
    } else {
      output += dir.fileName();
    }

    output += "\"}";
  }

  // send last string
  output += "]";
  server.sendContent(output);
  server.chunkedResponseFinalize();
}


/*
   Read the given file from the filesystem and stream it back to the client
*/
bool handleFileRead(String path) {
  DBG_OUTPUT_PORT.println(String("handleFileRead: ") + path);
  if (!fsOK) {
    replyServerError(FPSTR(FS_INIT_ERROR));
    return true;
  }

  if (path.endsWith("/")) {
    path += "index.html";
  }

  String contentType;
  if (server.hasArg("download")) {
    contentType = F("application/octet-stream");
  } else {
    contentType = mime::getContentType(path);
  }

  if (!fileSystem->exists(path)) {
    // File not found, try gzip version
    path = path + ".gz";
  }
  if (fileSystem->exists(path)) {
    File file = fileSystem->open(path, "r");
    if (server.streamFile(file, contentType) != file.size()) {
      DBG_OUTPUT_PORT.println("Sent less data than expected!");
    }
    file.close();
    return true;
  }

  return false;
}


/*
   As some FS (e.g. LittleFS) delete the parent folder when the last child has been removed,
   return the path of the closest parent still existing
*/
String lastExistingParent(String path) {
  while (path != "" && !fileSystem->exists(path)) {
    if (path.lastIndexOf('/') > 0) {
      path = path.substring(0, path.lastIndexOf('/'));
    } else {
      path = String();  // No slash => the top folder does not exist
    }
  }
  DBG_OUTPUT_PORT.println(String("Last existing parent: ") + path);
  return path;
}

/*
   Handle the creation/rename of a new file
   Operation      | req.responseText
   ---------------+--------------------------------------------------------------
   Create file    | parent of created file
   Create folder  | parent of created folder
   Rename file    | parent of source file
   Move file      | parent of source file, or remaining ancestor
   Rename folder  | parent of source folder
   Move folder    | parent of source folder, or remaining ancestor
*/
void handleFileCreate() {
  if (!fsOK) {
    return replyServerError(FPSTR(FS_INIT_ERROR));
  }

  String path = server.arg("path");
  if (path == "") {
    return replyBadRequest(F("PATH ARG MISSING"));
  }

#ifdef USE_SPIFFS
  if (checkForUnsupportedPath(path).length() > 0) {
    return replyServerError(F("INVALID FILENAME"));
  }
#endif

  if (path == "/") {
    return replyBadRequest("BAD PATH");
  }

  if (fileSystem->exists(path)) {
    return replyBadRequest(F("PATH FILE EXISTS"));
  }

  String src = server.arg("src");
  if (src == "") {
    // No source specified: creation
    DBG_OUTPUT_PORT.println(String("handleFileCreate: ") + path);
    if (path.endsWith("/")) {
      // Create a folder
      path.remove(path.length() - 1);
      if (!fileSystem->mkdir(path)) {
        return replyServerError(F("MKDIR FAILED"));
      }
    } else {
      // Create a file
      File file = fileSystem->open(path, "w");
      if (file) {
        file.write((const char*)0);
        file.close();
      } else {
        return replyServerError(F("CREATE FAILED"));
      }
    }
    if (path.lastIndexOf('/') > -1) {
      path = path.substring(0, path.lastIndexOf('/'));
    }
    replyOKWithMsg(path);
  } else {
    // Source specified: rename
    if (src == "/") {
      return replyBadRequest("BAD SRC");
    }
    if (!fileSystem->exists(src)) {
      return replyBadRequest(F("SRC FILE NOT FOUND"));
    }

    DBG_OUTPUT_PORT.println(String("handleFileCreate: ") + path + " from " + src);

    if (path.endsWith("/")) {
      path.remove(path.length() - 1);
    }
    if (src.endsWith("/")) {
      src.remove(src.length() - 1);
    }
    if (!fileSystem->rename(src, path)) {
      return replyServerError(F("RENAME FAILED"));
    }
    replyOKWithMsg(lastExistingParent(src));
  }
}


/*
   Delete the file or folder designed by the given path.
   If it's a file, delete it.
   If it's a folder, delete all nested contents first then the folder itself

   IMPORTANT NOTE: using recursion is generally not recommended on embedded devices and can lead to crashes (stack overflow errors).
   This use is just for demonstration purpose, and FSBrowser might crash in case of deeply nested filesystems.
   Please don't do this on a production system.
*/
void deleteRecursive(String path) {
  File file = fileSystem->open(path, "r");
  bool isDir = file.isDirectory();
  file.close();

  // If it's a plain file, delete it
  if (!isDir) {
    fileSystem->remove(path);
    return;
  }

  // Otherwise delete its contents first
  Dir dir = fileSystem->openDir(path);

  while (dir.next()) {
    deleteRecursive(path + '/' + dir.fileName());
  }

  // Then delete the folder itself
  fileSystem->rmdir(path);
}


/*
   Handle a file deletion request
   Operation      | req.responseText
   ---------------+--------------------------------------------------------------
   Delete file    | parent of deleted file, or remaining ancestor
   Delete folder  | parent of deleted folder, or remaining ancestor
*/
void handleFileDelete() {
  if (!fsOK) {
    return replyServerError(FPSTR(FS_INIT_ERROR));
  }

  String path = server.arg(0);
  if (path == "" || path == "/") {
    return replyBadRequest("BAD PATH");
  }

  DBG_OUTPUT_PORT.println(String("handleFileDelete: ") + path);
  if (!fileSystem->exists(path)) {
    return replyNotFound(FPSTR(FILE_NOT_FOUND));
  }
  deleteRecursive(path);

  replyOKWithMsg(lastExistingParent(path));
}

/*
   Handle a file upload request
*/
void handleFileUpload() {
  if (!fsOK) {
    return replyServerError(FPSTR(FS_INIT_ERROR));
  }
  if (server.uri() != "/edit") {
    return;
  }
  HTTPUpload& upload = server.upload();
  if (upload.status == UPLOAD_FILE_START) {
    String filename = upload.filename;
    // Make sure paths always start with "/"
    if (!filename.startsWith("/")) {
      filename = "/" + filename;
    }
    DBG_OUTPUT_PORT.println(String("handleFileUpload Name: ") + filename);
    uploadFile = fileSystem->open(filename, "w");
    if (!uploadFile) {
      return replyServerError(F("CREATE FAILED"));
    }
    DBG_OUTPUT_PORT.println(String("Upload: START, filename: ") + filename);
  } else if (upload.status == UPLOAD_FILE_WRITE) {
    if (uploadFile) {
      size_t bytesWritten = uploadFile.write(upload.buf, upload.currentSize);
      if (bytesWritten != upload.currentSize) {
        return replyServerError(F("WRITE FAILED"));
      }
    }
    DBG_OUTPUT_PORT.println(String("Upload: WRITE, Bytes: ") + upload.currentSize);
  } else if (upload.status == UPLOAD_FILE_END) {
    if (uploadFile) {
      uploadFile.close();
    }
    DBG_OUTPUT_PORT.println(String("Upload: END, Size: ") + upload.totalSize);
  }
}


/*
   The "Not Found" handler catches all URI not explicitly declared in code
   First try to find and return the requested file from the filesystem,
   and if it fails, return a 404 page with debug information
*/
void handleNotFound() {
  if (!fsOK) {
    return replyServerError(FPSTR(FS_INIT_ERROR));
  }

  String uri = WebServer::urlDecode(server.uri());  // required to read paths with blanks

  if (handleFileRead(uri)) {
    return;
  }

  // Dump debug data
  String message;
  message.reserve(100);
  message = F("Error: File not found\r\nURI: ");
  message += uri;
  message += F("\r\nMethod: ");
  message += (server.method() == HTTP_GET) ? "GET" : "POST";
  message += F("\r\nArguments: ");
  message += server.args();
  message += "\r\n";
  for (uint8_t i = 0; i < server.args(); i++) {
    message += F(" NAME:");
    message += server.argName(i);
    message += F("\r\n VALUE:");
    message += server.arg(i);
    message += "\r\n";
  }
  message += "path=";
  message += server.arg("path");
  message += "\r\n";
  DBG_OUTPUT_PORT.print(message);

  return replyNotFound(message);
}

/*
   This specific handler returns the index.html (or a gzipped version) from the /edit folder.
   If the file is not present but the flag INCLUDE_FALLBACK_INDEX_HTM has been set, falls back to the version
   embedded in the program code.
   Otherwise, fails with a 404 page with debug information
*/
void handleGetEdit() {
  if (handleFileRead(F("/edit/index.html"))) {
    return;
  }

#ifdef INCLUDE_FALLBACK_INDEX_HTM
  server.sendHeader(F("Content-Encoding"), "gzip");
  server.send(200, "text/html", index_htm_gz, index_htm_gz_len);
#else
  replyNotFound(FPSTR(FILE_NOT_FOUND));
#endif
}

void led_control() 
{
 String act_state = server.arg("state");
//extern bool LED_on;
  switch (act_state.toInt())
  {
    case 0: {  //off
      LED_on = false;
      LED_breathe = false;
      digitalWrite(LED_BUILTIN, false);
      break;
    }
    case 1: {  //on
      LED_on = true;
      LED_breathe = false;
      digitalWrite(LED_BUILTIN, true);
      break;
    }
    case 2: {  //on
      LED_on = true;
      LED_breathe = true;
      break;
    }
  default:
    break;
  }
  server.send(200, "text/plain");
}
void get_led() {
  String state;
  state = (digitalRead(LED_BUILTIN)) ? "ON" : "OFF";
  server.send(200, "text/plain", state);

}
void uptime() {
  auto _time = millis();
  String time = (String)(_time/(1000*60*60)%60)+":"+(String)(_time/(1000*60)%60)+":"+(String)((_time/1000)%60);
  server.send(200, "text/plain", time);
}

void drawGraph() {
  String out;
  out.reserve(2600);
  char temp[70];
  out += "<svg xmlns=\"http://www.w3.org/2000/svg\" version=\"1.1\" width=\"400\" height=\"150\">\n";
  out += "<rect width=\"400\" height=\"150\" fill=\"rgb(250, 230, 210)\" stroke-width=\"1\" stroke=\"rgb(0, 0, 0)\" />\n";
  out += "<g stroke=\"black\">\n";
  int y = rand() % 130;
  for (int x = 10; x < 390; x += 10) {
    int y2 = rand() % 130;
    sprintf(temp, "<line x1=\"%d\" y1=\"%d\" x2=\"%d\" y2=\"%d\" stroke-width=\"1\" />\n", x, 140 - y, x + 10, 140 - y2);
    out += temp;
    y = y2;
  }
  out += "</g>\n</svg>\n";

  server.send(200, "image/svg+xml", out);
}

void settings() {
    if (handleFileRead(F("/settings/index.html"))) {
    return;
  }
}

void i2cResult() {
  String jsonString;
  jsonString = "[";
  for (uint8_t i =0; i <sizeof(i2cIDs); i++) {
    if (i2cIDs[i] == 1)
    jsonString +=  (String)(i)+",";
  }
  jsonString = jsonString.substring(0, jsonString.lastIndexOf(","));
  jsonString += "]";
  

  server.send(200, "application/json", jsonString);
}

void updateHid() {
  if (server.hasArg("usagepage")) {
    String a = server.arg("usagepage");
    if (a.length()) {
      if (a.toInt()>0) {
        hid_usage_page_val = a.toInt();
      } else {
        hid_usage_page_val = HID_USAGE_PAGE_DESKTOP;
      }
    } else {
      server.send(200,"text/plain",(String)hid_usage_page_val);
    }
  }

  if (server.hasArg("usage")) {
    String a = server.arg("usage");
    if (a.length()) {
      if (a.toInt()>0) {
        hid_usage_val = a.toInt();
      } else {
        hid_usage_val = HID_USAGE_DESKTOP_JOYSTICK;
      }
    } else {
      server.send(200,"text/plain",(String)hid_usage_val);
    }
  }

  if (server.hasArg("axis")) {
    String a = server.arg("axis");
    if (a.length()) {
      if (a.toInt()>0) {
        AxisCount = a.toInt();
      } else {
        AxisCount = 0;
      }
    } else {
      server.send(200,"text/plain",(String)AxisCount);
    }
  }  

  if (server.hasArg("buttons")) {
    String a = server.arg("buttons");
    if (a.length()) {
      if (a.toInt()>0) {
        ButtonCount = a.toInt();
      } else {
        ButtonCount = 0;
      }
    } else {
      server.send(200,"text/plain",(String)ButtonCount);
    }
  }

  if (server.hasArg("hats")) {
    String a = server.arg("hats");
    if (a.length()) {
      if (a.toInt()>0) {
        HatCount = a.toInt();
      } else {
        HatCount = 0;
      }
    } else {
      server.send(200,"text/plain",(String)HatCount);
    }
  }

  if (server.hasArg("restart")) {
    setupUSB();
    server.send(200);
  }
}


// Check if header is present and correct
bool is_authenticated() {
  Serial.println("Enter is_authenticated");
  if (server.hasHeader("Cookie")) {
    Serial.print("Found cookie: ");
    String cookie = server.header("Cookie");
    Serial.println(cookie);
    if (cookie.indexOf("PICOSESSIONID=1") != -1) {
      Serial.println("Authentication Successful");
      return true;
    }
  }
  Serial.println("Authentication Failed");
  return false;
}

//void handleEdit();
// login page, also called for disconnect
void handleEditorLogin() {
  String msg;
  if (server.hasHeader("Cookie")) {
    Serial.print("Found cookie: ");
    String cookie = server.header("Cookie");
    Serial.println(cookie);
  }
  
   if (server.hasArg("DISCONNECT")) {
     Serial.println("Disconnection");
     server.sendHeader("Location", "/editor");
     server.sendHeader("Cache-Control", "no-cache");
     //server.sendHeader("Set-Cookie", "PICOSESSIONID=0");
     server.send(301);
     return;
   }
   if (server.hasArg("USERNAME") && server.hasArg("PASSWORD")) {
    if (server.arg("USERNAME") == "admin" && server.arg("PASSWORD") == "admin") {

      server.sendHeader("Location", "/editor");
      server.sendHeader("Cache-Control", "no-cache");
      //server.sendHeader("Set-Cookie", "PICOSESSIONID=1");
      //server.send(301);
      handleGetEdit();

      Serial.println("Log in Successful");
      return;
    }
    msg = "Wrong username/password! try again.";
    Serial.println("Log in Failed");
  }
  //String content = "<html><body><form action='/login' method='POST'>To log in, please use : admin/admin<br>";
  String content = "<html><body><form action='/editor' method='POST'>Login to use the editor<br>";
  content += "User:    <input type='text' name='USERNAME' placeholder='user name'><br>";
  content += "Password:<input type='password' name='PASSWORD' placeholder='password'><br>";
  content += "<input type='submit' name='SUBMIT' value='Submit'></form>" + msg + "<br>";
  content += "</body></html>";
  server.send(200, "text/html", content);
}

void setupWifi() {

  ////////////////////////////////
  // WI-FI INIT
  DBG_OUTPUT_PORT.printf("Connecting to %s\n", ssid);
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);
  // Wait for connection

  ////////////////////////////////
  // MDNS INIT
  if (MDNS.begin(host)) {
    MDNS.addService("http", "tcp", 80);
    //DBG_OUTPUT_PORT.print(F("Open http://"));
    //DBG_OUTPUT_PORT.print(host);
    //DBG_OUTPUT_PORT.println(F(".local/edit to open the FileSystem Browser"));
  }

  ////////////////////////////////
  // WEB SERVER INIT

  // Control builtin-led
  server.on("/led_set", led_control);

  server.on("/led_brightness", NULL);

  // Get builtin-led
  server.on("/led_get", get_led);

  // Get Uptime
  server.on("/uptime", uptime);

  server.on("/test.svg", drawGraph);

  // Get I2C slave list
  server.on("/listI2C", i2cResult);

  server.on("/settings",NULL);

  server.on("/settings/updatehid",updateHid);

  // Filesystem status
  server.on("/status", HTTP_GET, handleStatus);
  // List directory
  server.on("/list", HTTP_GET, handleFileList);

  // Load editor
  server.on("/editor", handleEditorLogin);
  //server.on("/edit", HTTP_GET, handleGetEdit);
  // Create file
  server.on("/edit", HTTP_PUT, handleFileCreate);
  // Delete file
  server.on("/edit", HTTP_DELETE, handleFileDelete);

  // Upload file
  // - first callback is called after the request has ended with all parsed arguments
  // - second callback handles file upload at that location
  server.on("/edit", HTTP_POST, replyOK, handleFileUpload);

  server.on("/edit/index.html", [](){server.send(403);});

  server.on("/",[](){if(handleFileRead(F("/index.html"))) {return;}});

  // Default handler for all URIs not defined above
  // Use it to read files from filesystem
  server.onNotFound(handleNotFound);

  server.collectHeaders("User-Agent", "Cookie");

  // Start server
  server.begin();

  DBG_OUTPUT_PORT.println("HTTP server started");
}
