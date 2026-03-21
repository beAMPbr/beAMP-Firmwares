#include <Wire.h>
#include <WiFi.h>
#include <WebServer.h>
#include <WiFiManager.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <Update.h>           // Para o OTA Local
#include <HTTPClient.h>       // Para o OTA Cloud
#include <HTTPUpdate.h>       // Para o OTA Cloud
#include <WiFiClientSecure.h>

// --- VERSÃO DO SISTEMA ---
String FIRMWARE_VERSION = "1.0"; // Mude isto sempre que lançar uma atualização!

// ==========================================================
// CONFIGURAÇÃO DE HARDWARE (MIGRADO PARA ESP32-S3)
// ==========================================================
#define PIN_STBY 5       
#define PIN_RESET 0      
#define PIN_BT_RST 4     

#define I2C_SDA 8        
#define I2C_SCL 9        
#define DSP_I2C_ADDR 0x34

#define TELA_RX 10       
#define TELA_TX 11       

// --- ENDEREÇOS DO DSP ---
#define ADDR_Z1_SRC 0x0000
#define ADDR_Z1_VOL 0x0007
#define ADDR_Z2_SRC 0x0001
#define ADDR_Z2_VOL 0x000B
#define ADDR_Z3_SRC 0x0002
#define ADDR_Z3_VOL 0x000F
#define ADDR_Z4_SRC 0x0003
#define ADDR_Z4_VOL 0x0013

const uint16_t addr_bass[] = {0x0004, 0x0008, 0x000C, 0x0010};
const uint16_t addr_mid[]  = {0x0005, 0x0009, 0x000D, 0x0011};
const uint16_t addr_treb[] = {0x0006, 0x000A, 0x000E, 0x0012};

#define SAVE_DELAY 3000 

WebServer server(80);
Preferences preferences;
WiFiManager wm;

// --- VARIÁVEIS ---
float vol[4];
int src[4];
bool isMuted = false;
bool zoneMute[4] = {false, false, false, false};

String zoneName[4] = {"ZONA 1", "ZONA 2", "ZONA 3", "ZONA 4"};
String srcName[3] = {"LINE 1", "LINE 2", "BT"};

int eqBass[4] = {6, 6, 6, 6};
int eqMid[4]  = {6, 6, 6, 6};
int eqTreb[4] = {6, 6, 6, 6};

uint16_t addr_vol[] = {ADDR_Z1_VOL, ADDR_Z2_VOL, ADDR_Z3_VOL, ADDR_Z4_VOL};
uint16_t addr_src[] = {ADDR_Z1_SRC, ADDR_Z2_SRC, ADDR_Z3_SRC, ADDR_Z4_SRC};

bool needsToSave = false;
unsigned long lastChangeTime = 0;
bool isGlobalLocked = false;
unsigned long lockStartTime = 0;
unsigned long pressTime = 0;
bool isPressing = false;

String globalAmpName = "";
int screenTimeSec = 300;
int screenBrightness = 100;

// Variáveis de controle
bool portalRunning = false;
bool needsReboot = false;
bool isScreenSynced = false; // ---> NOVO: Variável que vigia o estado da sincronização!

// --- FUNÇÕES AUXILIARES ---
void applyHardwareStby(bool mute) {
  if (mute) {
    pinMode(PIN_STBY, OUTPUT); digitalWrite(PIN_STBY, LOW);
    Serial.println("[HW] STANDBY ATIVADO (Mute Fisico)");
  } else {
    pinMode(PIN_STBY, INPUT);
    Serial.println("[HW] STANDBY LIBERADO (Som ON)");
  }
}

void dspWriteInt(uint16_t address, int value) {
  if(address == 0xFFFF) return; 
  Wire.beginTransmission(DSP_I2C_ADDR);
  Wire.write((uint8_t)(address >> 8)); Wire.write((uint8_t)(address & 0xFF));
  Wire.write(0x00); Wire.write(0x00);
  Wire.write((uint8_t)(value >> 8)); Wire.write((uint8_t)(value & 0xFF));
  Wire.endTransmission();
}

void dspWriteVol(uint16_t address, float level) {
  if(address == 0xFFFF) return;
  if (level < 0.0) level = 0.0; if (level > 1.0) level = 1.0;

  // ---> A MAGIA DO VOLUME LOGARÍTMICO ACONTECE AQUI <---
  // Pega o valor linear (ex: 0.50) e eleva ao cubo (0.50 * 0.50 * 0.50 = 0.125)
  float logLevel = pow(level, 3.0f);

  // Calcula os bits para o chip usando a nova curva "amaciada"
  int32_t val = (int32_t)(logLevel * 8388608.0f);
  Wire.beginTransmission(DSP_I2C_ADDR);
  Wire.write((uint8_t)(address >> 8)); Wire.write((uint8_t)(address & 0xFF));
  Wire.write((uint8_t)((val >> 24) & 0xFF)); Wire.write((uint8_t)((val >> 16) & 0xFF));
  Wire.write((uint8_t)((val >> 8) & 0xFF)); Wire.write((uint8_t)(val & 0xFF));
  Wire.endTransmission();
}

void safeSourceChange(int z, int newSrc) {
  if (isGlobalLocked) return;
  isGlobalLocked = true; lockStartTime = millis();
  applyHardwareStby(true); delay(150); 
  dspWriteInt(addr_src[z], newSrc); src[z] = newSrc;
  delay(1200); 
  if (!isMuted) applyHardwareStby(false);
}

void wakeSystem() {
  Serial.println("[DSP] Acordando o sistema e chacoalhando DSP...");
  applyHardwareStby(true); delay(200);
  for(int i=0; i<4; i++) {
    // 1. Chacoalhada da Fonte
    int currentSrc = src[i];
    int tempSrc = (currentSrc == 2) ? 0 : 2; 
    dspWriteInt(addr_src[i], tempSrc);
    delay(30); 
    dspWriteInt(addr_src[i], currentSrc); 
    delay(30); 
    
    // 2. Chacoalhada dos Equalizadores (NOVO)
    int tBass = (eqBass[i] == 12) ? 11 : eqBass[i] + 1;
    dspWriteInt(addr_bass[i], tBass); dspWriteInt(addr_bass[i], eqBass[i]);
    
    int tMid = (eqMid[i] == 12) ? 11 : eqMid[i] + 1;
    dspWriteInt(addr_mid[i], tMid); dspWriteInt(addr_mid[i], eqMid[i]);
    
    int tTreb = (eqTreb[i] == 12) ? 11 : eqTreb[i] + 1;
    dspWriteInt(addr_treb[i], tTreb); dspWriteInt(addr_treb[i], eqTreb[i]);

    // 3. Chacoalhada do Volume
    if(!zoneMute[i]) {
        float tempVol = vol[i] - 0.05f;
        if (tempVol < 0.0f) tempVol = 0.0f;
        dspWriteVol(addr_vol[i], tempVol); 
        dspWriteVol(addr_vol[i], vol[i]);
    }
  }
  delay(800); if (!isMuted) applyHardwareStby(false);
}

void resetarBluetooth() {
  pinMode(PIN_BT_RST, OUTPUT); digitalWrite(PIN_BT_RST, HIGH);
  delay(1500); pinMode(PIN_BT_RST, INPUT);
}

// --- ESCUTA DA TELA ---
void ouvirTela() {
  // ---> MUDANÇA: 'while' em vez de 'if' processa TUDO instantaneamente e esvazia o buffer <---
  while (Serial2.available() > 0) { 
    String msg = Serial2.readStringUntil('\n');
    msg.trim(); 
    if (msg.startsWith("CMD:")) {
      int p1 = msg.indexOf(':'); int p2 = msg.indexOf(':', p1 + 1);
      int p3 = msg.indexOf(':', p2 + 1);
      if (p1 > 0 && p2 > 0 && p3 > 0) {
        String comando = msg.substring(p1 + 1, p2);
        String zonaStr = msg.substring(p2 + 1, p3); 
        int valor = msg.substring(p3 + 1).toInt();
        
        if (comando == "WAKE") { wakeSystem(); return; }
        else if (comando == "GMUTE") {
            isMuted = (valor == 1);
            applyHardwareStby(isMuted);
            needsToSave = true; lastChangeTime = millis(); 
            Serial2.printf("SYNC:GMUTE:ALL:%d\n", isMuted ? 1 : 0);
            return;
        }
        else if (comando == "BT_RST") { resetarBluetooth(); return; }
        
        // ---> ADICIONE ESTE BLOCO AQUI <---
        else if (comando == "RST_FAB") {
            Serial.println("!!! RESET DE FABRICA SOLICITADO PELA TELA !!!");
            wm.resetSettings(); 
            preferences.begin("beamp-quad", false);
            preferences.clear(); 
            preferences.end();
            ESP.restart();
            return;
        }
        // ----------------------------------
        
        int z = 0;
        if (zonaStr == "Z2") z = 1; else if (zonaStr == "Z3") z = 2;
        else if (zonaStr == "Z4") z = 3;

        if (comando == "GET_SYNC") {
            isScreenSynced = true; // ---> O GATILHO! A tela confirmou que está viva e a ouvir.
            
            Serial2.printf("SYNC:VOL:%s:%d\n", zonaStr.c_str(), (int)(vol[z]*100));
            Serial2.printf("SYNC:SRC:%s:%d\n", zonaStr.c_str(), src[z]);
            Serial2.printf("SYNC:ZMUTE:%s:%d\n", zonaStr.c_str(), zoneMute[z] ? 1 : 0);
            Serial2.printf("SYNC:BASS:%s:%d\n", zonaStr.c_str(), eqBass[z]);
            Serial2.printf("SYNC:MID:%s:%d\n", zonaStr.c_str(), eqMid[z]);
            Serial2.printf("SYNC:TREB:%s:%d\n", zonaStr.c_str(), eqTreb[z]);
            for(int i = 0; i < 4; i++) { Serial2.printf("SYNC:NAME:Z%d:%s\n", i + 1, zoneName[i].c_str()); }
            for(int i = 0; i < 3; i++) { Serial2.printf("SYNC:INP:%d:%s\n", i, srcName[i].c_str()); }
            
            Serial2.printf("SYNC:GMUTE:ALL:%d\n", isMuted ? 1 : 0);
            Serial2.printf("SYNC:TIME:ALL:%d\n", screenTimeSec * 1000);
            Serial2.printf("SYNC:BRIGHT:ALL:%d\n", screenBrightness);
            
            atualizarInfoRedeNaTela(); // Garante que também preenche WiFi logo ao nascer
        }
        else if (comando == "VOL") {
          vol[z] = valor / 100.0f;
          if(!zoneMute[z]) dspWriteVol(addr_vol[z], vol[z]); needsToSave = true; lastChangeTime = millis();
        } 
        else if (comando == "SRC") { safeSourceChange(z, valor);
          needsToSave = true; lastChangeTime = millis(); }
        else if (comando == "ZMUTE") {
          zoneMute[z] = (valor == 1);
          if(zoneMute[z]) dspWriteVol(addr_vol[z], 0.0); else dspWriteVol(addr_vol[z], vol[z]);
          needsToSave = true; lastChangeTime = millis();
        }
        else if (comando == "BASS") { eqBass[z] = valor; dspWriteInt(addr_bass[z], valor);
          needsToSave = true; lastChangeTime = millis(); }
        else if (comando == "MID") { eqMid[z] = valor;
          dspWriteInt(addr_mid[z], valor); needsToSave = true; lastChangeTime = millis(); }
        else if (comando == "TREB") { eqTreb[z] = valor;
          dspWriteInt(addr_treb[z], valor); needsToSave = true; lastChangeTime = millis(); }
      }
    }
  }
}

// --- RADAR DO WI-FI ---
void handleStatus() {
  String json = "{";
  json += "\"gm\":" + String(isMuted ? 1 : 0) + ",";
  for(int i=0; i<4; i++) {
    json += "\"v" + String(i) + "\":" + String((int)(vol[i]*100)) + ",";
    json += "\"s" + String(i) + "\":" + String(src[i]) + ",";
    json += "\"zm" + String(i) + "\":" + String(zoneMute[i]) + ",";
    json += "\"qb" + String(i) + "\":" + String(eqBass[i]) + ",";
    json += "\"qm" + String(i) + "\":" + String(eqMid[i]) + ",";
    json += "\"qt" + String(i) + "\":" + String(eqTreb[i]);
    if(i<3) json += ",";
  }
  json += "}";
  server.send(200, "application/json", json);
}

// --- INTERFACE WEB beAMP ---
void handleRoot() {
  String html = "<!DOCTYPE html><html lang='pt-br'><head><meta charset='UTF-8'><meta name='viewport' content='width=device-width,initial-scale=1'>";
  html += "<title>" + (globalAmpName != "" ? globalAmpName : String("beAMP QUAD")) + "</title>";
  html += "<style>";
  html += ":root { --red: #E31E24; --grey: #3A4A53; --dark: #121212; --card: #1E1E1E; --text: #E0E0E0; }";
  html += "body{font-family:'Segoe UI', Roboto, sans-serif; background:var(--dark); color:var(--text); margin:0; padding:20px; text-align:center;}";
  html += ".logo-area{margin-bottom:20px;} .logo-text{font-size:2em; font-weight:bold; color:var(--text);} span.brand-red{color:var(--red);}";
  html += ".slogan{font-size:0.9em; color:var(--grey); letter-spacing:1px; margin-top:-5px; display:block;}";
  html += ".master-controls{display:flex; gap:10px; justify-content:center; margin-bottom:25px; flex-wrap:wrap;}";
  html += ".btn-main{padding:15px 25px; border:none; border-radius:50px; font-weight:bold; cursor:pointer; font-size:1em; transition:0.2s; min-width:140px; display:flex; align-items:center; justify-content:center; gap:8px;}";
  html += ".btn-mute{background:#333; color:white; border:2px solid var(--red);}";
  html += ".btn-mute.active{background:var(--red); color:white; border-color:var(--red);}";
  html += ".btn-wake{background:var(--grey); color:white;}";
  html += ".card{background:var(--card); padding:20px; border-radius:15px; margin-bottom:20px; border:1px solid #333; box-shadow:0 4px 10px rgba(0,0,0,0.3); text-align:left;}";
  html += ".zone-header{display:flex; justify-content:space-between; align-items:center; margin-bottom:15px; border-bottom:1px solid #333; padding-bottom:10px;}";
  html += ".zone-title-group{display:flex; align-items:center; gap:10px; flex-grow:1;}";
  html += ".zone-name{color:var(--text); font-size:1.3em; font-weight:bold; width:100%;}";
  html += ".btn-z-mute{background:transparent; color:#666; border:1px solid #666; width:40px; height:40px; border-radius:50%; font-size:1.2em; cursor:pointer; flex-shrink:0; margin-left:10px;}";
  html += ".btn-z-mute.active{background:var(--red); color:white; border-color:var(--red);}";
  html += ".controls-row{display:flex; gap:10px; margin-bottom:15px;}";
  html += ".btn-src{flex:1; padding:12px; background:#2a2a2a; color:#888; border:none; border-radius:8px; font-weight:bold; cursor:pointer; font-size: 0.9em;}";
  html += ".btn-src.active{background:var(--red)!important; color:white!important;}";
  html += "input[type=range]{width:100%; height:6px; background:#333; border-radius:5px; outline:none; -webkit-appearance:none; margin:15px 0;}";
  html += "input[type=range]::-webkit-slider-thumb{-webkit-appearance:none; width:18px; height:18px; background:var(--red); border-radius:50%; cursor:pointer;}";
  html += ".eq-grid{display:grid; grid-template-columns:1fr 1fr 1fr; gap:10px; margin-top:10px;}";
  html += ".eq-control span{font-size:0.7em; display:block; color:#777; text-align:center;}";
  html += ".locked-ui{pointer-events:none; opacity:0.4; filter:grayscale(1); transition:0.3s;}";
  html += "button:disabled, input:disabled{opacity:0.4;cursor:not-allowed;}"; 
  html += "</style>";
  
  html += "<script>";
  html += "let timers={}; function set(p,v,z){ let key=p+z; clearTimeout(timers[key]); timers[key]=setTimeout(()=>{fetch('/set?z='+z+'&'+p+'='+v);}, 150); }";
  html += "function lock(){ document.body.classList.add('locked-ui'); setTimeout(()=>document.body.classList.remove('locked-ui'), 2000); }";
  html += "function setVol(v,z){ set('v',v,z); }";
  html += "function toggleMute(){ fetch('/mute').then(()=>location.reload()); }";
  html += "function wake(){ fetch('/wake').then(()=>alert('Comando de Despertar Enviado!')); }";
  html += "function toggleZMute(el,z){ lock(); let s=el.classList.toggle('active'); fetch('/set?z='+z+'&zm='+(s?1:0)); }";
  html += "function uiSrc(z,v,el){ lock(); document.querySelectorAll('.btn-src-'+z).forEach(b=>b.classList.remove('active')); el.classList.add('active'); fetch('/set?z='+z+'&s='+v); }";
  
  html += "setInterval(()=>{ if(document.querySelector(':active')) return; ";
  html += "fetch('/status').then(r=>r.json()).then(d=>{ ";
  html += "let mg=document.getElementById('btnMuteGlobal'); ";
  html += "if(mg){ if(d.gm==1){ mg.classList.add('active'); mg.innerHTML='🔇 UNMUTE'; }else{ mg.classList.remove('active'); mg.innerHTML='🔇 MUTE'; } } ";
  html += "for(let i=0; i<4; i++){ ";
  html += "let v=document.getElementById('vol'+i); if(v) v.value=d['v'+i]; ";
  html += "let qb=document.getElementById('qb'+i); if(qb) qb.value=d['qb'+i]; ";
  html += "let qm=document.getElementById('qm'+i); if(qm) qm.value=d['qm'+i]; ";
  html += "let qt=document.getElementById('qt'+i); if(qt) qt.value=d['qt'+i]; ";
  html += "let s0=document.getElementById('s'+i+'_0'); let s1=document.getElementById('s'+i+'_1'); let s2=document.getElementById('s'+i+'_2'); ";
  html += "if(s0 && s1 && s2){ s0.classList.remove('active'); s1.classList.remove('active'); s2.classList.remove('active'); ";
  html += "if(d['s'+i]==0) s0.classList.add('active'); else if(d['s'+i]==1) s1.classList.add('active'); else s2.classList.add('active'); } ";
  html += "let zm=document.getElementById('zmBtn'+i); ";
  html += "if(zm){ if(d['zm'+i]==1) zm.classList.add('active'); else zm.classList.remove('active'); } ";
  html += "} }).catch(e=>{}); }, 1500);";
  html += "</script></head><body>";

  html += "<div style='text-align: right;'><a href='/config' style='text-decoration:none; font-size:24px;'>⚙️</a></div>";
  
  //LOGOTIPO ....html += "<div class='logo-area'><img src='data:image/png;base64,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' style='max-width: 250px; height: auto;'></div>";
  html += "<div class='logo-area'><h1 style='color:white;'>be<span style='color:var(--red);'>AMP</span></h1></div>";

  html += "<div class='master-controls'>";
  String mClass = isMuted ? "active" : "";
  String mText = isMuted ? "UNMUTE" : "MUTE";
  html += "<button id='btnMuteGlobal' class='btn-main btn-mute "+mClass+"' onclick='toggleMute()'>🔇 "+mText+"</button>";
  html += "<button class='btn-main btn-wake' onclick='wake()'>⚡ ACORDAR</button>";
  html += "</div>";
  
  for(int i=0; i<4; i++) {
    html += "<div class='card'>";
    String zmClass = zoneMute[i] ? "active" : "";
    html += "<div class='zone-header'>";
    html += "<div class='zone-title-group'>";
    html += "<div class='zone-name'>"+zoneName[i]+"</div>";
    html += "</div>";
    
    html += "<button id='zmBtn"+String(i)+"' class='btn-z-mute "+zmClass+"' onclick='toggleZMute(this,"+String(i)+")'>🔇</button>";
    html += "</div>";

    html += "<div class='controls-row'>";
    html += "<button id='s"+String(i)+"_0' class='btn-src btn-src-"+String(i)+" "+(src[i]==0?"active":"")+"' onclick='uiSrc("+String(i)+",0,this)'>"+srcName[0]+"</button>";
    html += "<button id='s"+String(i)+"_1' class='btn-src btn-src-"+String(i)+" "+(src[i]==1?"active":"")+"' onclick='uiSrc("+String(i)+",1,this)'>"+srcName[1]+"</button>";
    html += "<button id='s"+String(i)+"_2' class='btn-src btn-src-"+String(i)+" "+(src[i]==2?"active":"")+"' onclick='uiSrc("+String(i)+",2,this)'>"+srcName[2]+"</button></div>";
    
    html += "<div style='font-size:0.8em; color:#aaa; font-weight:bold;'>VOLUME</div>";
    html += "<input id='vol"+String(i)+"' type='range' min='0' max='100' value='"+String((int)(vol[i]*100))+"' oninput='setVol(this.value,"+String(i)+")'>";
    html += "<div style='font-size:0.8em; color:#aaa; font-weight:bold; margin-top:10px;'>EQUALIZAÇÃO</div>";
    
    html += "<div class='eq-grid'>";
    html += "<div class='eq-control'><input id='qb"+String(i)+"' type='range' min='0' max='12' value='"+String(eqBass[i])+"' oninput='set(\"qb\",this.value,"+String(i)+")'><span>BASS</span></div>";
    html += "<div class='eq-control'><input id='qm"+String(i)+"' type='range' min='0' max='12' value='"+String(eqMid[i])+"' oninput='set(\"qm\",this.value,"+String(i)+")'><span>MID</span></div>";
    html += "<div class='eq-control'><input id='qt"+String(i)+"' type='range' min='0' max='12' value='"+String(eqTreb[i])+"' oninput='set(\"qt\",this.value,"+String(i)+")'><span>TREBLE</span></div>";
    html += "</div></div>";
  }

  html += "</body></html>";
  server.send(200, "text/html", html);
}

// ==========================================================
// PÁGINA DE CONFIGURAÇÕES
// ==========================================================
void handleConfig() {
  String html = "<!DOCTYPE html><html lang='pt-br'><head><meta charset='UTF-8'><meta name='viewport' content='width=device-width, initial-scale=1.0'>";
  html += "<title>beAMP - Configurações</title><style>";
  html += "body{background-color:#121212;color:#ffffff;font-family:'Segoe UI',sans-serif;margin:0;padding:20px;}";
  html += ".card{background-color:#1e1e1e;border-radius:12px;padding:20px;margin-bottom:20px;border:1px solid #333;}";
  html += "h2{margin-top:0;font-size:1.2rem;color:#E31E24;border-bottom:1px solid #333;padding-bottom:10px;}";
  html += "input[type=text],input[type=number],select{width:100%;padding:12px;background:#2c2c2c;border:1px solid #444;color:#fff;border-radius:6px;box-sizing:border-box;font-size:1rem;margin-top:5px;margin-bottom:15px;}";
  html += "input[type=range]{width:100%; height:6px; background:#333; border-radius:5px; outline:none; -webkit-appearance:none; margin:15px 0;}";
  html += "input[type=range]::-webkit-slider-thumb{-webkit-appearance:none; width:18px; height:18px; background:#E31E24; border-radius:50%; cursor:pointer;}";
  html += ".btn-save{background:#E31E24;color:white;border:none;padding:15px;width:100%;border-radius:6px;font-size:1.1rem;font-weight:bold;cursor:pointer;}";
  html += ".header{display:flex;align-items:center;margin-bottom:20px;}";
  html += ".header a{color:#E31E24;text-decoration:none;font-size:24px;margin-right:15px;}";
  html += "</style></head><body><div style='max-width:600px;margin:0 auto;'>";
  
  html += "<div class='header'><a href='/'>⬅ Voltar</a><h1 style='margin:0;'>Configurações</h1></div>";
  
  html += "<div class='card'><h2>Nomes das Zonas</h2>";
  html += "<label>Zona 1</label><input type='text' id='z1' maxlength='15'>";
  html += "<label>Zona 2</label><input type='text' id='z2' maxlength='15'>";
  html += "<label>Zona 3</label><input type='text' id='z3' maxlength='15'>";
  html += "<label>Zona 4</label><input type='text' id='z4' maxlength='15'></div>";
  
  html += "<div class='card'><h2>Nomes das Entradas</h2>";
  html += "<label>Entrada 1 (Line 1)</label><input type='text' id='in0' maxlength='15'>";
  html += "<label>Entrada 2 (Line 2)</label><input type='text' id='in1' maxlength='15'>";
  html += "<label>Entrada 3 (Bluetooth)</label><input type='text' id='in2' maxlength='15'></div>";
  
  html += "<div class='card'><h2>Tela do Amplificador</h2>";
  html += "<label>Desligar tela após inatividade (segundos)</label>";
  html += "<input type='number' id='time' min='0' max='3600' placeholder='Ex: 120'>";
  html += "<p style='font-size:0.8em; color:#888;'>Digite 0 para a tela nunca desligar.</p>";
  html += "<label style='margin-top:15px; display:block;'>Brilho da Tela (%)</label>";
  html += "<input type='range' id='bright' min='10' max='100' value='100' oninput='document.getElementById(\"brVal\").innerText=this.value+\"%\";'>";
  html += "<div id='brVal' style='text-align:center; font-size:0.9em; color:#aaa; font-weight:bold;'>100%</div>";
  html += "</div>";

  html += "<div class='card'><h2>Bluetooth</h2>";
  html += "<p style='font-size:0.8em; color:#888;'>Se o Bluetooth travar ou não conectar, force a reinicialização do módulo.</p>";
  html += "<button onclick='resetBT()' style='width:100%; background:#2c2c2c; color:white; border:1px solid #444; padding:12px; border-radius:6px; font-size:1rem; cursor:pointer; font-weight:bold;'>🔄 Reiniciar Módulo Bluetooth</button>";
  html += "</div>";
  
  html += "<div class='card' style='border: 1px solid #E31E24;'>";
  html += "<h2 style='color:#E31E24;'>Avançado - Sistema</h2>";
  html += "<label>Nome Wi-Fi / Título</label><input id='sysName' type='text' maxlength='20'>";
  html += "<p style='font-size:0.8em; color:#888;'>Alterar a identidade fará o sistema reiniciar.</p>";
  
  html += "<hr style='border:1px solid #333; margin: 20px 0;'>";
  html += "<h2 style='color:#E31E24; margin-top:0;'>Reset de Fábrica</h2>";
  html += "<p style='font-size:0.8em; color:#888;'>Apaga todas as configurações, incluindo a rede Wi-Fi salva.</p>";
  html += "<button onclick='factoryReset()' style='width:100%; background:transparent; color:#E31E24; border:2px solid #E31E24; padding:12px; border-radius:6px; font-size:1rem; cursor:pointer; font-weight:bold;'>⚠️ Restaurar Padrões de Fábrica</button>";
  html += "</div>";

  // ==========================================
  // BOTÃO DE ATUALIZAÇÃO CLOUD OTA
  // ==========================================
  html += "<div class='card'><h2 style='color:#E31E24;'>Manutenção</h2>";
  html += "<p style='font-size:0.8em; color:#888;'>Verifique se há atualizações de sistema disponíveis na nuvem.</p>";
  html += "<a href='/update_cloud' style='display:block; background:#444; color:#fff; text-align:center; padding:15px; border-radius:6px; text-decoration:none; font-weight:bold; font-size:1.1rem; margin-bottom:5px;'>☁️ Buscar Atualização na Nuvem</a>";
  html += "</div>";

  html += "<button class='btn-save' onclick='salvar()'>Salvar Configurações</button><div id='msg' style='text-align:center;margin-top:15px;font-weight:bold;'></div></div>";
  
  html += "<script>";
  html += "window.onload=()=>{fetch('/api/get_conf').then(r=>r.json()).then(d=>{";
  html += "document.getElementById('z1').value=d.z1; document.getElementById('z2').value=d.z2;";
  html += "document.getElementById('z3').value=d.z3; document.getElementById('z4').value=d.z4;";
  html += "document.getElementById('in0').value=d.i0; document.getElementById('in1').value=d.i1;";
  html += "document.getElementById('in2').value=d.i2; document.getElementById('time').value=d.t;";
  html += "document.getElementById('bright').value=d.b; document.getElementById('brVal').innerText=d.b+'%';";
  html += "document.getElementById('sysName').value=d.sys;";
  html += "});};";
  html += "function resetBT(){ if(confirm('Forçar reinicialização do módulo Bluetooth?')){ fetch('/api/bt_rst').then(()=>alert('Comando Enviado!')); } }";
  html += "function factoryReset(){ if(confirm('ATENÇÃO! Isso apagará TODAS as configurações e a rede Wi-Fi atual.\\n\\nDeseja realmente continuar?')){ fetch('/api/factory_reset', {method:'POST'}).then(()=>{ alert('Reiniciando... Conecte-se à rede beAMP QUAD em alguns segundos.'); }); } }";
  html += "function salvar(){";
  html += "let p = new URLSearchParams();";
  html += "p.append('z1',document.getElementById('z1').value.toUpperCase()); p.append('z2',document.getElementById('z2').value.toUpperCase());";
  html += "p.append('z3',document.getElementById('z3').value.toUpperCase()); p.append('z4',document.getElementById('z4').value.toUpperCase());";
  html += "p.append('in0',document.getElementById('in0').value.toUpperCase()); p.append('in1',document.getElementById('in1').value.toUpperCase());";
  html += "p.append('in2',document.getElementById('in2').value.toUpperCase()); p.append('time',document.getElementById('time').value);";
  html += "p.append('br',document.getElementById('bright').value);";
  html += "p.append('sys',document.getElementById('sysName').value);";
  html += "document.getElementById('msg').innerText='A guardar...'; document.getElementById('msg').style.color='#aaa';";
  html += "fetch('/api/set_conf',{method:'POST',body:p}).then(r=>{if(r.ok){";
  html += "document.getElementById('msg').innerText='Salvas com sucesso!';document.getElementById('msg').style.color='#4CAF50';";
  html += "if(r.headers.get('x-reboot')==='1'){ setTimeout(()=>location.reload(), 6000); }";
  html += "}else{document.getElementById('msg').innerText='Erro!';document.getElementById('msg').style.color='red';}});";
  html += "}";
  html += "</script></body></html>";
  
  server.send(200, "text/html", html);
}

void handleGetConf() {
  String json = "{";
  json += "\"z1\":\"" + zoneName[0] + "\",\"z2\":\"" + zoneName[1] + "\",";
  json += "\"z3\":\"" + zoneName[2] + "\",\"z4\":\"" + zoneName[3] + "\",";
  json += "\"i0\":\"" + srcName[0] + "\",\"i1\":\"" + srcName[1] + "\",";
  json += "\"i2\":\"" + srcName[2] + "\",\"t\":" + String(screenTimeSec) + ",";
  json += "\"b\":" + String(screenBrightness) + ",";
  json += "\"sys\":\"" + globalAmpName + "\"";
  json += "}";
  server.send(200, "application/json", json);
}

void handleSetConf() {
  bool willRestart = false;

  if (server.hasArg("z1")) zoneName[0] = server.arg("z1");
  if (server.hasArg("z2")) zoneName[1] = server.arg("z2");
  if (server.hasArg("z3")) zoneName[2] = server.arg("z3");
  if (server.hasArg("z4")) zoneName[3] = server.arg("z4");
  if (server.hasArg("in0")) srcName[0] = server.arg("in0");
  if (server.hasArg("in1")) srcName[1] = server.arg("in1");
  if (server.hasArg("in2")) srcName[2] = server.arg("in2");
  if (server.hasArg("time")) screenTimeSec = server.arg("time").toInt();
  if (server.hasArg("br")) screenBrightness = server.arg("br").toInt();

  if (server.hasArg("sys")) {
      String newSys = server.arg("sys");
      if(newSys != globalAmpName && newSys != "") {
          globalAmpName = newSys;
          preferences.begin("beamp-quad", false);
          preferences.putString("friendlyName", globalAmpName);
          preferences.end();
          willRestart = true;
      }
  }

  needsToSave = true; 
  lastChangeTime = millis();

  for(int i = 0; i < 4; i++) { Serial2.printf("SYNC:NAME:Z%d:%s\n", i + 1, zoneName[i].c_str()); }
  for(int i = 0; i < 3; i++) { Serial2.printf("SYNC:INP:%d:%s\n", i, srcName[i].c_str()); }
  Serial2.printf("SYNC:TIME:ALL:%d\n", screenTimeSec * 1000);
  Serial2.printf("SYNC:BRIGHT:ALL:%d\n", screenBrightness);

  if(willRestart) {
      server.sendHeader("x-reboot", "1");
      server.send(200, "text/plain", "OK");
      delay(1000);
      ESP.restart();
  } else {
      server.send(200, "text/plain", "OK");
  }
}

void handleFactoryReset() {
  Serial.println("!!! RESET DE FABRICA VIA WEB !!!");
  server.send(200, "text/plain", "OK");
  delay(1000);
  wm.resetSettings(); 
  preferences.begin("beamp-quad", false);
  preferences.clear(); 
  preferences.end();
  ESP.restart();
}

void handleRenameSrc() {
  if (!server.hasArg("i") || !server.hasArg("n")) return;
  int idx = server.arg("i").toInt();
  if(idx >= 0 && idx < 3) {
      srcName[idx] = server.arg("n");
      needsToSave = true; lastChangeTime = millis();
      Serial2.printf("SYNC:INP:%d:%s\n", idx, srcName[idx].c_str());
  }
  server.send(200, "text/plain", "OK");
}

void handleSetName() {
  if (server.hasArg("n")) {
    String newName = server.arg("n");
    Serial.println("[SYS] Novo Nome de Batismo Recebido: " + newName);
    preferences.begin("beamp-quad", false);
    preferences.putString("friendlyName", newName);
    preferences.end();
    server.send(200, "text/plain", "OK");
    delay(1000);
    ESP.restart();
  } else {
    server.send(400, "text/plain", "Falta argumento n");
  }
}

void handleMute() { 
  isMuted = !isMuted;
  Serial.print("[CMD] Mute Geral alterado para: "); Serial.println(isMuted);
  applyHardwareStby(isMuted);
  needsToSave = true; lastChangeTime = millis();
  Serial2.printf("SYNC:GMUTE:ALL:%d\n", isMuted ? 1 : 0);
  server.send(200, "text/plain", "OK");
}

void handleWake() { 
  Serial.println("[CMD] Comando Wake Recebido");
  wakeSystem();
  server.send(200, "text/plain", "OK");
}

void handleSet() {
  if (!server.hasArg("z")) return;
  int z = server.arg("z").toInt();
  if (server.hasArg("s")) { 
    int val = server.arg("s").toInt();
    Serial.print("[CMD] Z"); Serial.print(z); Serial.print(" Fonte: "); Serial.println(val);
    safeSourceChange(z, val);
    needsToSave = true; lastChangeTime = millis();
    Serial2.printf("SYNC:SRC:Z%d:%d\n", z + 1, val);
  }
  
  if (server.hasArg("v") && !isGlobalLocked) { 
    int volInt = server.arg("v").toInt();
    vol[z] = volInt / 100.0f; 
    Serial.print("[CMD] Z"); Serial.print(z);
    Serial.print(" Vol: "); Serial.println(vol[z]);
    if(!zoneMute[z]) dspWriteVol(addr_vol[z], vol[z]); needsToSave = true;
    lastChangeTime = millis();
    Serial2.printf("SYNC:VOL:Z%d:%d\n", z + 1, volInt);
  }
  
  if (server.hasArg("zm")) { 
    zoneMute[z] = (server.arg("zm").toInt() == 1);
    Serial.print("[CMD] Z"); Serial.print(z);
    Serial.print(" Mute: "); Serial.println(zoneMute[z]);
    if(zoneMute[z]) dspWriteVol(addr_vol[z], 0.0); else dspWriteVol(addr_vol[z], vol[z]); needsToSave = true; lastChangeTime = millis();
    Serial2.printf("SYNC:ZMUTE:Z%d:%d\n", z + 1, zoneMute[z] ? 1 : 0);
  }
  
  if (server.hasArg("qb")) { 
    eqBass[z] = server.arg("qb").toInt();
    if(eqBass[z] > 12) eqBass[z] = 12; if(eqBass[z] < 0) eqBass[z] = 0;
    dspWriteInt(addr_bass[z], eqBass[z]); needsToSave = true; lastChangeTime = millis();
    Serial2.printf("SYNC:BASS:Z%d:%d\n", z + 1, eqBass[z]); 
  }
  if (server.hasArg("qm")) { 
    eqMid[z] = server.arg("qm").toInt();
    if(eqMid[z] > 12) eqMid[z] = 12; if(eqMid[z] < 0) eqMid[z] = 0;
    dspWriteInt(addr_mid[z], eqMid[z]); needsToSave = true; lastChangeTime = millis();
    Serial2.printf("SYNC:MID:Z%d:%d\n", z + 1, eqMid[z]); 
  }
  if (server.hasArg("qt")) { 
    eqTreb[z] = server.arg("qt").toInt();
    if(eqTreb[z] > 12) eqTreb[z] = 12; if(eqTreb[z] < 0) eqTreb[z] = 0;
    dspWriteInt(addr_treb[z], eqTreb[z]); needsToSave = true; lastChangeTime = millis();
    Serial2.printf("SYNC:TREB:Z%d:%d\n", z + 1, eqTreb[z]); 
  }
  
  server.send(200, "text/plain", "OK");
}

void setup() {
  applyHardwareStby(true); 
  pinMode(PIN_BT_RST, INPUT);
  pinMode(PIN_RESET, INPUT_PULLUP);
  Serial.begin(115200);

  Serial2.begin(115200, SERIAL_8N1, TELA_RX, TELA_TX);
  Serial.setTimeout(20);
  
  Serial.println("\n\n--- INICIANDO beAMP QUAD (ESP32-S3) ---");
  
  Wire.begin(I2C_SDA, I2C_SCL);
  Wire.setClock(40000);
  Serial.println("Aguardando DSP Wondom JAB5 iniciar...");
  delay(3500); 

  Wire.beginTransmission(DSP_I2C_ADDR);
  Wire.endTransmission();
  delay(100);

  preferences.begin("beamp-quad", false);
  isMuted = preferences.getBool("isMuted", false);
  
  globalAmpName = preferences.getString("friendlyName", "");
  
  srcName[0] = preferences.getString("in0", "LINE 1");
  srcName[1] = preferences.getString("in1", "LINE 2");
  srcName[2] = preferences.getString("in2", "BT");
  
  screenTimeSec = preferences.getInt("screenTime", 300);
  screenBrightness = preferences.getInt("brightness", 100);
  
  Serial.println("Carregando Preferências...");
  for(int i=0; i<4; i++) {
    vol[i] = preferences.getFloat(("v"+String(i)).c_str(), 0.3);
    src[i] = preferences.getInt(("s"+String(i)).c_str(), 0);
    zoneName[i] = preferences.getString(("n"+String(i)).c_str(), "ZONA "+String(i+1));
    zoneMute[i] = preferences.getBool(("zm"+String(i)).c_str(), false);
    
    eqBass[i] = preferences.getInt(("qb"+String(i)).c_str(), 6);
    eqMid[i]  = preferences.getInt(("qm"+String(i)).c_str(), 6);
    eqTreb[i] = preferences.getInt(("qt"+String(i)).c_str(), 6);
  }
  preferences.end();

  Serial.println("Enviando config para DSP...");
  for(int i=0; i<4; i++) {
    dspWriteVol(addr_vol[i], 0.0);
    dspWriteInt(addr_src[i], src[i]);
    delay(50);
    dspWriteInt(addr_bass[i], eqBass[i]);
    dspWriteInt(addr_mid[i], eqMid[i]);
    dspWriteInt(addr_treb[i], eqTreb[i]);
    delay(50);
    if(!zoneMute[i]) dspWriteVol(addr_vol[i], vol[i]);
  }

  std::vector<const char *> menu = {"wifi", "sep", "close"};
  wm.setMenu(menu);
  wm.setTitle("beAMP - Configuração Wi-Fi");
  
  wm.setCustomHeadElement(
    "<style>"
    "body{background:#121212!important;color:#E0E0E0!important;font-family:sans-serif;text-align:center;}"
    "h1{display:none;} " 
    
    "h2, h3 {text-align:center!important; margin-top:20px; color:#ffffff!important; font-size:1.5em;}" 
    "div.msg { text-align:center!important; font-weight:bold; font-size:1.2em; margin-bottom:15px; }"
    
    "div.wrap form button, div.wrap form input[type='submit'], div.wrap button {"
      "background:#E31E24!important; border:none!important; border-radius:50px!important;"
      "padding:15px!important; font-weight:bold!important; color:white!important;"
      "width:100%; max-width:300px; margin:5px auto; cursor:pointer; display:block;"
    "}"
    
    "div.wrap a.b {"
      "background:#E31E24!important; border-radius:50px!important; color:white!important;"
      "display:inline-block; padding:15px; margin:5px auto; width:100%; max-width:300px; box-sizing:border-box; text-align:center;"
    "}"
    
    "input[type='text'], input[type='password'] {background:#ffffff!important;color:#121212!important;border-radius:8px!important;border:1px solid #cccccc!important;padding:10px; width:100%; max-width:300px; box-sizing:border-box; margin:5px auto; display:block;}"
    
    "input[type='checkbox'] {width:auto!important; display:inline-block!important; margin-right:8px!important; transform:scale(1.2);}"
    
    "a{color:#ffffff!important;text-decoration:none;font-weight:bold;}" 
    
    "div,p,span,label{color:#E0E0E0!important;}"
    
    "svg, .q { filter: invert(1) brightness(200%)!important; }" 
    "</style>"
    
    "<script>"
    "window.addEventListener('DOMContentLoaded', function() {"
      "function rep(n) {"
        "if (n.nodeType === 3) {"
          "n.nodeValue = n.nodeValue.replace('Configure WiFi', 'Configurar Wi-Fi')"
                                   ".replace('Close', 'Fechar')"
                                   ".replace('Show Password', 'Mostrar Senha')"
                                   ".replace('show password', 'Mostrar Senha')"
                                   ".replace('Show password', 'Mostrar Senha')"
                                   ".replace('Password', 'Senha')"
                                   ".replace('Save', 'Salvar')"
                                   ".replace('Refresh', 'Atualizar')"
                                   ".replace('Saving Credentials', 'Salvando as configurações...')"
                                   ".replace('Trying to connect ESP to network.', 'Conectando o beAMP à sua rede Wi-Fi.')"
                                   /* ---> A SUA NOVA TRADUÇÃO ESTÁ AQUI <--- */
                                   ".replace('If it fails reconnect to AP to try again', 'Em caso de falha, conecte-se ao beAMP novamente para tentar de novo.');"
        "} else if (n.nodeType === 1 && n.tagName !== 'SCRIPT' && n.tagName !== 'STYLE') {"
          "for (var i = 0; i < n.childNodes.length; i++) rep(n.childNodes[i]);"
        "}"
      "}"
      "rep(document.body);"
      "var p = document.querySelector('input[name=\"p\"]'); if(p) p.placeholder='';" 
      
      "document.querySelectorAll('div').forEach(function(d){"
        "if(d.innerText.trim() === 'no AP set' || d.innerText.trim() === 'No AP set') d.style.display = 'none';"
      "});"
      
      "document.querySelectorAll('path').forEach(function(p){"
         "if(p.getAttribute('fill') === '#000000') p.setAttribute('fill', '#ffffff');"
      "});"
      
    "});"
    "</script>"
    
    /* ---> LOGOTIPO A LINHA 1 DA SUA IMAGEM ENTROU AQUI <--- */
    //"<div style='margin: 20px 0;'><img src='data:image/png;base64,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' style='max-width: 250px; height: auto;'></div>"
    "<div style='margin: 20px 0;'><h1 style='color:white;'>be<span style='color:#E31E24;'>AMP</span></h1></div>"
  );
  
  WiFi.mode(WIFI_STA); 
  WiFi.begin();
  delay(200);   
  
  String mac = WiFi.macAddress();
  Serial.println("[WIFI] MAC Address Cru: " + mac); 
  
  mac.replace(":", "");
  String idUnico = mac.substring(8); 
  
  if(globalAmpName == "") {
      globalAmpName = "beAMP_" + idUnico;
  }
  
  String nomeAP = globalAmpName;
  String nomeMDNS = globalAmpName;
  nomeMDNS.replace(" ", "-");
  nomeMDNS.toLowerCase();
  
  Serial.println("[WIFI] Identidade Unica Gerada: " + nomeAP);

  wm.setSaveConfigCallback([](){
      Serial.println("[WIFI] Configuracoes salvas pelo cliente. O sistema vai reiniciar!");
      needsReboot = true;
  });
  
  wm.setConfigPortalBlocking(false);
  
  if(wm.autoConnect(nomeAP.c_str())) {
      Serial.println("[WIFI] Conectado a rede!");
      
      if (MDNS.begin(nomeMDNS.c_str())) 
        MDNS.addService("http", "tcp", 80);
      
      server.on("/", handleRoot);
      server.on("/status", handleStatus); 
      server.on("/set", handleSet);
      server.on("/mute", handleMute);
      server.on("/wake", handleWake);
      server.on("/setname", handleSetName); 
      server.on("/rensrc", handleRenameSrc);
      server.on("/config", handleConfig);
      server.on("/api/get_conf", handleGetConf);
      server.on("/api/set_conf", HTTP_POST, handleSetConf);
      server.on("/api/bt_rst", []() { 
          resetarBluetooth(); 
          server.send(200, "text/plain", "OK"); 
      });
      server.on("/api/factory_reset", HTTP_POST, handleFactoryReset);
      
      // ==========================================================
      // 1. ROTA DE ATUALIZAÇÃO OTA LOCAL (ESCONDIDA: /update)
      // ==========================================================
      server.on("/update", HTTP_GET, []() {
        server.sendHeader("Connection", "close");
        String htmlOTA = "<!DOCTYPE html><html><head><meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'><title>beAMP - Sistema Técnico</title>";
        htmlOTA += "<style>body{background:#121212;color:#E0E0E0;font-family:sans-serif;text-align:center;padding:50px;} form{background:#1e1e1e;padding:30px;border-radius:10px;display:inline-block;} input[type=file]{margin-bottom:20px;} input[type=submit]{background:#E31E24;color:white;border:none;padding:10px 20px;border-radius:20px;font-weight:bold;cursor:pointer;}</style></head>";
        htmlOTA += "<body><h2>beAMP Firmware Update (TESTE GITHUB)</h2><form method='POST' action='/update' enctype='multipart/form-data'>";
        htmlOTA += "<input type='file' name='update' accept='.bin'><br><br><input type='submit' value='Instalar Ficheiro'></form></body></html>";
        server.send(200, "text/html", htmlOTA);
      });

      server.on("/update", HTTP_POST, []() {
        server.sendHeader("Connection", "close");
        String successHTML = "<!DOCTYPE html><html><head><meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'><meta http-equiv='refresh' content='10; url=/'><title>beAMP - Status</title>";
        successHTML += "<style>body{background:#121212;color:#E0E0E0;font-family:sans-serif;text-align:center;padding:40px 20px;margin:0;} .card{background:#1e1e1e;padding:40px 30px;border-radius:12px;display:inline-block;box-shadow:0 8px 20px rgba(0,0,0,0.6);max-width:400px;width:90%;box-sizing:border-box;} h2{color:#E31E24;margin-top:0;font-size:1.5rem;} p{color:#aaa;line-height:1.5;} .loader{border:4px solid #333;border-top:4px solid #E31E24;border-radius:50%;width:40px;height:40px;animation:spin 1s linear infinite;margin:20px auto;} @keyframes spin{0%{transform:rotate(0deg);}100%{transform:rotate(360deg);}}</style></head>";
        successHTML += "<body><div class='card'><h2>" + String(Update.hasError() ? "Falha na Atualizacao!" : "Atualizacao Concluida!") + "</h2>";
        if(!Update.hasError()) {
            successHTML += "<div class='loader'></div><p>A reiniciar o sistema.</p><p style='font-size:0.9em;color:#777;'>Voltara automaticamente...</p>";
        } else {
            successHTML += "<p>Erro ao gravar o arquivo.</p>";
        }
        successHTML += "</div></body></html>";
        
        server.send(200, "text/html", successHTML);
        delay(1000);
        ESP.restart();
      }, []() {
        HTTPUpload& upload = server.upload();
        if (upload.status == UPLOAD_FILE_START) {
          if (!Update.begin(UPDATE_SIZE_UNKNOWN)) Update.printError(Serial);
        } else if (upload.status == UPLOAD_FILE_WRITE) {
          if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) Update.printError(Serial);
        } else if (upload.status == UPLOAD_FILE_END) {
          if (Update.end(true)) Serial.printf("OTA Concluido: %u bytes\n", upload.totalSize);
        }
      });

      // ==========================================================
      // 2. ROTA DE ATUALIZAÇÃO VIA NUVEM (COM CONTROLO DE VERSÃO)
      // ==========================================================
      server.on("/update_cloud", HTTP_GET, []() {
        // 1. Prepara a ligação ao GitHub para ler o ficheiro de versão
        WiFiClientSecure client;
        client.setInsecure();
        HTTPClient http;
        
        // Link do ficheiro de texto
        String urlVersao = "https://raw.githubusercontent.com/beAMPbr/beAMP-Firmwares/refs/heads/main/QUAD/versao.txt";
        http.begin(client, urlVersao);
        int httpCode = http.GET();
        
        if (httpCode == HTTP_CODE_OK) {
            String versaoNuvem = http.getString();
            versaoNuvem.trim(); // Limpa espaços e quebras de linha invisíveis
            
            // 2. Compara as versões
            if (versaoNuvem != FIRMWARE_VERSION) {
                // --- TEM VERSÃO NOVA! MOSTRA ECRÃ DE CARREGAMENTO E INICIA ---
                String loadingHTML = "<!DOCTYPE html><html><head><meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'><meta http-equiv='refresh' content='40; url=/'><title>beAMP - Atualizando</title>";
                loadingHTML += "<style>body{background:#121212;color:#E0E0E0;font-family:sans-serif;text-align:center;padding:40px 20px;margin:0;} .card{background:#1e1e1e;padding:40px 30px;border-radius:12px;display:inline-block;box-shadow:0 8px 20px rgba(0,0,0,0.6);max-width:400px;width:90%;box-sizing:border-box;} h2{color:#E31E24;margin-top:0;font-size:1.5rem;} p{color:#aaa;line-height:1.5;} .loader{border:4px solid #333;border-top:4px solid #E31E24;border-radius:50%;width:40px;height:40px;animation:spin 1s linear infinite;margin:20px auto;} @keyframes spin{0%{transform:rotate(0deg);}100%{transform:rotate(360deg);}}</style></head>";
                loadingHTML += "<body><div class='card'><h2>Atualizacao em Curso</h2><div class='loader'></div><p>A descarregar a versao <b>" + versaoNuvem + "</b> da nuvem.</p><p style='font-size:0.9em;color:#777;'>Por favor, aguarde cerca de 1 minuto...</p></div></body></html>";
                server.send(200, "text/html", loadingHTML);
                
                delay(1000); 
                
                // Link do ficheiro Binário
                String urlBin = "https://raw.githubusercontent.com/beAMPbr/beAMP-Firmwares/refs/heads/main/QUAD/MESTRE_ESP32S3_V1.ino.bin";
                httpUpdate.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
                t_httpUpdate_return ret = httpUpdate.update(client, urlBin);
                
                if(ret == HTTP_UPDATE_FAILED) {
                    Serial.printf("Erro Cloud OTA: %s\n", httpUpdate.getLastErrorString().c_str());
                }
            } else {
                // --- JÁ ESTÁ ATUALIZADO! MOSTRA ECRÃ VERDE ---
                String okHTML = "<!DOCTYPE html><html><head><meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'><meta http-equiv='refresh' content='5; url=/config'><title>beAMP - Status</title>";
                okHTML += "<style>body{background:#121212;color:#E0E0E0;font-family:sans-serif;text-align:center;padding:40px 20px;margin:0;} .card{background:#1e1e1e;padding:40px 30px;border-radius:12px;display:inline-block;box-shadow:0 8px 20px rgba(0,0,0,0.6);max-width:400px;width:90%;box-sizing:border-box;} h2{color:#4CAF50;margin-top:0;font-size:1.5rem;} p{color:#aaa;line-height:1.5;}</style></head>";
                okHTML += "<body><div class='card'><h2>Sistema Atualizado!</h2><p>O seu beAMP ja possui a versao mais recente (<b>" + FIRMWARE_VERSION + "</b>).</p><p style='font-size:0.9em;color:#777;'>A voltar as configuracoes...</p></div></body></html>";
                server.send(200, "text/html", okHTML);
            }
        } else {
            // --- ERRO AO LIGAR AO GITHUB ---
            String errHTML = "<!DOCTYPE html><html><head><meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'><meta http-equiv='refresh' content='5; url=/config'><title>beAMP - Erro</title>";
            errHTML += "<style>body{background:#121212;color:#E0E0E0;font-family:sans-serif;text-align:center;padding:40px 20px;margin:0;} .card{background:#1e1e1e;padding:40px 30px;border-radius:12px;display:inline-block;box-shadow:0 8px 20px rgba(0,0,0,0.6);max-width:400px;width:90%;box-sizing:border-box;} h2{color:#E31E24;margin-top:0;font-size:1.5rem;} p{color:#aaa;line-height:1.5;}</style></head>";
            errHTML += "<body><div class='card'><h2>Falha de Ligacao</h2><p>Nao foi possivel verificar atualizacoes. Verifique a internet do equipamento.</p><p style='font-size:0.9em;color:#777;'>A voltar as configuracoes...</p></div></body></html>";
            server.send(200, "text/html", errHTML);
        }
        http.end();
      });

      server.begin();
  } else {
      Serial.println("[WIFI] Nenhuma rede salva encontrada. Modo AP Iniciado no fundo.");
      portalRunning = true;
  }
  
  WiFi.setSleep(false);
  WiFi.setAutoReconnect(true);
  
  Serial.println("Servidor Online. Liberando Audio...");
  applyHardwareStby(isMuted);

  delay(500); 
  Serial.println("Enviando sinal de READY para a tela...");
  Serial2.println("SYS:READY");
}

void atualizarInfoRedeNaTela() {
  String status;
  String ip;
  String ssid;
  
  // ---> CORREÇÃO: Os valores da tela são sempre reais, com ou sem internet!
  String telaTempo = String(screenTimeSec) + "s";
  String telaBrilho = String(screenBrightness) + "%";

  if (WiFi.status() == WL_CONNECTED) {
    status = "CONECTADO";
    ip = WiFi.localIP().toString();
    ssid = WiFi.SSID();
    if (ssid == "") ssid = wm.getWiFiSSID();
    if (ssid == "") ssid = "Rede Salva";
  } else {
    status = "DESCONECTADO";
    ip = "----";
    ssid = "----";
  }
  
  Serial2.println("INF:STATUS:" + status);
  Serial2.println("INF:IP:" + ip);
  Serial2.println("INF:SSID:" + ssid);
  Serial2.println("INF:NOME:" + globalAmpName);
  
  Serial2.println("INF:TELA_T:" + telaTempo);
  Serial2.println("INF:TELA_B:" + telaBrilho);
}

void loop() {
  if (needsReboot) {
      delay(1000);
      ESP.restart();
  }

  if (portalRunning) {
      wm.process();
  } else {
      server.handleClient();
  }
  
  ouvirTela();
  
  // ---> GATILHO INSISTENTE DE SINCRONIZAÇÃO <---
  static unsigned long lastReadyPing = 0;
  if (!isScreenSynced && millis() - lastReadyPing > 2000) {
      Serial2.println("SYS:READY");
      lastReadyPing = millis();
  }
  
  static unsigned long lastNetUpdate = 0;
  if (millis() - lastNetUpdate > 5000) {
      atualizarInfoRedeNaTela();
      lastNetUpdate = millis();
  }
  if (isGlobalLocked && (millis() - lockStartTime > 2000)) isGlobalLocked = false;
  
  if (digitalRead(PIN_RESET) == LOW) {
    if (!isPressing) { isPressing = true;
    pressTime = millis(); Serial.println("Botao Reset Pressionado..."); }
    if (millis() - pressTime > 5000) { 
        Serial.println("!!! RESET DE FABRICA !!!");
        wm.resetSettings(); 
        preferences.begin("beamp-quad", false);
        preferences.clear(); 
        preferences.end();
        ESP.restart();
    }
  } else { isPressing = false; }
  
  if (needsToSave && (millis() - lastChangeTime > SAVE_DELAY)) {
    Serial.println("[MEM] Salvando alteracoes...");
    preferences.begin("beamp-quad", false);
    preferences.putBool("isMuted", isMuted);
    for(int i=0; i<4; i++) {
      preferences.putFloat(("v"+String(i)).c_str(), vol[i]);
      preferences.putInt(("s"+String(i)).c_str(), src[i]);
      preferences.putString(("n"+String(i)).c_str(), zoneName[i]);
      preferences.putBool(("zm"+String(i)).c_str(), zoneMute[i]);
      
      preferences.putInt(("qb"+String(i)).c_str(), eqBass[i]);
      preferences.putInt(("qm"+String(i)).c_str(), eqMid[i]);
      preferences.putInt(("qt"+String(i)).c_str(), eqTreb[i]);
    }
    preferences.putString("in0", srcName[0]);
    preferences.putString("in1", srcName[1]);
    preferences.putString("in2", srcName[2]);
    preferences.putInt("screenTime", screenTimeSec);
    preferences.putInt("brightness", screenBrightness);
    
    preferences.end();
    needsToSave = false;
    Serial.println("[MEM] Salvo com sucesso.");
  }
}