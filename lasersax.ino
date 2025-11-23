#include <WiFi.h>
#include <Adafruit_TLC5947.h>
#include <ArtnetWifi.h>
#include <WebServer.h>
#include <Preferences.h>

#define NUM_TLC5947 1

// Pins ESP32 -> TLC5947
#define TLC_CLK   18
#define TLC_DAT   23
#define TLC_LAT   16
// OE optionnel (sinon non connecté ou à GND)

#define NB_LASER 16

Adafruit_TLC5947 tlc = Adafruit_TLC5947(NUM_TLC5947, TLC_CLK, TLC_DAT, TLC_LAT);

char IdToPin[16];

TaskHandle_t Task1;
TaskHandle_t Task2;

int count = 0;

// ----------- CONFIG WIFI (DEFAULT / USINE) -------------
// Utilisés seulement si aucune config sauvegardée trouvée
const char* DEFAULT_SSID     = "slyzic-hotspot";
const char* DEFAULT_PASSWORD = "totototo";

// ----------- CONFIG AP DE SECOURS ----------------
const char* CONFIG_AP_SSID = "LaserConfig";
const char* CONFIG_AP_PASS = "12345678"; // min 8 caractères

WebServer server(80);
Preferences preferences;

// Variables globales pour la config STA persistante
String staSsidConfig;
String staPassConfig;

// ----------- CONFIG ART-NET ----------
ArtnetWifi artnet;

const uint16_t ARTNET_UNIVERSE = 0;
const uint8_t  NUM_LASERS      = 16;
const uint8_t  START_CHANNEL   = 1;
const uint8_t  NB_DMX_CHANNEL  = 18; //1 : Mode - 2: Param - 3-18 dimmer 

// ----------- DMX BUFFER --------------
uint8_t dmxData[512];

uint8_t currentMode = 0;
uint8_t currentParam = 0;

// ===================== PERSISTENCE WIFI =====================

void saveWifiConfig(const String& ssid, const String& pass) {
  preferences.begin("wifi", false); // false = lecture/écriture
  preferences.putString("ssid", ssid);
  preferences.putString("pass", pass);
  preferences.end();
  Serial.println("Config WiFi sauvegardée en NVS.");
}

void loadWifiConfig(String& ssid, String& pass) {
  preferences.begin("wifi", true); // true = lecture seule
  ssid = preferences.getString("ssid", "");
  pass = preferences.getString("pass", "");
  preferences.end();

  if (ssid.length() > 0) {
    Serial.print("Config WiFi trouvée en NVS : SSID = ");
    Serial.println(ssid);
  } else {
    Serial.println("Aucune config WiFi sauvegardée trouvée.");
  }
}

// ===================== SERVEUR WEB / CONFIG AP =====================

void handleRoot() {
  String html = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
  <meta charset="utf-8">
  <title>Configuration WiFi</title>
</head>
<body>
  <h1>Configuration du WiFi (STA)</h1>
  <p>SSID actuel sauvegardé : <b>)rawliteral";

  if (staSsidConfig.length() > 0) {
    html += staSsidConfig;
  } else {
    html += "Aucun (mode usine)";
  }

  html += R"rawliteral(</b></p>
  <form method="POST" action="/save">
    <label>SSID WiFi :</label><br>
    <input type="text" name="ssid" required><br><br>
    <label>Mot de passe WiFi :</label><br>
    <input type="password" name="password" required><br><br>
    <input type="submit" value="Enregistrer & Reboot">
  </form>
  <p>AP de configuration : SSID <b>)rawliteral";
  html += CONFIG_AP_SSID;
  html += R"rawliteral(</b></p>
  <p>IP AP : )rawliteral";
  html += WiFi.softAPIP().toString();
  html += R"rawliteral(</p>
</body>
</html>
)rawliteral";

  server.send(200, "text/html", html);
}

void handleSave() {
  if (!server.hasArg("ssid") || !server.hasArg("password")) {
    server.send(400, "text/plain", "Parametres manquants");
    return;
  }

  String newSsid = server.arg("ssid");
  String newPass = server.arg("password");

  if (newSsid.length() == 0) {
    server.send(400, "text/plain", "SSID vide");
    return;
  }

  // Tu peux ajouter des contraintes ici si tu veux (longueur mini, etc.)

  saveWifiConfig(newSsid, newPass);

  String html = "<html><body><h1>Configuration enregistree</h1>";
  html += "<p>Nouveau SSID : <b>" + newSsid + "</b></p>";
  html += "<p>L'ESP va redemarrer dans 2 secondes...</p>";
  html += "</body></html>";

  server.send(200, "text/html", html);

  Serial.println("Nouvelle config WiFi enregistree, redemarrage...");
  delay(2000);
  ESP.restart();
}

void setupWebServer() {
  server.on("/", HTTP_GET, handleRoot);
  server.on("/save", HTTP_POST, handleSave);
  server.begin();
  Serial.println("Serveur HTTP de configuration demarre");
}

void startConfigAP() {
  Serial.println("Demarrage de l'AP de configuration...");
  WiFi.mode(WIFI_AP);
  WiFi.softAP(CONFIG_AP_SSID, CONFIG_AP_PASS);

  Serial.print("AP SSID : ");
  Serial.println(CONFIG_AP_SSID);
  Serial.print("IP AP : ");
  Serial.println(WiFi.softAPIP());

  setupWebServer();
}

// ===================== WIFI STA =====================

bool connectWifi(const char* ssid, const char* password) {
  if (ssid == nullptr || strlen(ssid) == 0) return false;

  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);

  Serial.print("Connexion WiFi a ");
  Serial.print(ssid);

  unsigned long start = millis();
  const unsigned long timeout = 15000; // 15 s

  while (WiFi.status() != WL_CONNECTED && (millis() - start) < timeout) {
    delay(500);
    Serial.print(".");
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println();
    Serial.print("Connecte, IP = ");
    Serial.println(WiFi.localIP());
    return true;
  } else {
    Serial.println();
    Serial.println("Echec de connexion WiFi.");
    WiFi.disconnect(true);
    return false;
  }
}

// ===================== ART-NET / LASERS =====================

// Callback appelé à chaque frame ArtDMX reçue
void onDmxFrame(uint16_t universe, uint16_t length, uint8_t sequence, uint8_t* data) {
  Serial.println("Received DMX!!!!!");
 
  if (universe != ARTNET_UNIVERSE) return;

  if (length > 512) length = 512;
  memcpy(dmxData, data, length);

  for (uint8_t i = 0; i < NB_DMX_CHANNEL; i++) {
    uint16_t dmxChannel = START_CHANNEL - 1 + i; // DMX canal 1 → index 0
    if (dmxChannel < length) {
      uint8_t val = data[dmxChannel];
      if (dmxChannel == 0) {
        Serial.print("Switching mode : ");
        Serial.println(val);
        currentMode = val;  
      }
      if (dmxChannel == 1) {
        Serial.print("Param : ");
        Serial.println(val);
        currentParam = val;  
      }
      if (dmxChannel >= 2) {
        setLaserBrightness(i - 2, val * 16);
      } else {
        setLaserBrightness(i - 2, 0);
      }
    }
  }

  tlc.write();
}

void sinWaveEffect(unsigned long timeMs, float speed, float wavelength) {
  for (int i = 0; i < NB_LASER; i++) {
    float phase = (i * wavelength) + (timeMs * speed);
    float wave = (sin(phase) + 1.0) * 0.5;
    uint16_t intensity = (uint16_t)(wave * 4095);
    setLaserBrightness(i, intensity);
  }
  tlc.write();
}

void sinWaveEffectBy8Reverse(unsigned long timeMs, float speed, float wavelength) {
  for (int i = 0; i < NB_LASER / 2; i++) {
    float phase = (i * wavelength) + (timeMs * speed);
    float wave = (sin(phase) + 1.0) * 0.5;
    uint16_t intensity = (uint16_t)(wave * 4095);
    setLaserBrightness(i, intensity);
    setLaserBrightness(NB_LASER - 1 - i, intensity);
  }
  tlc.write();
}

void sinWaveEffectBy8(unsigned long timeMs, float speed, float wavelength) {
  for (int i = 0; i < NB_LASER / 2; i++) {
    float phase = (i * wavelength) + (timeMs * speed);
    float wave = (sin(phase) + 1.0) * 0.5;
    uint16_t intensity = (uint16_t)(wave * 4095);
    setLaserBrightness(NB_LASER/2 - 1 - i, intensity);
    setLaserBrightness(NB_LASER/2 + i, intensity);
  }
  tlc.write();
}

// laserIndex : 0..15
// value : 0..4095 (12 bits)
void setLaserBrightness(int laserIndex, uint16_t value) {
  if (laserIndex < 0 || laserIndex >= NB_LASER) return;
  tlc.setPWM(IdToPin[laserIndex], value);
}

void AllLightsOn() {
  for (int i = 0; i < NB_LASER; i++) {
    setLaserBrightness(i, 4095);
  }
  tlc.write();
}

void AllLightsOff() {
  for (int i = 0; i < NB_LASER; i++) {
    setLaserBrightness(i, 0);
  }
  tlc.write();
}

void chaser(int speed) { // between 0 and 100
  for (int i = 0; i < NB_LASER; i++) {
    for (int j = 0; j < NB_LASER; j++) {
      setLaserBrightness(j, (j == i) ? 4095 : 0);
    }
    tlc.write();
    if (speed > 100) speed = 100;
    if (speed < 1)   speed = 1;
    delay(1000 / speed);
  }
}

void strobe(int param) { // between 0 and 255
  float speed = (1 - (float)param/255) * 120 + 30;
  Serial.print("strobe speed : ");
  Serial.println(speed);
  AllLightsOn();
  delay(speed);
  AllLightsOff();   
  delay(speed);
}

// ===================== FREE RTOS TASKS =====================

void Task1code(void* pvParameters) {
  // Init Art-Net
  artnet.begin();
  artnet.setArtDmxCallback(onDmxFrame);

  for (;;) {
    artnet.read();
    vTaskDelay(1 / portTICK_PERIOD_MS);
  }
}

void Task2code(void* pvParameters) {
  for (;;) {
    if (currentMode > 100) {
      unsigned long now = millis();
      float wavelength = (((float)currentMode - 100)/ 150) * 2 + 0.03;
      Serial.print("wavelength : ");
      Serial.println(wavelength);
      float speed = ((float)currentParam/255) * 0.03 + 0.002;
      Serial.print("speed : ");
      Serial.println(speed);
      sinWaveEffectBy8(now, speed, wavelength);
    }
    else if (currentMode > 50) {
      chaser(currentParam);
    }
    else if (currentMode > 25) {
      strobe(currentParam);
    }
        
    vTaskDelay(1 / portTICK_PERIOD_MS);
  }
}

// ===================== SETUP / LOOP =====================

void setup() {
  Serial.begin(115200);

  IdToPin[0] = 12;
  IdToPin[1] = 13;
  IdToPin[2] = 14;
  IdToPin[3] = 15;
  IdToPin[4] = 16;
  IdToPin[5] = 17;
  IdToPin[6] = 18;
  IdToPin[7] = 19;
  IdToPin[8] = 4;
  IdToPin[9] = 5;
  IdToPin[10] = 6;
  IdToPin[11] = 7;
  IdToPin[12] = 8;  
  IdToPin[13] = 9;  
  IdToPin[14] = 10;  
  IdToPin[15] = 11;  

  tlc.begin();

  // Tout éteindre au départ
  for (int i = 0; i < 24; i++) {
    tlc.setPWM(i, 0);
  }
  tlc.write();

  // Charger la config WiFi persistante (si dispo)
  loadWifiConfig(staSsidConfig, staPassConfig);

  bool wifiOk = false;

  // 1) Si une config est sauvegardée → on tente en priorité
  if (staSsidConfig.length() > 0) {
    Serial.println("loading existing config");
    wifiOk = connectWifi(staSsidConfig.c_str(), staPassConfig.c_str());
  }

  // 2) Sinon, on tente les valeurs "usine"
  if (!wifiOk) {
    Serial.println("Loading default wifi setup");
    wifiOk = connectWifi(DEFAULT_SSID, DEFAULT_PASSWORD);
    if (wifiOk) {
      // On peut éventuellement les sauvegarder en tant que config persistante
      saveWifiConfig(DEFAULT_SSID, DEFAULT_PASSWORD);
      staSsidConfig = DEFAULT_SSID;
      staPassConfig = DEFAULT_PASSWORD;
    }
  }

  // 3) Si toujours pas de WiFi → on lance l’AP de configuration
  if (!wifiOk) {
    startConfigAP();
  }

  // Création de la première tâche (Art-Net)
  xTaskCreatePinnedToCore(
    Task1code,
    "Task1",
    4096,
    NULL,
    1,
    &Task1,
    0
  );

  // Création de la deuxième tâche (effets)
  xTaskCreatePinnedToCore(
    Task2code,
    "Task2",
    4096,
    NULL,
    1,
    &Task2,
    1
  );
}

void loop() {
  // Gestion éventuelle des requêtes HTTP (si AP de config lancé)
  server.handleClient();
  delay(2);
}
