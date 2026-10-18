#include <Wire.h>
#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <math.h>

// ==========================================
// SPINESENSE: POSTURE MONITOR + WIFI DASHBOARD (merged)
// Board: ESP8266 NodeMCU
// Your original posture logic is kept as-is; the dashboard,
// terminal and live settings are added around it.
// ==========================================

// ---------- WIFI ACCESS POINT ----------
const char* AP_SSID = "SpineSense";
const char* AP_PASSWORD = "Spine1234";

ESP8266WebServer server(80);

// ---------- HARDWARE PINS ----------
const int MPU_ADDR = 0x68;
const int BUZZER_PIN = D5;
const int TOUCH_PIN = D6;
const int LDR_PIN = A0;

// ---------- FIXED TIMINGS ----------
const unsigned long CALIBRATION_WAIT = 5000;
const unsigned long BEEP_INTERVAL = 200;  // Start a new beep every 500 ms while alarm is active
const unsigned long LONG_PRESS_DURATION = 2000;

// ---------- ADJUSTABLE SETTINGS (dashboard sliders change these) ----------
unsigned long POSTURE_DELAY = 10000;
unsigned long SNOOZE_DURATION = 30000;
float POSTURE_THRESHOLD = 30.0;
float ORIGINAL_POSITION_RANGE = 10.0;
int DIM_THRESHOLD = 500;

// ---------- POSTURE DATA ----------
float baselineAngle = 0.0;
float filteredAngle = 0.0;
float relativeAngle = 0.0;

bool sensorReady = false;
bool sensorConnected = false;

bool postureCandidate = false;
bool postureAlarm = false;

unsigned long postureStartTime = 0;
unsigned long nextBeepTime = 0;

// ---------- TOUCH DATA ----------
bool previousTouch = false;
bool longPressHandled = false;
bool touchedNow = false;
unsigned long touchStartTime = 0;

// ---------- SNOOZE DATA ----------
unsigned long snoozeUntil = 0;
bool wasSnoozing = false;

// ---------- BUZZER DATA ----------
bool buzzerOn = false;
unsigned long buzzerOffTime = 0;

// ---------- DASHBOARD DATA ----------
int lightValue = 0;
unsigned long lastStatusPrint = 0;
int calPhase = 0;                 // 0 = not calibrating, 1 = waiting, 2 = sampling
unsigned long calEnd = 0;
bool recalRequested = false;      // set by the dashboard, handled safely in loop()

// ---------- TERMINAL LOG (shown in dashboard) ----------
#define LOG_N 40
String logBuf[LOG_N];
unsigned long logId = 0;

void logLine(const String& s) {
  String l = "[" + String(millis() / 1000.0, 1) + "s] " + s;
  Serial.println(l);
  logBuf[logId % LOG_N] = l;
  logId++;
}

// ==========================================
// TIME HELPER
// ==========================================
bool timeReached(unsigned long now, unsigned long target) {
  return (long)(now - target) >= 0;
}

// Like delay(), but keeps answering dashboard requests meanwhile.
void waitServing(unsigned long ms) {
  unsigned long end = millis() + ms;
  while (!timeReached(millis(), end)) {
    server.handleClient();
    delay(2);
  }
}

// ==========================================
// READ MPU6050 ACCELEROMETER
// ==========================================
bool readAccel(float &axG, float &ayG, float &azG) {
  Wire.beginTransmission(MPU_ADDR);
  Wire.write(0x3B);

  if (Wire.endTransmission(false) != 0) {
    return false;
  }

  Wire.requestFrom(MPU_ADDR, 6);

  if (Wire.available() < 6) {
    return false;
  }

  int16_t ax = (int16_t)(((uint16_t)Wire.read() << 8) | Wire.read());
  int16_t ay = (int16_t)(((uint16_t)Wire.read() << 8) | Wire.read());
  int16_t az = (int16_t)(((uint16_t)Wire.read() << 8) | Wire.read());

  // Default MPU6050 range: +/- 2g
  axG = ax / 16384.0;
  ayG = ay / 16384.0;
  azG = az / 16384.0;

  return true;
}

// ==========================================
// CALCULATE ANGLE
// ==========================================
float calculateAngle(float ayG, float azG) {
  return atan2(ayG, azG) * 180.0 / PI;
}

float angleDifference(float angle, float baseline) {
  float difference = angle - baseline;

  while (difference > 180.0) difference -= 360.0;
  while (difference < -180.0) difference += 360.0;

  return difference;
}

// ==========================================
// BUZZER CONTROL
// ==========================================
void stopBuzzer() {
  noTone(BUZZER_PIN);
  buzzerOn = false;
}

void startBeep(unsigned long now) {
  tone(BUZZER_PIN, 2500);
  buzzerOn = true;
  buzzerOffTime = now + 150;
}

// ==========================================
// SNOOZE (used by touch sensor and dashboard)
// ==========================================
void applySnooze() {
  snoozeUntil = millis() + SNOOZE_DURATION;

  postureAlarm = false;
  postureCandidate = false;
  postureStartTime = 0;

  stopBuzzer();

  logLine("Posture alarm snoozed for " + String(SNOOZE_DURATION / 1000) + " seconds.");
}

// ==========================================
// CALIBRATE ORIGINAL POSITION
// ==========================================
bool calibratePosition() {
  stopBuzzer();

  postureAlarm = false;
  postureCandidate = false;
  postureStartTime = 0;

  snoozeUntil = 0;
  wasSnoozing = false;

  logLine("Calibration started. Hold the desired position for 5 seconds.");

  calPhase = 1;
  calEnd = millis() + CALIBRATION_WAIT;
  waitServing(CALIBRATION_WAIT);

  calPhase = 2;
  calEnd = millis() + 1000;

  float total = 0.0;
  int samples = 0;

  for (int i = 0; i < 50; i++) {
    float ax, ay, az;

    if (readAccel(ax, ay, az)) {
      total += calculateAngle(ay, az);
      samples++;
    }

    waitServing(20);
  }

  calPhase = 0;
  recalRequested = false;

  if (samples < 40) {
    logLine("Calibration failed!");
    sensorReady = false;
    sensorConnected = false;
    return false;
  }

  baselineAngle = total / samples;
  filteredAngle = baselineAngle;
  relativeAngle = 0.0;

  sensorReady = true;
  sensorConnected = true;

  logLine("Baseline angle: " + String(baselineAngle, 1) + " deg. Calibration complete.");

  return true;
}

// ==========================================
// DASHBOARD HTML
// ==========================================
const char PAGE[] PROGMEM = R"HTML(<!doctype html><html lang="en"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1"><meta name="theme-color" content="#080b14"><title>SpineSense | Neural Posture System</title>
<style>
:root{color-scheme:dark;--bg:#080b14;--panel:#101625;--panel2:#0c1220;--line:#202b42;--tx:#edf4ff;--mut:#8493ad;--cyan:#62e8ff;--violet:#a78bfa;--ok:#5ee6b0;--warn:#ffc56b;--bad:#ff6685;--c:var(--cyan)}
*{box-sizing:border-box}html{scroll-behavior:smooth}body{margin:0;background:radial-gradient(ellipse at 12% 0%,#17254a 0,transparent 35%),radial-gradient(ellipse at 100% 20%,#241a45 0,transparent 27%),var(--bg);color:var(--tx);font:14px/1.5 Inter,ui-sans-serif,system-ui,-apple-system,"Segoe UI",sans-serif;padding:24px;max-width:1440px;margin:auto}
header{display:flex;justify-content:space-between;align-items:center;gap:16px;margin-bottom:26px}.brand{display:flex;align-items:center;gap:13px}.logo{width:44px;height:44px;border-radius:14px;display:grid;place-items:center;background:linear-gradient(135deg,#62e8ff,#8b7cff);color:#07101a;font-size:24px;font-weight:900;box-shadow:0 0 28px #62e8ff35}.eyebrow{font-size:10px;letter-spacing:.2em;text-transform:uppercase;color:var(--cyan);font-weight:800}h1{font-size:23px;letter-spacing:-.04em;margin:0;font-weight:750}.subtitle{color:var(--mut);font-size:12px;margin-top:2px}.topright{display:flex;align-items:center;gap:10px;flex-wrap:wrap}.pill{display:inline-flex;align-items:center;gap:8px;padding:8px 12px;border:1px solid var(--line);border-radius:99px;background:#101625c9;color:var(--c);font-size:12px;font-weight:650}.dot{width:8px;height:8px;border-radius:50%;background:currentColor;box-shadow:0 0 12px currentColor}.tiny{color:var(--mut);font-size:11px}.layout{display:grid;grid-template-columns:minmax(280px,.86fr) minmax(0,1.6fr);gap:18px}.panel{background:linear-gradient(145deg,#141c2dF2,#0d1321F2);border:1px solid #28334b;border-radius:22px;padding:21px;box-shadow:0 18px 45px #0002;min-width:0}.panelhead{display:flex;justify-content:space-between;align-items:center;gap:12px;margin-bottom:17px}.paneltitle{font-weight:750;font-size:14px;letter-spacing:-.01em}.kicker{font-size:10px;letter-spacing:.14em;text-transform:uppercase;color:var(--mut);font-weight:750}.tag{font-size:10px;color:var(--cyan);border:1px solid #62e8ff40;background:#62e8ff0d;border-radius:7px;padding:4px 7px}.gaugewrap{display:grid;place-items:center;padding:5px 0 14px}.ring{width:min(260px,78vw);aspect-ratio:1;border-radius:50%;display:grid;place-items:center;position:relative;background:conic-gradient(from 220deg,var(--c) calc(var(--p)*1%),#273147 0 78%,transparent 78%);filter:drop-shadow(0 0 18px color-mix(in srgb,var(--c) 16%,transparent))}.ring:before{content:"";position:absolute;inset:10px;border-radius:50%;background:linear-gradient(145deg,#141c2d,#0b101d);border:1px solid #2a354d}.ring:after{content:"";position:absolute;inset:22px;border-radius:50%;border:1px dashed #35425c}.gaugecontent{position:relative;z-index:1;text-align:center}.big{font-size:53px;line-height:1.1;letter-spacing:-.06em;font-weight:750;color:var(--c);font-variant-numeric:tabular-nums}.unit{font-size:19px;letter-spacing:0}.gaugecaption{color:var(--mut);font-size:11px;margin-top:8px}.gaugemini{font-size:10px;color:var(--mut);letter-spacing:.12em;text-transform:uppercase;margin-bottom:6px}.statusline{text-align:center;font-size:13px;font-weight:700;color:var(--c);margin:0 0 17px}.statusline:before{content:"";display:inline-block;width:6px;height:6px;border-radius:50%;background:currentColor;vertical-align:middle;margin-right:8px}.metrics{display:grid;grid-template-columns:repeat(2,minmax(0,1fr));gap:10px}.metric{border:1px solid var(--line);background:#0b1120;border-radius:14px;padding:13px;min-width:0}.metriclabel{font-size:10px;color:var(--mut);text-transform:uppercase;letter-spacing:.1em}.metricvalue{font-size:23px;font-weight:750;letter-spacing:-.04em;margin-top:5px;overflow-wrap:anywhere;font-variant-numeric:tabular-nums}.metricfoot{font-size:10px;color:var(--mut);margin-top:2px}.rightcol{display:grid;gap:18px;align-content:start}.chartbox{height:235px;position:relative}canvas{width:100%;height:100%;display:block}.chartlegend{display:flex;gap:14px;flex-wrap:wrap;color:var(--mut);font-size:10px;margin-top:8px}.legenddot{display:inline-block;width:7px;height:7px;border-radius:50%;margin-right:5px;background:var(--cyan)}.legendline{display:inline-block;width:15px;height:2px;background:#64748b;vertical-align:middle;margin-right:5px}.lower{display:grid;grid-template-columns:1fr 1fr;gap:18px;margin-top:18px}.lightvalue{display:flex;justify-content:space-between;align-items:baseline;gap:10px}.lightnumber{font-size:30px;font-weight:750;letter-spacing:-.05em}.bar{height:8px;background:#252f45;border-radius:99px;margin:12px 0 8px;overflow:hidden}.bar span{display:block;height:100%;width:0;background:linear-gradient(90deg,var(--violet),var(--cyan));border-radius:99px;transition:width .3s}.btns{display:flex;gap:9px;flex-wrap:wrap;margin:16px 0 4px}button{font:inherit;font-weight:650;font-size:12px;color:var(--tx);background:#192238;border:1px solid #303d57;border-radius:11px;padding:10px 13px;cursor:pointer;transition:transform .15s,border-color .15s,background .15s}button:hover,button:focus-visible{border-color:var(--cyan);background:#1d2b43;outline:none}button:active{transform:scale(.98)}button.primary{background:linear-gradient(120deg,#62e8ff,#a78bfa);border:0;color:#07101a}#sets{margin-top:16px;display:grid;grid-template-columns:1fr;gap:2px}#sets label{display:flex;justify-content:space-between;gap:12px;margin:12px 0 4px;font-size:12px;color:#dce7f8}#sets label span:last-child{color:var(--cyan);font-variant-numeric:tabular-nums}input[type=range]{width:100%;accent-color:var(--cyan);cursor:pointer}#term{height:205px;overflow:auto;background:#070b13;border:1px solid #222d43;border-radius:12px;padding:12px;font-size:11px;white-space:pre-wrap;color:#a7b9d5;font-family:ui-monospace,SFMono-Regular,Consolas,monospace}#term .a{color:var(--bad)}#term .w{color:var(--warn)}#cmd{width:100%;margin-top:9px;background:#070b13;color:var(--cyan);border:1px solid #29344b;border-radius:11px;padding:11px 12px;font:12px ui-monospace,SFMono-Regular,Consolas,monospace;outline:none}#cmd:focus{border-color:var(--cyan)}.foot{display:flex;justify-content:space-between;gap:10px;flex-wrap:wrap;margin:18px 3px 0;color:#687893;font-size:10px}.foot strong{color:#a4b3ca;font-weight:650}
@media(max-width:900px){body{padding:15px}.layout{grid-template-columns:1fr}.lower{grid-template-columns:1fr}.panel{padding:17px}.rightcol{gap:14px}header{align-items:flex-start}.topright{justify-content:flex-end}.ring{width:min(250px,70vw)}}@media(max-width:460px){body{padding:11px}.brand{gap:9px}.logo{width:38px;height:38px;border-radius:12px}h1{font-size:20px}.subtitle{font-size:10px}.topright{max-width:130px}.topright .tiny{display:none}.panel{border-radius:17px;padding:14px}.big{font-size:45px}.chartbox{height:190px}.metricvalue{font-size:20px}}
@media(prefers-reduced-motion:reduce){*{transition:none!important;scroll-behavior:auto!important}}
/* SPINESENSE CYBERPUNK REDESIGN v2 — visual-only overrides; API and controls stay intact */
:root{color-scheme:dark;--bg:#05070b;--panel:#0c1118;--panel2:#080d13;--line:#26352f;--tx:#effff6;--mut:#8a9b95;--cyan:#b7ff3c;--violet:#b18cff;--ok:#b7ff3c;--warn:#ffd166;--bad:#ff5277;--c:var(--cyan)}
body{max-width:1500px;padding:26px;background:radial-gradient(ellipse at 18% -10%,#24391a 0,transparent 31%),radial-gradient(ellipse at 100% 5%,#25183b 0,transparent 28%),linear-gradient(rgba(183,255,60,.025) 1px,transparent 1px),linear-gradient(90deg,rgba(183,255,60,.025) 1px,transparent 1px),#05070b;background-size:auto,auto,28px 28px,28px 28px,auto}
header{padding:0 0 19px;border-bottom:1px solid #b7ff3c30;margin-bottom:22px}.logo{border-radius:12px;background:linear-gradient(135deg,#b7ff3c,#d8ff91);color:#091006;font-size:31px;box-shadow:0 0 32px #b7ff3c35}.eyebrow{color:var(--cyan);letter-spacing:.24em}h1{font-size:clamp(19px,2.2vw,27px);letter-spacing:.015em}.subtitle{letter-spacing:.02em}.pill{border-color:#b7ff3c40;background:#0b130e;color:var(--cyan)}.panel{background:linear-gradient(145deg,rgba(16,24,28,.97),rgba(8,13,18,.98));border:1px solid #33463b;border-radius:15px;box-shadow:0 12px 38px #0008,inset 0 1px #ffffff08;position:relative;overflow:hidden}.panel:before{content:"";position:absolute;left:0;top:0;width:34px;height:2px;background:var(--cyan);box-shadow:0 0 12px var(--cyan)}.paneltitle{font-size:15px}.kicker{color:#a7b8af;letter-spacing:.19em}.tag{color:var(--cyan);border-color:#b7ff3c50;background:#b7ff3c0b;letter-spacing:.09em}.ring{filter:drop-shadow(0 0 22px color-mix(in srgb,var(--c) 24%,transparent))}.ring:before{background:radial-gradient(circle at 50% 35%,#151f20,#080d12 72%);border-color:#35463b}.ring:after{border-color:#465748}.big{text-shadow:0 0 24px color-mix(in srgb,var(--c) 30%,transparent)}.metric{background:linear-gradient(145deg,#10171b,#090e13);border-color:#293a32;border-radius:11px}.metriclabel{letter-spacing:.14em}.metricvalue{color:#f1fff5}.metricfoot{color:#84978c}.chartbox{background:linear-gradient(180deg,#070d10a0,transparent);border-radius:10px}.lower{margin-top:18px}.bar{background:#1b2922}.bar span{background:linear-gradient(90deg,#b18cff,#b7ff3c);box-shadow:0 0 12px #b7ff3c50}button{border-radius:9px;background:#111c19;border-color:#34483c}button:hover,button:focus-visible{border-color:var(--cyan);background:#19271e;box-shadow:0 0 16px #b7ff3c18}button.primary{background:linear-gradient(110deg,#b7ff3c,#e2ffad);color:#0a1207;box-shadow:0 0 20px #b7ff3c18}#sets label span:last-child{color:var(--cyan)}input[type=range]{accent-color:var(--cyan)}#term{background:#040807;border-color:#26372d;color:#b7d5c0;border-radius:9px}#cmd{background:#040807;border-color:#304338;color:var(--cyan);border-radius:9px}.foot{border-top:1px solid #b7ff3c20;padding-top:13px}
@media(max-width:900px){body{padding:15px}.panel{border-radius:13px}}
</style></head><body>
<header><div class="brand"><div class="logo">⌁</div><div><div class="eyebrow">PERSONAL BIOMETRIC INTERFACE · 01</div><h1>SPINESENSE <span style="color:var(--violet)">//</span> NEURAL POSTURE</h1><div class="subtitle">Live body-alignment telemetry <span style="color:var(--ok)">●</span> local edge system</div></div></div><div class="topright"><span class="pill"><i class="dot"></i><span id="st">Connecting to device…</span></span><span class="tiny">LOCAL DEVICE · LIVE</span></div></header>
<div class="layout">
 <section class="panel"><div class="panelhead"><div><div class="kicker">Posture monitor</div><div class="paneltitle">Live deviation</div></div><span class="tag">LIVE FEED</span></div>
  <div class="gaugewrap"><div class="ring" id="ring" style="--p:0"><div class="gaugecontent"><div class="gaugemini">from baseline</div><div class="big" id="dev">--<span class="unit">°</span></div><div class="gaugecaption">angle difference</div></div></div></div><div class="statusline" id="statusline">Waiting for sensor data</div>
  <div class="metrics"><div class="metric"><div class="metriclabel">Current tilt</div><div class="metricvalue" id="ang">--</div><div class="metricfoot">measured angle</div></div><div class="metric"><div class="metriclabel">Baseline</div><div class="metricvalue" id="ref">--</div><div class="metricfoot">reference angle</div></div><div class="metric"><div class="metriclabel">Touch sensor</div><div class="metricvalue" id="tch">--</div><div class="metricfoot">snooze / recalibrate</div></div><div class="metric"><div class="metriclabel">Device uptime</div><div class="metricvalue" id="up">--</div><div class="metricfoot">since power-on</div></div></div>
 </section>
 <div class="rightcol"><section class="panel"><div class="panelhead"><div><div class="kicker">Sensor telemetry</div><div class="paneltitle">Tilt history</div></div><span class="tag">ROLLING TRACE</span></div><div class="chartbox"><canvas id="cv"></canvas></div><div class="chartlegend"><span><i class="legenddot"></i>Measured angle</span><span><i class="legendline"></i>Baseline</span><span><i class="legendline" style="background:#62e8ff55"></i>Allowed range</span></div></section>
 <section class="panel"><div class="panelhead"><div><div class="kicker">Device controls</div><div class="paneltitle">Quick actions</div></div><span class="tag">ON DEVICE</span></div><div class="btns" style="margin-top:0"><button class="primary" onclick="cmd('recal')">↻ Recalibrate</button><button onclick="cmd('snooze')">◷ Snooze alarm</button><button onclick="cmd('beep')">♫ Test buzzer</button></div><div class="tiny">Settings update the running device. Keep this page open for live monitoring.</div></section></div>
</div>
<div class="lower"><section class="panel"><div class="panelhead"><div><div class="kicker">Environment</div><div class="paneltitle">Ambient light</div></div><span class="tag" id="ld">READING…</span></div><div class="lightvalue"><div class="lightnumber" id="lv">--</div><div class="tiny">Analog sensor · A0</div></div><div class="bar"><span id="lb"></span></div><div class="tiny">Light level compared with your configured dim-light threshold.</div><div class="panelhead" style="margin:22px 0 8px"><div><div class="kicker">Personalization</div><div class="paneltitle">Monitor settings</div></div><span class="tag">ADJUSTABLE</span></div><div id="sets"></div></section>
 <section class="panel"><div class="panelhead"><div><div class="kicker">System activity</div><div class="paneltitle">Event console</div></div><button onclick="cmd('clear')">Clear log</button></div><div id="term" aria-live="polite"></div><input id="cmd" placeholder="Enter command: snooze, recal, beep, clear" autocomplete="off" spellcheck="false"><div class="tiny" style="margin-top:9px">Device events and command responses appear here.</div></section></div>
<footer class="foot"><span><strong>SPINESENSE</strong> · posture companion prototype</span><span>Connected over the device's local Wi-Fi · dashboard data is not cloud-hosted</span></footer>
<script>
const $=i=>document.getElementById(i);let hist=[],lastLog=0,D={};
const S=[["thr","Posture threshold","deg",5,60,1],["ret","Return range","deg",2,25,1],["per","Persistence","s",1,30,1],["snz","Snooze duration","s",5,120,5],["ldr","Dim light threshold","raw",100,1000,10]];
$("sets").innerHTML=S.map(s=>`<label><span>${s[1]}</span><span id="v_${s[0]}"></span></label><input aria-label="${s[1]}" type="range" id="r_${s[0]}" min="${s[3]}" max="${s[4]}" step="${s[5]}">`).join("");
S.forEach(s=>{$("r_"+s[0]).onchange=e=>fetch(`/api/set?k=${s[0]}&v=${e.target.value}`).catch(()=>{})});
function cmd(c){fetch("/api/cmd?c="+encodeURIComponent(c)).catch(()=>{})}
$("cmd").onkeydown=e=>{if(e.key==="Enter"&&e.target.value){cmd(e.target.value.trim());e.target.value=""}};
function fmt(s){s=Math.floor(s);if(s<3600)return Math.floor(s/60)+"m "+s%60+"s";return Math.floor(s/3600)+"h "+Math.floor(s%3600/60)+"m"}
function render(d){D=d;let c="--cyan",t="Posture within range",short="CONNECTED";
 if(!d.mpu){c="--bad";t="MPU6050 sensor not found";short="SENSOR ERROR"}
 else if(d.state===0){c="--warn";t="Calibration starts in "+d.left+"s";short="CALIBRATING"}
 else if(d.state===1){c="--warn";t="Calibrating reference position";short="CALIBRATING"}
 else if(d.snz>0){c="--violet";t="Alarm snoozed · "+d.snz+"s remaining";short="SNOOZED"}
 else if(d.alarm){c="--bad";t="Posture alert · adjust your position";short="ALARM ACTIVE"}
 else if(d.bad>0){c="--warn";t="Outside preferred range · "+d.bad+"s";short="CHECK POSTURE"}
 document.documentElement.style.setProperty("--c","var("+c+")");$("st").textContent=short;$("statusline").textContent=t;
 $("dev").innerHTML=d.dev.toFixed(1)+"<span class=\"unit\">°</span>";$("ang").textContent=d.ang.toFixed(1)+"°";$("ref").textContent=d.ref.toFixed(1)+"°";
 $("ring").style.setProperty("--p",Math.min(78,d.dev/Math.max(1,d.thr)*78));$("tch").textContent=d.touch?"Active":"Idle";$("up").textContent=fmt(d.up);
 $("lv").textContent=d.ldr;$("lb").style.width=Math.min(100,d.ldr/10)+"%";$("ld").textContent=d.ldr>d.ldrT?"DIM LIGHT":"BRIGHT";
 S.forEach(s=>{const v={thr:d.thr,ret:d.ret,per:d.per,snz:d.snzS,ldr:d.ldrT}[s[0]];$("v_"+s[0]).textContent=v+" "+s[2];const r=$("r_"+s[0]);if(document.activeElement!==r)r.value=v});
 hist.push(d.ang);if(hist.length>150)hist.shift();draw();}
function draw(){const cv=$("cv"),r=window.devicePixelRatio||1,w=Math.max(1,cv.clientWidth),h=Math.max(1,cv.clientHeight);cv.width=w*r;cv.height=h*r;const x=cv.getContext("2d");x.setTransform(r,0,0,r,0,0);x.clearRect(0,0,w,h);const ref=Number(D.ref)||0,th=Number(D.thr)||30,vals=hist.concat([ref-th,ref+th,ref]);const lo=Math.min(...vals)-5,hi=Math.max(...vals)+5,span=Math.max(20,hi-lo);const Y=a=>h-(a-lo)/span*h;
 x.fillStyle="rgba(98,232,255,.08)";x.fillRect(0,Y(ref+th),w,Math.max(0,Y(ref-th)-Y(ref+th)));
 x.strokeStyle="#364158";x.lineWidth=1;x.setLineDash([5,5]);x.beginPath();x.moveTo(0,Y(ref));x.lineTo(w,Y(ref));x.stroke();x.setLineDash([]);
 for(let i=1;i<4;i++){x.strokeStyle="rgba(132,147,173,.12)";x.beginPath();x.moveTo(0,h*i/4);x.lineTo(w,h*i/4);x.stroke()}
 x.strokeStyle=getComputedStyle(document.documentElement).getPropertyValue("--c").trim()||"#62e8ff";x.lineWidth=2;x.lineJoin="round";x.beginPath();hist.forEach((a,i)=>{const px=i/Math.max(1,149)*w,py=Math.max(2,Math.min(h-2,Y(a)));i?x.lineTo(px,py):x.moveTo(px,py)});x.stroke();if(hist.length){const px=(hist.length-1)/149*w,py=Math.max(2,Math.min(h-2,Y(hist[hist.length-1])));x.fillStyle=x.strokeStyle;x.beginPath();x.arc(px,py,3.5,0,Math.PI*2);x.fill()}}
async function tick(){try{render(await(await fetch("/api/data",{cache:"no-store"})).json())}catch(e){$("st").textContent="DEVICE OFFLINE";$("statusline").textContent="Waiting for dashboard connection"}setTimeout(tick,400)}
async function logs(){try{const j=await(await fetch("/api/log?after="+lastLog,{cache:"no-store"})).json();const t=$("term");if(j.n<lastLog)t.innerHTML="";if(j.l.length){const stick=t.scrollTop+t.clientHeight>=t.scrollHeight-20;j.l.forEach(l=>{const s=document.createElement("div");s.textContent=l;if(/ALARM/i.test(l))s.className="a";else if(/Snooz|Calib/i.test(l))s.className="w";t.appendChild(s)});while(t.children.length>200)t.removeChild(t.firstChild);if(stick)t.scrollTop=t.scrollHeight}lastLog=j.n}catch(e){}setTimeout(logs,800)}
window.addEventListener("resize",draw);tick();logs();
</script></body></html>)HTML";

// ==========================================
// DASHBOARD API
// ==========================================
void handleData() {
  unsigned long now = millis();

  bool snoozing = snoozeUntil != 0 && !timeReached(now, snoozeUntil);
  long snz = snoozing ? (long)((snoozeUntil - now) / 1000) + 1 : 0;
  long left = (calPhase != 0 && !timeReached(now, calEnd)) ? (long)((calEnd - now) / 1000) + 1 : 0;
  long bad = postureCandidate ? (long)((now - postureStartTime) / 1000) : 0;
  int state = (calPhase == 1) ? 0 : (calPhase == 2) ? 1 : 2;

  String j;
  j.reserve(300);
  j = "{";
  j += "\"mpu\":" + String(sensorConnected ? 1 : 0);
  j += ",\"state\":" + String(state);
  j += ",\"ang\":" + String(filteredAngle, 1);
  j += ",\"ref\":" + String(baselineAngle, 1);
  j += ",\"dev\":" + String(fabs(relativeAngle), 1);
  j += ",\"alarm\":" + String(postureAlarm ? 1 : 0);
  j += ",\"snz\":" + String(snz);
  j += ",\"left\":" + String(left);
  j += ",\"bad\":" + String(bad);
  j += ",\"ldr\":" + String(lightValue);
  j += ",\"touch\":" + String(touchedNow ? 1 : 0);
  j += ",\"up\":" + String(now / 1000);
  j += ",\"thr\":" + String(POSTURE_THRESHOLD, 0);
  j += ",\"ret\":" + String(ORIGINAL_POSITION_RANGE, 0);
  j += ",\"per\":" + String(POSTURE_DELAY / 1000);
  j += ",\"snzS\":" + String(SNOOZE_DURATION / 1000);
  j += ",\"ldrT\":" + String(DIM_THRESHOLD);
  j += "}";

  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "application/json", j);
}

void handleLog() {
  unsigned long after = server.hasArg("after") ? strtoul(server.arg("after").c_str(), NULL, 10) : 0;
  unsigned long oldest = (logId > LOG_N) ? logId - LOG_N : 0;
  if (after > logId) after = 0;
  if (after < oldest) after = oldest;

  String j = "{\"n\":" + String(logId) + ",\"l\":[";
  for (unsigned long i = after; i < logId; i++) {
    String s = logBuf[i % LOG_N];
    s.replace("\\", "\\\\");
    s.replace("\"", "\\\"");
    if (i > after) j += ",";
    j += "\"" + s + "\"";
  }
  j += "]}";

  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "application/json", j);
}

void handleCmd() {
  String c = server.arg("c");

  if (c == "snooze") applySnooze();
  else if (c == "recal") recalRequested = true;      // done in loop(), not here
  else if (c == "beep") { startBeep(millis()); logLine("Test beep"); }
  else if (c == "clear") { for (int i = 0; i < LOG_N; i++) logBuf[i] = ""; logId = 0; }
  else logLine("Unknown command: " + c);

  server.send(200, "text/plain", "ok");
}

void handleSet() {
  String k = server.arg("k");
  float v = server.arg("v").toFloat();

  if (k == "thr") POSTURE_THRESHOLD = constrain(v, 5, 60);
  else if (k == "ret") ORIGINAL_POSITION_RANGE = constrain(v, 2, 25);
  else if (k == "per") POSTURE_DELAY = (unsigned long)constrain((int)v, 1, 30) * 1000UL;
  else if (k == "snz") SNOOZE_DURATION = (unsigned long)constrain((int)v, 5, 120) * 1000UL;
  else if (k == "ldr") DIM_THRESHOLD = constrain((int)v, 100, 1000);
  else { server.send(400, "text/plain", "bad key"); return; }

  logLine("Setting " + k + " = " + String(v, 0));
  server.send(200, "text/plain", "ok");
}

// ==========================================
// START WIFI AND WEB SERVER
// ==========================================
void startDashboard() {
  WiFi.mode(WIFI_AP);

  bool apStarted = WiFi.softAP(AP_SSID, AP_PASSWORD);

  if (apStarted) {
    logLine(String("Wi-Fi access point started: ") + AP_SSID);
    logLine("Open dashboard: http://" + WiFi.softAPIP().toString());
  } else {
    logLine("Failed to start Wi-Fi access point!");
  }

  server.on("/", HTTP_GET, []() {
    server.sendHeader("Cache-Control", "no-store");
    server.send_P(200, "text/html", PAGE);
  });
  server.on("/api/data", HTTP_GET, handleData);
  server.on("/api/log", HTTP_GET, handleLog);
  server.on("/api/cmd", HTTP_GET, handleCmd);
  server.on("/api/set", HTTP_GET, handleSet);

  server.onNotFound([]() {
    server.send(404, "text/plain", "Page not found");
  });

  server.begin();
  logLine("Web server started.");
}

// ==========================================
// SETUP
// ==========================================
void setup() {
  Serial.begin(115200);          // must come BEFORE any Serial.print
  delay(200);
  Serial.println();
  Serial.println("=== SPINESENSE BOOTING ===");

  pinMode(TOUCH_PIN, INPUT);
  pinMode(BUZZER_PIN, OUTPUT);
  stopBuzzer();

  // Start dashboard before calibration.
  startDashboard();

  // MPU6050 uses I2C: SDA = D2, SCL = D1
  Wire.begin(D2, D1);

  Wire.beginTransmission(MPU_ADDR);
  Wire.write(0x6B);
  Wire.write(0x00); // Wake MPU6050

  if (Wire.endTransmission() != 0) {
    sensorConnected = false;

    logLine("MPU6050 not detected! Check SDA, SCL, VCC and GND.");
    logLine("Dashboard remains available.");

    while (true) {
      server.handleClient();
      delay(2);
    }
  }

  sensorConnected = true;
  logLine("MPU6050 found at 0x68");

  if (!calibratePosition()) {
    logLine("Calibration failed. Check sensor.");

    while (true) {
      server.handleClient();
      delay(2);
    }
  }

  logLine("=== SPINESENSE READY ===");
  logLine("Short touch: snooze. Long hold + release: recalibrate.");
}

// ==========================================
// MAIN LOOP
// ==========================================
void loop() {
  server.handleClient();

  // Recalibration requested from the dashboard
  if (recalRequested) {
    calibratePosition();
  }

  unsigned long now = millis();

  // 1. READ MPU6050
  float ax, ay, az;
  bool validReading = readAccel(ax, ay, az);

  if (validReading) {
    sensorConnected = true;

    float rawAngle = calculateAngle(ay, az);

    if (!sensorReady) {
      filteredAngle = rawAngle;
      sensorReady = true;
    } else {
      filteredAngle = 0.85 * filteredAngle + 0.15 * rawAngle;
    }

    relativeAngle = angleDifference(filteredAngle, baselineAngle);
  } else {
    sensorConnected = false;
  }

  // 2. TOUCH SENSOR
  bool touched = digitalRead(TOUCH_PIN) == HIGH;
  touchedNow = touched;

  if (touched && !previousTouch) {
    touchStartTime = now;
    longPressHandled = false;
  }

  // Detect long hold
  if (touched && !longPressHandled && now - touchStartTime >= LONG_PRESS_DURATION) {
    longPressHandled = true;

    postureAlarm = false;
    postureCandidate = false;

    stopBuzzer();

    logLine("Long hold detected. Release to recalibrate.");
  }

  // Act when touch is released
  if (!touched && previousTouch) {
    unsigned long duration = now - touchStartTime;

    if (longPressHandled) {
      calibratePosition();
    } else if (duration < LONG_PRESS_DURATION) {
      applySnooze();
    }
  }

  previousTouch = touched;

  // 3. SNOOZE STATUS
  now = millis();

  bool snoozing = snoozeUntil != 0 && !timeReached(now, snoozeUntil);

  if (wasSnoozing && !snoozing) {
    snoozeUntil = 0;

    logLine("Snooze ended.");

    if (validReading && fabs(relativeAngle) >= POSTURE_THRESHOLD) {
      postureAlarm = true;
      postureCandidate = false;
      postureStartTime = 0;
      nextBeepTime = now;

      logLine("Posture still outside threshold!");
    }
  }

  wasSnoozing = snoozing;

  // 4. POSTURE DETECTION
  if (!snoozing && validReading) {
    float deviation = fabs(relativeAngle);

    if (postureAlarm) {
      if (deviation <= ORIGINAL_POSITION_RANGE) {
        postureAlarm = false;
        postureCandidate = false;
        postureStartTime = 0;

        logLine("Original position restored.");
      }
    } else {
      if (deviation >= POSTURE_THRESHOLD) {
        if (!postureCandidate) {
          postureCandidate = true;
          postureStartTime = now;

          logLine("Posture deviation detected.");
        }

        if (now - postureStartTime >= POSTURE_DELAY) {
          postureAlarm = true;
          postureCandidate = false;
          nextBeepTime = now;

          logLine("ALARM: POSTURE ALERT!");
        }
      } else {
        postureCandidate = false;
        postureStartTime = 0;
      }
    }
  }

  // 5. BUZZER
  if (buzzerOn && timeReached(now, buzzerOffTime)) {
    stopBuzzer();
  }

  if (snoozing || !validReading) {
    stopBuzzer();
  } else if (!buzzerOn && postureAlarm && timeReached(now, nextBeepTime)) {
    startBeep(now);
    nextBeepTime = now + BEEP_INTERVAL;
  }

  // 6. LIGHT SENSOR
  lightValue = analogRead(LDR_PIN);

  // 7. STATUS LINE (Serial Monitor + dashboard terminal)
  if (now - lastStatusPrint >= 1000) {
    lastStatusPrint = now;

    logLine("Angle diff: " + String(relativeAngle, 1) + " deg | Light: " + String(lightValue) +
            (lightValue > DIM_THRESHOLD ? " (dim)" : "") +
            " | Alarm: " + (postureAlarm ? "ON" : "OFF") +
            " | Sensor: " + (sensorConnected ? "OK" : "ERROR"));
  }

  // Let Wi-Fi requests be handled regularly.
  server.handleClient();
  delay(20);
}
