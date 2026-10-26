
#include <WiFi.h>
#include <WebServer.h>
#include <Wire.h>
#include <BH1750.h>
#include <DHT.h>
#include <math.h>

const char* AP_SSID = "SmartHeadlight";
const char* AP_PASSWORD = "headlight123";

WebServer server(80);

#define I2C_SDA 21
#define I2C_SCL 22
#define RAIN_PIN 35
#define DHT_PIN 4
#define MAIN_LEFT_PIN 25
#define MAIN_RIGHT_PIN 26
#define DHT_TYPE DHT11

DHT dht(DHT_PIN, DHT_TYPE);
BH1750 lightSensor;

const uint32_t PWM_FREQUENCY = 5000;
const uint8_t PWM_RESOLUTION = 8;

#define PWM_CHANNEL_LEFT 0
#define PWM_CHANNEL_RIGHT 1

const float DARK_LUX = 10.0;
const float BRIGHT_LUX = 500.0;
const float HIGH_HUMIDITY = 75.0;

const int RAIN_THRESHOLD = 1800;
const int MEDIUM_BRIGHTNESS = 128;

const unsigned long DIM_INTERVAL_MS = 20;
const int DIM_STEP = 2;
const unsigned long DHT_INTERVAL = 2500;

bool bh1750Available = false;

float realLux = NAN;
float humidityPercent = NAN;

int rainRaw = 3500;

String operatingMode = "AUTO";
String simulationPreset = "DARK";
String lightStatus = "Starting...";

float simulatedLux = 5.0;
float simulatedHumidity = 50.0;
int simulatedRainRaw = 3500;

int currentBrightness = 0;
int targetBrightness = 0;

unsigned long lastDHTRead = 0;
unsigned long lastDimUpdate = 0;

String jsonNumber(float value, int decimals = 1) {
  if (isnan(value) || isinf(value)) {
    return "null";
  }
  return String(value, decimals);
}

String jsonEscape(String value) {
  value.replace("\\", "\\\\");
  value.replace("\"", "\\\"");
  value.replace("\n", " ");
  value.replace("\r", " ");
  return value;
}

bool isRainPresent(int rawValue) {
  return rawValue < RAIN_THRESHOLD;
}

void writeHeadlightPWM(int brightness) {
  brightness = constrain(brightness, 0, 255);
  ledcWrite(PWM_CHANNEL_LEFT, brightness);
  ledcWrite(PWM_CHANNEL_RIGHT, brightness);
  currentBrightness = brightness;
}

int calculateAmbientBrightness(float lux) {
  if (isnan(lux) || isinf(lux) || lux < 0) {
    return MEDIUM_BRIGHTNESS;
  }

  if (lux <= DARK_LUX) {
    return 255;
  }

  if (lux >= BRIGHT_LUX) {
    return 0;
  }

  float fraction = (lux - DARK_LUX) / (BRIGHT_LUX - DARK_LUX);
  int pwm = (int)round(255.0 * (1.0 - fraction));

  return constrain(pwm, 0, 255);
}

void readSensors() {
  if (bh1750Available) {
    float value = lightSensor.readLightLevel();

    if (!isnan(value) && !isinf(value) && value >= 0) {
      realLux = value;
    } else {
      realLux = NAN;
    }
  } else {
    realLux = NAN;
  }

  rainRaw = analogRead(RAIN_PIN);

  if (lastDHTRead == 0 ||
      millis() - lastDHTRead >= DHT_INTERVAL) {
    lastDHTRead = millis();

    float h = dht.readHumidity();

    if (!isnan(h) && !isinf(h) && h >= 0 && h <= 100) {
      humidityPercent = h;
    } else {
      humidityPercent = NAN;
    }
  }
}

void updateTargetBrightness() {
  float lux;
  float humidity;
  int rainValue;

  if (operatingMode == "SIMULATION") {
    lux = simulatedLux;
    humidity = simulatedHumidity;
    rainValue = simulatedRainRaw;
  } else {
    lux = realLux;
    humidity = humidityPercent;
    rainValue = rainRaw;
  }

  bool raining = isRainPresent(rainValue);

  int ambientPWM = calculateAmbientBrightness(lux);
  int humidityPWM = 0;

  if (!isnan(humidity) &&
      !isinf(humidity) &&
      humidity > HIGH_HUMIDITY) {
    float progress =
        (humidity - HIGH_HUMIDITY) / (100.0 - HIGH_HUMIDITY);

    progress = constrain(progress, 0.0f, 1.0f);

    humidityPWM = MEDIUM_BRIGHTNESS +
        (int)round(progress * (255 - MEDIUM_BRIGHTNESS));
  }

  targetBrightness = max(ambientPWM, humidityPWM);

  if (raining) {
    targetBrightness = max(targetBrightness, MEDIUM_BRIGHTNESS);
    lightStatus = "RAIN DETECTED - AT LEAST MEDIUM BRIGHTNESS";
  } else if (!isnan(humidity) && humidity > HIGH_HUMIDITY) {
    lightStatus = "HUMIDITY ABOVE 75% - BRIGHTNESS INCREASED";
  } else if (isnan(lux) || isinf(lux)) {
    lightStatus = "LIGHT SENSOR UNAVAILABLE - FALLBACK";
  } else if (lux <= DARK_LUX) {
    lightStatus = "DARK - FULL BRIGHTNESS";
  } else if (lux >= BRIGHT_LUX) {
    lightStatus = "BRIGHT - AMBIENT LIGHT CONTROL";
  } else {
    lightStatus = "AMBIENT LIGHT - GRADUAL DIMMING";
  }

  targetBrightness = constrain(targetBrightness, 0, 255);
}

bool setSimulation(String preset) {
  preset.toUpperCase();

  simulatedHumidity = 50.0;
  simulatedRainRaw = 3500;

  if (preset == "DARK") {
    simulatedLux = 5.0;
  } else if (preset == "DIM") {
    simulatedLux = 100.0;
  } else if (preset == "NORMAL") {
    simulatedLux = 250.0;
  } else if (preset == "BRIGHT") {
    simulatedLux = 800.0;
  } else if (preset == "RAIN") {
    simulatedLux = 250.0;
    simulatedRainRaw = 800;
  } else if (preset == "HUMID") {
    simulatedLux = 250.0;
    simulatedHumidity = 85.0;
  } else if (preset == "VERY_HUMID") {
    simulatedLux = 250.0;
    simulatedHumidity = 95.0;
  } else {
    return false;
  }

  simulationPreset = preset;
  updateTargetBrightness();

  Serial.println();
  Serial.println("SIMULATION UPDATED");
  Serial.print("Preset: ");
  Serial.println(simulationPreset);
  Serial.print("Lux: ");
  Serial.println(simulatedLux);
  Serial.print("Humidity: ");
  Serial.println(simulatedHumidity);
  Serial.print("Rain analog: ");
  Serial.println(simulatedRainRaw);
  Serial.print("Target PWM: ");
  Serial.println(targetBrightness);
  Serial.print("Rule: ");
  Serial.println(lightStatus);

  return true;
}

const char DASHBOARD_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Smart Headlight Control</title>
<style>
*{box-sizing:border-box}
body{margin:0;font-family:Arial,sans-serif;background:#101827;color:#f1f5f9}
header{padding:22px 14px;text-align:center;background:#172554;border-bottom:2px solid #38bdf8}
header h1{margin:0 0 8px;font-size:23px}
header p{margin:0;color:#bfdbfe;font-size:13px}
.container{width:94%;max-width:900px;margin:20px auto}
.card{background:#1e293b;border:1px solid #334155;border-radius:14px;padding:17px;margin-bottom:15px}
h2{color:#7dd3fc;font-size:17px;margin-top:0}
.grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(135px,1fr));gap:10px}
.metric{padding:14px;border-radius:10px;background:#0f172a;min-height:85px}
.metric small{display:block;color:#94a3b8;margin-bottom:9px}
.metric strong{font-size:21px;overflow-wrap:anywhere}
button,select{padding:11px 13px;margin:4px 3px 4px 0;border-radius:8px;border:0;font-size:14px}
button{cursor:pointer;color:white;background:#0369a1}
button.selected{background:#16a34a}
button:disabled{opacity:.5;cursor:wait}
select{background:#0f172a;color:white;border:1px solid #475569;max-width:100%;width:100%}
.status{font-size:17px;font-weight:bold;color:#86efac;line-height:1.5}
.headlight{background:#0f172a;padding:20px;border-radius:12px;text-align:center}
.lamps{display:flex;justify-content:center;gap:45px;margin:15px 0}
.lamp{width:65px;height:65px;border-radius:50%;background:#334155;border:3px solid #64748b;transition:background .15s,box-shadow .15s}
.lamp.on{background:#facc15;border-color:#fef08a;box-shadow:0 0 25px #facc15}
.bar{height:16px;width:100%;background:#334155;border-radius:20px;overflow:hidden;margin-top:12px}
.fill{width:0%;height:100%;background:#facc15;transition:width .2s}
.note{color:#cbd5e1;font-size:13px;line-height:1.6}
footer{text-align:center;padding:20px;color:#94a3b8;font-size:12px}
</style>
</head>
<body>
<header>
<h1>SMART HEADLIGHT CONTROL</h1>
<p>Ambient Light + Rain + Humidity</p>
</header>
<div class="container">
<div class="card">
<h2>Operating Mode</h2>
<button id="autoBtn" onclick="setMode('AUTO')">AUTO MODE</button>
<button id="simBtn" onclick="setMode('SIMULATION')">SIMULATION MODE</button>
<p class="note">Mode: <strong id="mode">Loading...</strong></p>
</div>

<div class="card" id="simulationCard">
<h2>Manual Simulation</h2>
<p class="note">Select a preset and press Apply Simulation.</p>
<select id="preset">
<option value="DARK">Dark - Full brightness</option>
<option value="DIM">Dim light</option>
<option value="NORMAL">Normal light</option>
<option value="BRIGHT">Bright - Ambient control</option>
<option value="RAIN">Rain - Medium brightness</option>
<option value="HUMID">Humidity 85% - Increased brightness</option>
<option value="VERY_HUMID">Humidity 95% - High brightness</option>
</select>
<button id="applyBtn" onclick="setPreset()">Apply Simulation</button>
<p class="note" id="simulationMessage">Select a preset to test.</p>
</div>

<div class="card">
<h2>Sensor Readings</h2>
<div class="grid">
<div class="metric"><small>Ambient light</small><strong id="lux">--</strong><small>lux</small></div>
<div class="metric"><small>Humidity</small><strong id="humidity">--</strong><small>%</small></div>
<div class="metric"><small>Rain analog</small><strong id="rainRaw">--</strong><small>0–4095</small></div>
</div>
</div>

<div class="card">
<h2>Active Control Rule</h2>
<p class="status" id="lightStatus">Waiting...</p>
</div>

<div class="card">
<h2>Headlight Output</h2>
<div class="headlight">
<div class="lamps">
<div><div id="leftLamp" class="lamp"></div><p>Left headlight</p></div>
<div><div id="rightLamp" class="lamp"></div><p>Right headlight</p></div>
</div>
<strong id="brightness">0%</strong>
<div class="bar"><div id="fill" class="fill"></div></div>
<p class="note">Current PWM: <strong id="pwm">0</strong> / 255</p>
<p class="note">Target PWM: <strong id="target">0</strong> / 255</p>
</div>
</div>

<div class="card">
<h2>Control Rules</h2>
<p class="note">
1. Dark environment: full brightness.<br>
2. Humidity at or below 75%: ambient-light control.<br>
3. Humidity above 75%: brightness increases progressively.<br>
4. Rain detection: at least medium brightness.<br>
5. Both headlights receive the same PWM value.
</p>
<p class="note">Rain detection assumes the analog reading decreases when wet. Calibrate the threshold for your sensor.</p>
</div>
</div>
<footer>ESP32 Local Dashboard - No internet required</footer>

<script>
let busy=false;

async function request(url){
  try{
    const response=await fetch(url,{cache:'no-store'});
    const data=await response.json();
    if(!response.ok||data.ok===false){
      throw new Error(data.error||'Request failed');
    }
    return data;
  }catch(error){
    console.error(error);
    return null;
  }
}

async function setMode(mode){
  if(busy)return;
  busy=true;
  setButtonsDisabled(true);

  const result=await request('/mode?value='+encodeURIComponent(mode));

  if(result){
    document.getElementById('simulationMessage').textContent=
      mode==='SIMULATION'
      ?'Simulation enabled. Select a preset and apply it.'
      :'AUTO enabled. Real sensor readings control the lights.';
    await updateDashboard();
  }else{
    document.getElementById('simulationMessage').textContent=
      'Mode change failed. Reload the page and try again.';
  }

  busy=false;
  setButtonsDisabled(false);
}

async function setPreset(){
  if(busy)return;
  const preset=document.getElementById('preset').value;
  busy=true;
  setButtonsDisabled(true);

  const result=await request('/preset?value='+encodeURIComponent(preset));

  if(result){
    document.getElementById('simulationMessage').textContent=
      'Applied preset: '+preset;
    await updateDashboard();
  }else{
    document.getElementById('simulationMessage').textContent=
      'Enable SIMULATION MODE first, then apply the preset.';
  }

  busy=false;
  setButtonsDisabled(false);
}

function setButtonsDisabled(disabled){
  document.getElementById('autoBtn').disabled=disabled;
  document.getElementById('simBtn').disabled=disabled;
  document.getElementById('applyBtn').disabled=disabled;
}

async function updateDashboard(){
  const d=await request('/status');
  if(!d)return;

  document.getElementById('mode').textContent=d.mode;
  document.getElementById('lightStatus').textContent=d.lightStatus;
  document.getElementById('lux').textContent=d.lux===null?'N/A':d.lux;
  document.getElementById('humidity').textContent=d.humidity===null?'N/A':d.humidity;
  document.getElementById('rainRaw').textContent=d.rainRaw;
  document.getElementById('pwm').textContent=d.currentPWM;
  document.getElementById('target').textContent=d.targetPWM;

  const percent=Math.round(d.currentPWM*100/255);
  document.getElementById('brightness').textContent=percent+'%';
  document.getElementById('fill').style.width=percent+'%';

  const on=d.currentPWM>0;
  document.getElementById('leftLamp').classList.toggle('on',on);
  document.getElementById('rightLamp').classList.toggle('on',on);

  document.getElementById('autoBtn').classList.toggle('selected',d.mode==='AUTO');
  document.getElementById('simBtn').classList.toggle('selected',d.mode==='SIMULATION');

  document.getElementById('simulationCard').style.display=
    d.mode==='SIMULATION'?'block':'none';

  if(d.mode==='SIMULATION'&&!busy){
    document.getElementById('preset').value=d.preset;
  }
}

updateDashboard();
setInterval(updateDashboard,500);
</script>
</body>
</html>
)rawliteral";

void handleStatus() {
  float displayLux = realLux;
  float displayHumidity = humidityPercent;
  int displayRainRaw = rainRaw;

  if (operatingMode == "SIMULATION") {
    displayLux = simulatedLux;
    displayHumidity = simulatedHumidity;
    displayRainRaw = simulatedRainRaw;
  }

  String json = "{";

  json += "\"ok\":true,";
  json += "\"mode\":\"" + jsonEscape(operatingMode) + "\",";
  json += "\"preset\":\"" + jsonEscape(simulationPreset) + "\",";
  json += "\"lightStatus\":\"" + jsonEscape(lightStatus) + "\",";
  json += "\"lux\":" + jsonNumber(displayLux) + ",";
  json += "\"humidity\":" + jsonNumber(displayHumidity) + ",";
  json += "\"rainRaw\":" + String(displayRainRaw) + ",";
  json += "\"currentPWM\":" + String(currentBrightness) + ",";
  json += "\"targetPWM\":" + String(targetBrightness);
  json += "}";

  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "application/json", json);
}

void handleMode() {
  if (!server.hasArg("value")) {
    server.send(400, "application/json",
                "{\"ok\":false,\"error\":\"Missing mode\"}");
    return;
  }

  String requested = server.arg("value");
  requested.toUpperCase();

  if (requested != "AUTO" && requested != "SIMULATION") {
    server.send(400, "application/json",
                "{\"ok\":false,\"error\":\"Invalid mode\"}");
    return;
  }

  operatingMode = requested;

  if (operatingMode == "AUTO") {
    readSensors();
  }

  updateTargetBrightness();
  server.send(200, "application/json", "{\"ok\":true}");
}

void handlePreset() {
  if (!server.hasArg("value")) {
    server.send(400, "application/json",
                "{\"ok\":false,\"error\":\"Missing preset\"}");
    return;
  }

  String requested = server.arg("value");
  requested.toUpperCase();

  if (operatingMode != "SIMULATION") {
    server.send(409, "application/json",
                "{\"ok\":false,\"error\":\"Enable SIMULATION mode first\"}");
    return;
  }

  if (!setSimulation(requested)) {
    server.send(400, "application/json",
                "{\"ok\":false,\"error\":\"Invalid preset\"}");
    return;
  }

  server.send(200, "application/json", "{\"ok\":true}");
}

void handleRoot() {
  server.send_P(200, "text/html", DASHBOARD_HTML);
}

void handleNotFound() {
  server.send(404, "text/plain", "Not found");
}

void setup() {
  Serial.begin(115200);
  delay(500);

  Serial.println();
  Serial.println("SMART HEADLIGHT SYSTEM");

  analogReadResolution(12);
  pinMode(RAIN_PIN, INPUT);

  ledcSetup(PWM_CHANNEL_LEFT, PWM_FREQUENCY, PWM_RESOLUTION);
  ledcSetup(PWM_CHANNEL_RIGHT, PWM_FREQUENCY, PWM_RESOLUTION);

  ledcAttachPin(MAIN_LEFT_PIN, PWM_CHANNEL_LEFT);
  ledcAttachPin(MAIN_RIGHT_PIN, PWM_CHANNEL_RIGHT);

  writeHeadlightPWM(0);

  dht.begin();

  Wire.begin(I2C_SDA, I2C_SCL);

  bh1750Available = lightSensor.begin(
    BH1750::CONTINUOUS_HIGH_RES_MODE
  );

  Serial.println(bh1750Available
    ? "BH1750 initialized."
    : "WARNING: BH1750 not detected.");

  WiFi.mode(WIFI_AP);

  if (WiFi.softAP(AP_SSID, AP_PASSWORD)) {
    Serial.println("Wi-Fi access point started.");
    Serial.print("Dashboard: http://");
    Serial.println(WiFi.softAPIP());
  } else {
    Serial.println("ERROR: Wi-Fi AP failed.");
  }

  server.on("/", HTTP_GET, handleRoot);
  server.on("/status", HTTP_GET, handleStatus);
  server.on("/mode", HTTP_GET, handleMode);
  server.on("/preset", HTTP_GET, handlePreset);
  server.onNotFound(handleNotFound);

  server.begin();

  lastDHTRead = 0;

  readSensors();
  updateTargetBrightness();

  Serial.println("Web server started.");
  Serial.println("Mode: AUTO");
}

void loop() {
  server.handleClient();

  if (operatingMode == "AUTO") {
    readSensors();
  }

  updateTargetBrightness();

  if (millis() - lastDimUpdate >= DIM_INTERVAL_MS) {
    lastDimUpdate = millis();

    if (currentBrightness < targetBrightness) {
      int nextValue = currentBrightness + DIM_STEP;

      if (nextValue > targetBrightness) {
        nextValue = targetBrightness;
      }

      writeHeadlightPWM(nextValue);
    }
    else if (currentBrightness > targetBrightness) {
      int nextValue = currentBrightness - DIM_STEP;

      if (nextValue < targetBrightness) {
        nextValue = targetBrightness;
      }

      writeHeadlightPWM(nextValue);
    }
  }

  delay(2);
}
