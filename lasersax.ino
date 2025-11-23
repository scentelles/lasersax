#include <WiFi.h>
#include <Adafruit_TLC5947.h>
#include <ArtnetWifi.h>

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

// ----------- CONFIG WIFI -------------
const char* ssid     = "Wifi_Home";
const char* password = "060877040178";

// ----------- CONFIG ART-NET ----------
ArtnetWifi artnet;

const uint16_t ARTNET_UNIVERSE = 0;   // Univers que tu utiliseras dans ton soft lumière
const uint8_t  NUM_LASERS      = 16;
const uint8_t  START_CHANNEL   = 1;   // DMX address de Laser1 = 1
const uint8_t  NB_DMX_CHANNEL = 18; //1 : Mode (Manual/AUTO-CHASER/AUTO-SINWAVE) - 2: STROBE - 3-18 dimmer 

// ----------- DMX BUFFER --------------
uint8_t dmxData[512]; // buffer de DMX reçu (optionnel, pour debug)


uint8_t currentMode = 0;
uint8_t currentParam = 0;

// Callback appelé à chaque frame ArtDMX reçue
void onDmxFrame(uint16_t universe, uint16_t length, uint8_t sequence, uint8_t* data) {
  Serial.println("Received DMX!!!!!");
 
  if (universe != ARTNET_UNIVERSE) return;

  // Copie optionnelle pour debug
  if (length > 512) length = 512;
  memcpy(dmxData, data, length);

  // Mise à jour des canaux DMX
  for (uint8_t i = 0; i < NB_DMX_CHANNEL; i++) {
    uint16_t dmxChannel = START_CHANNEL - 1 + i; // DMX canal 1 → index 0
    if (dmxChannel < length) {
      uint8_t val = data[dmxChannel];
      if(dmxChannel == 0)
      {
        Serial.print("Switching mode : ");
        Serial.println(val);
        currentMode = val;  
      }
      if(dmxChannel == 1)
      {
        Serial.print("Param : ");
        Serial.println(val);
        currentParam = val;  
      }
      if(dmxChannel >= 2)
      {
          setLaserBrightness(i-2, val*16);
        } else {
          setLaserBrightness(i-2, 0);
        }
      }
  }

  // Envoi des valeurs au TLC5947
  tlc.write();
}



void connectWifi() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);
  Serial.print("Connexion WiFi");

  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }

  Serial.println();
  Serial.print("Connecté, IP = ");
  Serial.println(WiFi.localIP());
}


void sinWaveEffect(unsigned long timeMs, float speed, float wavelength) {
  // timeMs : le temps actuel en millisecondes
  // speed : vitesse de déplacement de l'onde (ex: 0.005)
  // wavelength : longueur d'onde (ex: 0.5)

  for (int i = 0; i < NB_LASER; i++) {
    // Calcul de la phase
    float phase = (i * wavelength) + (timeMs * speed);

    // sin() produit -1..+1 → on normalise 0..1
    float wave = (sin(phase) + 1.0) * 0.5;

    // Convertir en intensité 12 bits (0..4095)
    uint16_t intensity = (uint16_t)(wave * 4095);

    setLaserBrightness(i, intensity);
  }

  tlc.write();  // envoyer au TLC
}

void sinWaveEffectBy8Reverse(unsigned long timeMs, float speed, float wavelength) {
  // timeMs : le temps actuel en millisecondes
  // speed : vitesse de déplacement de l'onde (ex: 0.005)
  // wavelength : longueur d'onde (ex: 0.5)

  for (int i = 0; i < NB_LASER/2; i++) {
    // Calcul de la phase
    float phase = (i * wavelength) + (timeMs * speed);

    // sin() produit -1..+1 → on normalise 0..1
    float wave = (sin(phase) + 1.0) * 0.5;

    // Convertir en intensité 12 bits (0..4095)
    uint16_t intensity = (uint16_t)(wave * 4095);

    setLaserBrightness(i, intensity);
    setLaserBrightness(NB_LASER - 1 - i, intensity);
  }

  tlc.write();  // envoyer au TLC
}
void sinWaveEffectBy8(unsigned long timeMs, float speed, float wavelength) {
  // timeMs : le temps actuel en millisecondes
  // speed : vitesse de déplacement de l'onde (ex: 0.005)
  // wavelength : longueur d'onde (ex: 0.5)

  for (int i = 0; i < NB_LASER/2; i++) {
    // Calcul de la phase
    float phase = (i * wavelength) + (timeMs * speed);

    // sin() produit -1..+1 → on normalise 0..1
    float wave = (sin(phase) + 1.0) * 0.5;

    // Convertir en intensité 12 bits (0..4095)
    uint16_t intensity = (uint16_t)(wave * 4095);

    setLaserBrightness(NB_LASER/2 - 1 - i, intensity);
    setLaserBrightness(NB_LASER/2 + i, intensity);
  }

  tlc.write();  // envoyer au TLC
}


// laserIndex : 0..24
// value : 0..4095 (12 bits)
void setLaserBrightness(int laserIndex, uint16_t value) {
  if (laserIndex < 0 || laserIndex >= NB_LASER) return;
  tlc.setPWM(IdToPin[laserIndex], value);
}

void AllLightsOn()
{
      for (int i = 0; i < NB_LASER; i++) {
        setLaserBrightness(i, 4095);
      }
}

void AllLightsOff()
{
      for (int i = 0; i < NB_LASER; i++) {
        setLaserBrightness(i, 0);
      }
}




void chaser(int speed) // between 0 and 100
{
  for (int i = 0; i < NB_LASER; i++) {
    for (int j = 0; j < NB_LASER; j++) {
      setLaserBrightness(j, (j == i) ? 4095 : 0);
    }
    tlc.write();
    if(speed > 100)
      speed = 100;
    if(speed < 1)
      speed = 1;
    
    delay(1000 / speed);

  }
}

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

  // Init WiFi
  connectWifi();






  // Création de la première tâche
  xTaskCreatePinnedToCore(
    Task1code,       // Fonction à lancer
    "Task1",         // Nom
    4096,            // Stack size
    NULL,            // Paramètre
    1,               // Priorité
    &Task1,          // Handle
    0                // Core 0
  );

  // Création de la deuxième tâche
  xTaskCreatePinnedToCore(
    Task2code,
    "Task2",
    4096,
    NULL,
    1,
    &Task2,
    1                // Core 1
  );


}


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
    
    if(currentMode > 100)
    {
      unsigned long now = millis();
      float wavelength = 0.02 + (currentMode - 100 ) / 150 ;
      Serial.println(wavelength);
      float speed = 2.0 / currentParam;
      Serial.println(speed);
      sinWaveEffectBy8(now, speed, wavelength);
    }
    else
    if(currentMode > 50)
    {
      chaser(currentParam);

    }    
    


    vTaskDelay(1 / portTICK_PERIOD_MS);
  }
}

void loop() {

/*
  // Exemple : fondu global monté/descendu
  for (int v = 0; v <= 4095; v += 40) {
    for (int i = 0; i < 24; i++) {
      setLaserBrightness(i, v);
    }
    tlc.write();
    delay(10);
  }

  for (int v = 4095; v >= 0; v -= 40) {
    for (int i = 0; i < 24; i++) {
      setLaserBrightness(i, v);
    }
    tlc.write();
    delay(10);
  }*/

    
}
