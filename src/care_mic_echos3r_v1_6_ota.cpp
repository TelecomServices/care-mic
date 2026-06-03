#include <M5Unified.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <WebServer.h>
#include <Preferences.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <HTTPUpdate.h>

#define DEVICE_ID "mic-01"
#define CURRENT_VERSION "1.6.0"

Preferences prefs;
WebServer server(80);
WiFiClient espClient;
PubSubClient mqtt(espClient);

// WiFi
String wifiSsid = "www.telecomservices.nl_2.4Ghz";
String wifiPass = "TS454600";

// MQTT
String mqttHost = "10.2.2.64";
int mqttPort = 1883;
String mqttUser = "TSO";
String mqttPass = "TSO@4546a.nl";
String topicPrefix = "devices/mic-01";

// OTA update
String updateChannel = "stable";
String updateBaseUrl = "https://raw.githubusercontent.com/JOUW_GITHUB_GEBRUIKER/care-mic-firmware/main/firmware";
String lastUpdateVersion = "";
String lastUpdateBinUrl = "";
String lastUpdateNotes = "";
String updateStatus = "Nog niet gecontroleerd";

// Luisteren / SIP
String sipExtension = "8201";
String listenUrl = "";

// Audio
static constexpr size_t REC_SAMPLES = 256;
int16_t samples[REC_SAMPLES];

float levelRaw = 0;
float levelSmooth = 0;
float baseline = 0;
float deviation = 0;

float activeMargin = 300;
uint32_t activeHoldMs = 5000;

float dayMargin = 500;
float dayHoldSec = 5;

float nightMargin = 250;
float nightHoldSec = 3;

int dayStartHour = 7;
int nightStartHour = 22;

bool triggered = false;
uint32_t aboveSince = 0;

bool calibrating = false;
uint32_t calibStart = 0;
float calibMin = 999999;

unsigned long lastPublish = 0;
unsigned long lastProfileCheck = 0;

// ================= SETTINGS =================

void loadSettings() {
  prefs.begin("caremic", true);

  mqttHost = prefs.getString("mqttHost", mqttHost);
  mqttPort = prefs.getInt("mqttPort", mqttPort);
  mqttUser = prefs.getString("mqttUser", mqttUser);
  mqttPass = prefs.getString("mqttPass", mqttPass);
  topicPrefix = prefs.getString("topic", topicPrefix);
  updateChannel = prefs.getString("updChan", updateChannel);
  updateBaseUrl = prefs.getString("updBase", updateBaseUrl);

  sipExtension = prefs.getString("sipExt", sipExtension);

  dayMargin = prefs.getFloat("dayMargin", dayMargin);
  dayHoldSec = prefs.getFloat("dayHoldSec", dayHoldSec);

  nightMargin = prefs.getFloat("nightMargin", nightMargin);
  nightHoldSec = prefs.getFloat("nightHoldSec", nightHoldSec);

  dayStartHour = prefs.getInt("dayStart", dayStartHour);
  nightStartHour = prefs.getInt("nightStart", nightStartHour);

  prefs.end();
}

void saveSettings() {
  prefs.begin("caremic", false);

  prefs.putString("mqttHost", mqttHost);
  prefs.putInt("mqttPort", mqttPort);
  prefs.putString("mqttUser", mqttUser);
  prefs.putString("mqttPass", mqttPass);
  prefs.putString("topic", topicPrefix);
  prefs.putString("updChan", updateChannel);
  prefs.putString("updBase", updateBaseUrl);

  prefs.putString("sipExt", sipExtension);

  prefs.putFloat("dayMargin", dayMargin);
  prefs.putFloat("dayHoldSec", dayHoldSec);

  prefs.putFloat("nightMargin", nightMargin);
  prefs.putFloat("nightHoldSec", nightHoldSec);

  prefs.putInt("dayStart", dayStartHour);
  prefs.putInt("nightStart", nightStartHour);

  prefs.end();
}

// ================= WIFI =================

void connectWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(wifiSsid.c_str(), wifiPass.c_str());

  Serial.print("WiFi verbinden");

  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }

  Serial.println();
  Serial.print("WiFi OK: ");
  Serial.println(WiFi.localIP());

  listenUrl = "http://" + WiFi.localIP().toString() + "/live";
}

// ================= MQTT =================

String topic(const char* sub) {
  return topicPrefix + "/" + sub;
}

void connectMQTT() {
  mqtt.setServer(mqttHost.c_str(), mqttPort);

  while (!mqtt.connected()) {
    Serial.print("MQTT verbinden... ");

    if (mqtt.connect(DEVICE_ID, mqttUser.c_str(), mqttPass.c_str())) {
      Serial.println("OK");
      mqtt.publish(topic("status").c_str(), "online", true);
    } else {
      Serial.print("fout ");
      Serial.println(mqtt.state());
      delay(2000);
    }
  }
}

// ================= PROFILE =================

String activeProfileName() {
  int h = 12;

  struct tm timeinfo;
  if (getLocalTime(&timeinfo, 10)) {
    h = timeinfo.tm_hour;
  }

  if (dayStartHour < nightStartHour) {
    if (h >= dayStartHour && h < nightStartHour) return "day";
    return "night";
  }

  if (h >= dayStartHour || h < nightStartHour) return "day";
  return "night";
}

void updateProfile() {
  String p = activeProfileName();

  if (p == "day") {
    activeMargin = dayMargin;
    activeHoldMs = (uint32_t)(dayHoldSec * 1000.0f);
  } else {
    activeMargin = nightMargin;
    activeHoldMs = (uint32_t)(nightHoldSec * 1000.0f);
  }
}

// ================= AUDIO =================

float readMicLevel() {
  if (!M5.Mic.record(samples, REC_SAMPLES, 16000)) {
    return levelSmooth;
  }

  uint32_t sum = 0;

  for (size_t i = 0; i < REC_SAMPLES; i++) {
    int16_t v = samples[i];
    if (v < 0) v = -v;
    sum += v;
  }

  levelRaw = sum / (float)REC_SAMPLES;
  levelSmooth = levelSmooth * 0.85f + levelRaw * 0.15f;

  return levelSmooth;
}

// ================= JSON =================

String makeJson() {
  String json = "{";
  json += "\"device\":\"" DEVICE_ID "\",";
  json += "\"level\":" + String(levelSmooth, 1) + ",";
  json += "\"baseline\":" + String(baseline, 1) + ",";
  json += "\"deviation\":" + String(deviation, 1) + ",";
  json += "\"threshold\":" + String(baseline + activeMargin, 1) + ",";
  json += "\"margin\":" + String(activeMargin, 1) + ",";
  json += "\"hold_sec\":" + String(activeHoldMs / 1000.0f, 1) + ",";
  json += "\"triggered\":";
  json += (triggered ? "true" : "false");
  json += ",";
  json += "\"profile\":\"" + activeProfileName() + "\",";
  json += "\"wifi\":" + String(WiFi.RSSI()) + ",";
  json += "\"mqtt\":";
  json += (mqtt.connected() ? "true" : "false");
  json += ",";
  json += "\"calibrating\":";
  json += (calibrating ? "true" : "false");
  json += ",";
  json += "\"listen_url\":\"" + listenUrl + "\",";
  json += "\"sip_extension\":\"" + sipExtension + "\",";
  json += "\"dayMargin\":" + String(dayMargin, 1) + ",";
  json += "\"dayHoldSec\":" + String(dayHoldSec, 1) + ",";
  json += "\"nightMargin\":" + String(nightMargin, 1) + ",";
  json += "\"nightHoldSec\":" + String(nightHoldSec, 1) + ",";
  json += "\"dayStart\":" + String(dayStartHour) + ",";
  json += "\"nightStart\":" + String(nightStartHour) + ",";
  json += "\"mqttHost\":\"" + mqttHost + "\",";
  json += "\"mqttPort\":" + String(mqttPort) + ",";
  json += "\"mqttUser\":\"" + mqttUser + "\",";
  json += "\"topicPrefix\":\"" + topicPrefix + "\",";
  json += "\"version\":\"" CURRENT_VERSION "\",";
  json += "\"updateChannel\":\"" + updateChannel + "\",";
  json += "\"updateBaseUrl\":\"" + updateBaseUrl + "\",";
  json += "\"lastUpdateVersion\":\"" + lastUpdateVersion + "\",";
  json += "\"updateStatus\":\"" + updateStatus + "\"";
  json += "}";

  return json;
}

void publishStatus() {
  String json = makeJson();
  mqtt.publish(topic("state").c_str(), json.c_str(), true);
  Serial.println(json);
}

void publishTriggerEvent() {
  String json = "{";
  json += "\"device\":\"" DEVICE_ID "\",";
  json += "\"event\":\"sound_trigger\",";
  json += "\"level\":" + String(levelSmooth, 1) + ",";
  json += "\"baseline\":" + String(baseline, 1) + ",";
  json += "\"threshold\":" + String(baseline + activeMargin, 1) + ",";
  json += "\"margin\":" + String(activeMargin, 1) + ",";
  json += "\"duration_sec\":" + String(activeHoldMs / 1000.0f, 1) + ",";
  json += "\"profile\":\"" + activeProfileName() + "\",";
  json += "\"listen_url\":\"" + listenUrl + "\",";
  json += "\"sip_extension\":\"" + sipExtension + "\",";
  json += "\"triggered\":true";
  json += "}";

  mqtt.publish(topic("trigger").c_str(), json.c_str(), false);

  Serial.println("MQTT TRIGGER:");
  Serial.println(json);
}

void publishTestTrigger() {
  String json = "{";
  json += "\"device\":\"" DEVICE_ID "\",";
  json += "\"event\":\"test_trigger\",";
  json += "\"listen_url\":\"" + listenUrl + "\",";
  json += "\"sip_extension\":\"" + sipExtension + "\"";
  json += "}";

  mqtt.publish(topic("trigger").c_str(), json.c_str(), false);
}


// ================= OTA UPDATE =================

String extractJsonValue(String json, String key) {
  String pattern = "\"" + key + "\"";
  int p = json.indexOf(pattern);
  if (p < 0) return "";

  p = json.indexOf(":", p);
  if (p < 0) return "";

  p++;

  while (p < (int)json.length() && (json[p] == ' ' || json[p] == '\t' || json[p] == '\r' || json[p] == '\n')) {
    p++;
  }

  if (p >= (int)json.length()) return "";

  if (json[p] == '"') {
    p++;
    int e = json.indexOf("\"", p);
    if (e < 0) return "";
    return json.substring(p, e);
  }

  int e = p;
  while (e < (int)json.length() && json[e] != ',' && json[e] != '}') {
    e++;
  }

  String value = json.substring(p, e);
  value.trim();
  return value;
}

String updateManifestUrl() {
  String base = updateBaseUrl;
  if (base.endsWith("/")) {
    base.remove(base.length() - 1);
  }
  return base + "/" + updateChannel + ".json";
}

bool checkForUpdate() {
  if (WiFi.status() != WL_CONNECTED) {
    updateStatus = "Geen WiFi";
    return false;
  }

  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;
  String url = updateManifestUrl();

  Serial.print("Update check: ");
  Serial.println(url);

  if (!http.begin(client, url)) {
    updateStatus = "Update URL niet bereikbaar";
    return false;
  }

  int code = http.GET();

  if (code != 200) {
    updateStatus = "HTTP fout " + String(code);
    http.end();
    return false;
  }

  String body = http.getString();
  http.end();

  lastUpdateVersion = extractJsonValue(body, "version");
  lastUpdateBinUrl = extractJsonValue(body, "bin_url");
  lastUpdateNotes = extractJsonValue(body, "notes");

  if (lastUpdateVersion.length() == 0 || lastUpdateBinUrl.length() == 0) {
    updateStatus = "Manifest ongeldig";
    return false;
  }

  if (lastUpdateVersion == CURRENT_VERSION) {
    updateStatus = "Geen update beschikbaar";
    return false;
  }

  updateStatus = "Update beschikbaar: " + lastUpdateVersion;
  return true;
}

void installUpdate() {
  if (lastUpdateBinUrl.length() == 0) {
    bool available = checkForUpdate();
    if (!available) {
      return;
    }
  }

  updateStatus = "Update wordt geinstalleerd";
  Serial.print("OTA download: ");
  Serial.println(lastUpdateBinUrl);

  WiFiClientSecure client;
  client.setInsecure();

  t_httpUpdate_return result = httpUpdate.update(client, lastUpdateBinUrl);

  switch (result) {
    case HTTP_UPDATE_FAILED:
      updateStatus = "Update mislukt: " + String(httpUpdate.getLastErrorString());
      Serial.println(updateStatus);
      break;

    case HTTP_UPDATE_NO_UPDATES:
      updateStatus = "Geen update";
      Serial.println(updateStatus);
      break;

    case HTTP_UPDATE_OK:
      updateStatus = "Update OK, herstart";
      Serial.println(updateStatus);
      break;
  }
}


// ================= WEB =================

void handleData() {
  server.send(200, "application/json", makeJson());
}

void handleSetDetect() {
  if (server.hasArg("dayMargin")) dayMargin = server.arg("dayMargin").toFloat();
  if (server.hasArg("dayHoldSec")) dayHoldSec = server.arg("dayHoldSec").toFloat();

  if (server.hasArg("nightMargin")) nightMargin = server.arg("nightMargin").toFloat();
  if (server.hasArg("nightHoldSec")) nightHoldSec = server.arg("nightHoldSec").toFloat();

  if (server.hasArg("dayStart")) dayStartHour = server.arg("dayStart").toInt();
  if (server.hasArg("nightStart")) nightStartHour = server.arg("nightStart").toInt();

  if (dayHoldSec < 1) dayHoldSec = 1;
  if (nightHoldSec < 1) nightHoldSec = 1;

  saveSettings();
  updateProfile();

  server.send(200, "text/plain", "OK");
}

void handleSetMqtt() {
  if (server.hasArg("host")) mqttHost = server.arg("host");
  if (server.hasArg("port")) mqttPort = server.arg("port").toInt();
  if (server.hasArg("user")) mqttUser = server.arg("user");

  if (server.hasArg("pass")) {
    String p = server.arg("pass");
    if (p.length() > 0) mqttPass = p;
  }

  if (server.hasArg("topic")) topicPrefix = server.arg("topic");
  if (server.hasArg("sip")) sipExtension = server.arg("sip");

  saveSettings();

  mqtt.disconnect();
  connectMQTT();

  server.send(200, "text/plain", "OK");
}

void handleRecalibrate() {
  calibrating = true;
  calibStart = millis();
  calibMin = 999999;
  aboveSince = 0;
  triggered = false;

  server.send(200, "text/plain", "Calibrating");
}

void handleTestTrigger() {
  publishTestTrigger();
  server.send(200, "text/plain", "Test trigger sent");
}

void handleSetUpdate() {
  if (server.hasArg("channel")) updateChannel = server.arg("channel");
  if (server.hasArg("base")) updateBaseUrl = server.arg("base");

  if (updateChannel != "stable" && updateChannel != "beta") {
    updateChannel = "stable";
  }

  saveSettings();
  updateStatus = "Update instellingen opgeslagen";
  server.send(200, "text/plain", "OK");
}

void handleCheckUpdate() {
  checkForUpdate();
  server.send(200, "application/json", makeJson());
}

void handleDoUpdate() {
  server.send(200, "text/plain", "Update gestart. Apparaat herstart vanzelf als de update lukt.");
  delay(500);
  installUpdate();
}

void writeWavHeader(WiFiClient &client, uint32_t sampleRate) {
  uint32_t dataSize = 0xFFFFFFFF;
  uint32_t fileSize = dataSize + 36;
  uint16_t audioFormat = 1;
  uint16_t numChannels = 1;
  uint16_t bitsPerSample = 16;
  uint32_t byteRate = sampleRate * numChannels * bitsPerSample / 8;
  uint16_t blockAlign = numChannels * bitsPerSample / 8;

  client.write((const uint8_t*)"RIFF", 4);
  client.write((uint8_t*)&fileSize, 4);
  client.write((const uint8_t*)"WAVE", 4);
  client.write((const uint8_t*)"fmt ", 4);

  uint32_t subChunk1Size = 16;
  client.write((uint8_t*)&subChunk1Size, 4);
  client.write((uint8_t*)&audioFormat, 2);
  client.write((uint8_t*)&numChannels, 2);
  client.write((uint8_t*)&sampleRate, 4);
  client.write((uint8_t*)&byteRate, 4);
  client.write((uint8_t*)&blockAlign, 2);
  client.write((uint8_t*)&bitsPerSample, 2);

  client.write((const uint8_t*)"data", 4);
  client.write((uint8_t*)&dataSize, 4);
}

void handleLive() {
  String html = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
<meta name="viewport" content="width=device-width, initial-scale=1">
<style>
body{font-family:Arial;background:#f3f4f6;padding:20px}
.card{background:white;padding:20px;border-radius:12px;box-shadow:0 2px 8px rgba(0,0,0,.1)}
audio{width:100%;margin-top:20px}
</style>
</head>
<body>
<div class="card">
<h2>Live meeluisteren</h2>
<p>Druk op play om live audio te horen.</p>
<audio controls src="/live.wav"></audio>
</div>
</body>
</html>
)rawliteral";

  server.send(200, "text/html", html);
}

void handleLiveWav() {
  WiFiClient client = server.client();

  client.println("HTTP/1.1 200 OK");
  client.println("Content-Type: audio/wav");
  client.println("Connection: close");
  client.println();

  writeWavHeader(client, 16000);

  Serial.println("Live audio gestart");

  while (client.connected()) {
    if (M5.Mic.record(samples, REC_SAMPLES, 16000)) {
      client.write((uint8_t*)samples, REC_SAMPLES * sizeof(int16_t));
    }
    delay(1);
  }

  Serial.println("Live audio gestopt");
}

// ================= WEB PAGE =================

void handleRoot() {
  String html = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
<meta name="viewport" content="width=device-width, initial-scale=1">

<style>
body{font-family:Arial;background:#f3f4f6;padding:20px}
.card{background:white;padding:20px;border-radius:12px;margin-bottom:20px;box-shadow:0 2px 8px rgba(0,0,0,.1)}
.big{font-size:48px;font-weight:bold}
.ok{color:green;font-weight:bold}
.alarm{color:red;font-weight:bold}
input{width:100%;padding:10px;margin-top:5px;margin-bottom:10px;font-size:16px}
button{width:100%;padding:12px;border:none;border-radius:8px;background:#2563eb;color:white;font-size:16px;margin-top:8px}
canvas{width:100%;height:180px;background:#fafafa;border-radius:10px}
.small{color:#555;font-size:14px}
</style>
</head>

<body>

<div class="card">
  <h2>Care Geluidsmonitor</h2>
  <div>Atom EchoS3R</div>
</div>

<div class="card">
  <div class="big" id="lvl">0</div>
  <div id="status" class="ok">Normaal</div>
  <br>
  Profiel: <b id="profile">-</b><br>
  WiFi: <span id="wifi">0</span> dBm<br>
  MQTT: <span id="mqtt">-</span><br>
  Luister URL: <span id="listen">-</span><br>
  SIP toestel: <span id="sipShow">-</span>
</div>

<div class="card">
  Baseline: <span id="base">0</span><br><br>
  Alarmgrens: <span id="alarm">0</span><br><br>
  Afwijking: <span id="dev">0</span>
</div>

<div class="card">
  <canvas id="chart"></canvas>
  <div class="small">
    Blauw = geluid<br>
    Grijs = normaal<br>
    Rood = alarmgrens
  </div>
</div>

<div class="card">
  <h3>Detectie profielen</h3>

  <h4>Dag</h4>
  Start uur
  <input id="dayStart" type="number" min="0" max="23">

  Geluidsniveau marge
  <input id="dayMargin" type="number">

  Triggerduur seconden
  <input id="dayHoldSec" type="number" min="1" step="1">

  <h4>Nacht</h4>
  Start uur
  <input id="nightStart" type="number" min="0" max="23">

  Geluidsniveau marge
  <input id="nightMargin" type="number">

  Triggerduur seconden
  <input id="nightHoldSec" type="number" min="1" step="1">

  <button onclick="saveDetect()">Detectie opslaan</button>
  <button onclick="recal()">Herkalibreren</button>
  <button onclick="testTrigger()">Test trigger MQTT</button>
</div>

<div class="card">
  <h3>MQTT / SIP instellingen</h3>

  Broker
  <input id="mqttHost" type="text">

  Poort
  <input id="mqttPort" type="number">

  Gebruiker
  <input id="mqttUser" type="text">

  Wachtwoord
  <input id="mqttPass" type="password" placeholder="leeg laten = niet wijzigen">

  Topic prefix
  <input id="topicPrefix" type="text">

  SIP toestelnummer
  <input id="sipExtension" type="text">

  <button onclick="saveMqtt()">MQTT / SIP opslaan</button>
</div>

<div class="card">
  <h3>Firmware update</h3>

  Huidige versie: <b id="version">-</b><br>
  Status: <span id="updateStatus">-</span><br><br>

  Update kanaal
  <select id="updateChannel" style="width:100%;padding:10px;margin-top:5px;margin-bottom:10px;font-size:16px">
    <option value="stable">stable</option>
    <option value="beta">beta</option>
  </select>

  Update basis URL
  <input id="updateBaseUrl" type="text">

  <button onclick="saveUpdate()">Update instellingen opslaan</button>
  <button onclick="checkUpdate()">Controleer update</button>
  <button onclick="doUpdate()">Update installeren</button>
</div>

<script>
let levels=[];
let bases=[];
let alarms=[];

let canvas=document.getElementById('chart');
let ctx=canvas.getContext('2d');

function resizeCanvas(){
  canvas.width=canvas.offsetWidth;
  canvas.height=180;
}

resizeCanvas();
window.addEventListener('resize',resizeCanvas);

function setIfNotFocused(id,value){
  let e=document.getElementById(id);
  if(document.activeElement!==e){
    e.value=value;
  }
}

function drawChart(){
  ctx.clearRect(0,0,canvas.width,canvas.height);
  if(levels.length<2)return;

  let maxVal=Math.max(...levels,...bases,...alarms)*1.2;
  if(maxVal<100)maxVal=100;

  function y(v){return canvas.height-((v/maxVal)*canvas.height);}

  function line(arr,color,width){
    ctx.beginPath();
    ctx.strokeStyle=color;
    ctx.lineWidth=width;
    for(let i=0;i<arr.length;i++){
      let x=i*(canvas.width/(arr.length-1));
      let yy=y(arr[i]);
      if(i===0)ctx.moveTo(x,yy);
      else ctx.lineTo(x,yy);
    }
    ctx.stroke();
  }

  line(levels,'#2563eb',2);
  line(bases,'#9ca3af',1);
  line(alarms,'#dc2626',2);
}

function update(){
  fetch('/data').then(r=>r.json()).then(d=>{
    document.getElementById('lvl').innerHTML=Math.round(d.level);
    document.getElementById('base').innerHTML=Math.round(d.baseline);
    document.getElementById('alarm').innerHTML=Math.round(d.threshold);
    document.getElementById('dev').innerHTML=Math.round(d.deviation);

    document.getElementById('profile').innerHTML=d.profile;
    document.getElementById('wifi').innerHTML=d.wifi;
    document.getElementById('mqtt').innerHTML=d.mqtt?'verbonden':'niet verbonden';
    document.getElementById('listen').innerHTML=d.listen_url;
    document.getElementById('sipShow').innerHTML=d.sip_extension;

    let s=document.getElementById('status');

    if(d.calibrating){
      s.innerHTML='Kalibreren...';
      s.className='alarm';
    }else if(d.triggered){
      s.innerHTML='ALARM';
      s.className='alarm';
    }else{
      s.innerHTML='Normaal';
      s.className='ok';
    }

    setIfNotFocused('dayMargin',d.dayMargin);
    setIfNotFocused('dayHoldSec',d.dayHoldSec);
    setIfNotFocused('nightMargin',d.nightMargin);
    setIfNotFocused('nightHoldSec',d.nightHoldSec);
    setIfNotFocused('dayStart',d.dayStart);
    setIfNotFocused('nightStart',d.nightStart);

    setIfNotFocused('mqttHost',d.mqttHost);
    setIfNotFocused('mqttPort',d.mqttPort);
    setIfNotFocused('mqttUser',d.mqttUser);
    setIfNotFocused('topicPrefix',d.topicPrefix);
    setIfNotFocused('sipExtension',d.sip_extension);

    document.getElementById('version').innerHTML=d.version;
    document.getElementById('updateStatus').innerHTML=d.updateStatus;
    setIfNotFocused('updateBaseUrl',d.updateBaseUrl);
    if(document.activeElement!==document.getElementById('updateChannel')){
      document.getElementById('updateChannel').value=d.updateChannel;
    }

    levels.push(d.level);
    bases.push(d.baseline);
    alarms.push(d.threshold);

    if(levels.length>80){
      levels.shift();
      bases.shift();
      alarms.shift();
    }

    drawChart();
  });
}

function saveDetect(){
  fetch('/setdetect?dayMargin='+document.getElementById('dayMargin').value+
  '&dayHoldSec='+document.getElementById('dayHoldSec').value+
  '&nightMargin='+document.getElementById('nightMargin').value+
  '&nightHoldSec='+document.getElementById('nightHoldSec').value+
  '&dayStart='+document.getElementById('dayStart').value+
  '&nightStart='+document.getElementById('nightStart').value);
}

function saveMqtt(){
  fetch('/setmqtt?host='+encodeURIComponent(document.getElementById('mqttHost').value)+
  '&port='+document.getElementById('mqttPort').value+
  '&user='+encodeURIComponent(document.getElementById('mqttUser').value)+
  '&pass='+encodeURIComponent(document.getElementById('mqttPass').value)+
  '&topic='+encodeURIComponent(document.getElementById('topicPrefix').value)+
  '&sip='+encodeURIComponent(document.getElementById('sipExtension').value));
}

function saveUpdate(){
  fetch('/setupdate?channel='+encodeURIComponent(document.getElementById('updateChannel').value)+
  '&base='+encodeURIComponent(document.getElementById('updateBaseUrl').value));
}

function checkUpdate(){
  fetch('/checkupdate').then(r=>r.json()).then(d=>{
    document.getElementById('updateStatus').innerHTML=d.updateStatus;
  });
}

function doUpdate(){
  if(confirm('Update installeren? Het apparaat zal herstarten.')){
    fetch('/doupdate');
  }
}

function recal(){
  fetch('/recalibrate');
}

function testTrigger(){
  fetch('/testtrigger');
}

setInterval(update,500);
update();
</script>

</body>
</html>
)rawliteral";

  server.send(200, "text/html", html);
}

// ================= SETUP =================

void setup() {
  auto cfg = M5.config();
  M5.begin(cfg);

  Serial.begin(115200);
  delay(1000);

  Serial.println("Care Mic Atom EchoS3R gestart");

  loadSettings();
  updateProfile();

  M5.Mic.begin();

  connectWiFi();

  configTime(3600, 3600, "pool.ntp.org", "time.nist.gov");

  mqtt.setServer(mqttHost.c_str(), mqttPort);
  connectMQTT();

  server.on("/", handleRoot);
  server.on("/data", handleData);
  server.on("/setdetect", handleSetDetect);
  server.on("/setmqtt", handleSetMqtt);
  server.on("/recalibrate", handleRecalibrate);
  server.on("/testtrigger", handleTestTrigger);
  server.on("/setupdate", handleSetUpdate);
  server.on("/checkupdate", handleCheckUpdate);
  server.on("/doupdate", handleDoUpdate);
  server.on("/live", handleLive);
  server.on("/live.wav", handleLiveWav);
  server.begin();

  Serial.println("Webserver gestart");
}

// ================= LOOP =================

void loop() {
  M5.update();
  server.handleClient();

  if (WiFi.status() != WL_CONNECTED) {
    connectWiFi();
  }

  if (!mqtt.connected()) {
    connectMQTT();
  }

  mqtt.loop();

  if (millis() - lastProfileCheck > 10000) {
    lastProfileCheck = millis();
    updateProfile();
  }

  float level = readMicLevel();

  if (calibrating) {
    if (level > 5 && level < calibMin) {
      calibMin = level;
    }

    if (millis() - calibStart > 10000) {
      baseline = calibMin;
      calibrating = false;
      Serial.print("Kalibratie klaar. Baseline: ");
      Serial.println(baseline);
    }

    delay(100);
    return;
  }

  if (baseline == 0) {
    baseline = level;
  }

  deviation = level - baseline;
  float threshold = baseline + activeMargin;

  bool above = level > threshold;

  if (!triggered && !above) {
    baseline = baseline * 0.995f + level * 0.005f;
  }

  if (above) {
    if (aboveSince == 0) {
      aboveSince = millis();
    }

    if (!triggered && millis() - aboveSince >= activeHoldMs) {
      triggered = true;
      Serial.println("TRIGGER AAN");
      publishTriggerEvent();
    }
  } else {
    aboveSince = 0;

    if (level < baseline + activeMargin * 0.5f) {
      if (triggered) {
        Serial.println("TRIGGER UIT");
      }
      triggered = false;
    }
  }

  if (millis() - lastPublish > 1000) {
    lastPublish = millis();
    publishStatus();
  }

  delay(100);
}