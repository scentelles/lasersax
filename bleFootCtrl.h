#include <NimBLEDevice.h>

// UUID BLE MIDI (confirmés par tes logs nRF/ESP32)
static NimBLEUUID PEDAL_SERVICE_UUID("03b80e5a-ede8-4b33-a751-6ce34ec4c700");
static NimBLEUUID PEDAL_CHAR_UUID   ("7772e5db-3868-4112-a1a9-f2669d106bf3");

// Service HID 0x1812 pour repérer le pédalier au scan (optionnel mais pratique)
static NimBLEUUID HID_SERVICE_UUID((uint16_t)0x1812);

// Si tu connais le nom Bluetooth exact, mets-le ici (vu dans nRF Connect)
static const char* PEDAL_NAME_1 = "CHOCOLATE";   // à adapter
static const char* PEDAL_NAME_2 = "M-Wave";      // à adapter

NimBLEClient*  g_client    = nullptr;
NimBLEAddress  g_pedalAddr;
bool           g_havePedal = false;
bool           g_doConnect = false;
bool           g_connected = false;

uint8_t blePedalValue = -1;
uint8_t currentMode = 0;
uint8_t currentParam = 0;

// À appeler depuis setup()
void bleFootCtrlInit();

// À appeler dans loop() (ou dans la phase de "wait BLE" au démarrage)
void bleFootCtrlLoop();

// ================== Ta logique à toi : que faire avec le CC ? ==================
void handlePedalCC(uint8_t channel, uint8_t cc, uint8_t value) {
  
  if(value == 5) //bank change when pressing 2 buttons simultaneously
    return;
  // Ici tu mets TA logique finale : GPIO, MIDI out, etc.
  // Pour l’instant, on log juste.
  Serial.print("Pedal CC - ch: ");
  Serial.print(channel + 1);
  Serial.print("  CC: ");
  Serial.print(cc);
  Serial.print("  val: ");
  Serial.println(value);

  // Exemple : si CC3 valeur 0x7F = bouton 1 appuyé
  // if (cc == 3 && value == 0x7F) { ... }



  if(cc%4 == 0)
  {
    if(currentMode != 0)
      currentMode = 0; 
    else
      currentMode = 1; 
  }
  
  if(cc%4 == 1)
    currentMode = 101 + 25*cc/4;

  if(cc%4 == 2)
    currentMode = 51;  

  if(cc%4 == 3)
    currentMode = 26;    

   
  currentParam = cc/4 * 25 + 25;
  Serial.print("currentmode : ");  
  Serial.println(currentMode);
  Serial.print("currentParam : ");  
  Serial.println(currentParam);
}

// ================== Décodeur BLE MIDI minimal ==================
// data / length = ce que tu reçois dans notifyCB
void decodeBleMidi(uint8_t* data, size_t length) {
  // BLE MIDI : octets avec bit 7 = 1 peuvent être des timestamps.
  // On cherche un status MIDI (0x80-0xEF) puis ses 2 data bytes.
  size_t i = 0;
  while (i < length) {
    uint8_t b = data[i];

    // Timestamp (MSB=1, mais pas un status MIDI valide)
    if ((b & 0x80) && (b < 0x80 || b > 0xEF)) {
      i++;
      continue;
    }

    // Status MIDI ?
    if (b >= 0x80 && b <= 0xEF) {
      uint8_t status = b;
      uint8_t ch     = status & 0x0F;

      // Il nous faut au moins 2 data bytes derrière
      if (i + 4 < length) {
        uint8_t type = data[i + 2];
        uint8_t d1 = data[i + 3];
        uint8_t d2 = data[i + 4];
        if (type == 0xB0) { // Control Change
          handlePedalCC(ch, d1, d2);
        } else {
          // On pourrait gérer NoteOn/NoteOff etc. si besoin
          Serial.print("MIDI non-CC reçu, status=0x");
          Serial.println(type, HEX);
        }

        i += 5;
        continue;
      } else {
        // message tronqué
        break;
      }
    }

    // Octet inconnu, on avance
    i++;
  }
}

// ================== CALLBACK NOTIFY ==================
void pedalNotifyCB(NimBLERemoteCharacteristic* chr,
                   uint8_t* data,
                   size_t length,
                   bool isNotify) {
  auto* svc = chr->getRemoteService();

  Serial.println("==== NOTIFY PEDAL ====");
  Serial.print("Service: ");
  Serial.println(svc->getUUID().toString().c_str());
  Serial.print("Char   : ");
  Serial.println(chr->getUUID().toString().c_str());

  Serial.print("Data (");
  Serial.print(length);
  Serial.println(" octets) :");
  for (size_t i = 0; i < length; i++) {
    Serial.print("0x");
    if (data[i] < 0x10) Serial.print("0");
    Serial.print(data[i], HEX);
    Serial.print(" ");
  }
  Serial.println();
  Serial.println("======================");

  // On passe dans le décodeur BLE MIDI
  decodeBleMidi(data, length);
}

// ================== CLIENT CALLBACKS ==================
class MyClientCallbacks : public NimBLEClientCallbacks {
  void onConnect(NimBLEClient* pClient) override {
    Serial.println("Connecté au pédalier");
    g_connected = true;
    pClient->updateConnParams(120, 120, 0, 60);
  }

  void onDisconnect(NimBLEClient* pClient, int reason) override {
    Serial.print("Déconnecté, reason = ");
    Serial.println(reason);
    g_connected = false;
    g_doConnect = false;

    // Si tu veux auto-reconnect, tu peux mettre un flag ici
    // ou relancer un scan.
    NimBLEScan* scan = NimBLEDevice::getScan();
    if (!scan->isScanning()) {
      Serial.println("Relance du scan (pédalier déconnecté)...");
      g_havePedal = false;
      scan->start(3000, false); // scan 3 secondes
    }
  }
};

MyClientCallbacks g_clientCallbacks;

// ================== CONNEXION CIBLÉE ==================
bool connectToPedal() {
  if (!g_havePedal) {
    Serial.println("connectToPedal() appelé mais aucune adresse pedal connue");
    return false;
  }

  Serial.print("Tentative de connexion à : ");
  Serial.println(g_pedalAddr.toString().c_str());

  if (g_client == nullptr) {
    g_client = NimBLEDevice::createClient();
    g_client->setClientCallbacks(&g_clientCallbacks);
  }

  if (!g_client->connect(g_pedalAddr, false)) { // false = pas de reconnexion auto
    Serial.println("Connexion échouée");
    return false;
  }

  Serial.println("Connexion OK, recherche du service BLE MIDI...");
  NimBLERemoteService* pedalService = g_client->getService(PEDAL_SERVICE_UUID);
  if (!pedalService) {
    Serial.println("Service BLE MIDI non trouvé (UUID incorrect ou device différent ?)");
    g_client->disconnect();
    return false;
  }

  Serial.println("Service BLE MIDI trouvé. Recherche de la caractéristique data...");
  NimBLERemoteCharacteristic* pedalChar = pedalService->getCharacteristic(PEDAL_CHAR_UUID);
  if (!pedalChar) {
    Serial.println("Caractéristique BLE MIDI non trouvée (UUID incorrect ?)");
    g_client->disconnect();
    return false;
  }

  Serial.print("Caractéristique trouvée : ");
  Serial.println(pedalChar->getUUID().toString().c_str());
  Serial.print("Props : ");
  if (pedalChar->canRead())     Serial.print("R ");
  if (pedalChar->canWrite())    Serial.print("W ");
  if (pedalChar->canNotify())   Serial.print("N ");
  if (pedalChar->canIndicate()) Serial.print("I ");
  Serial.println();

  Serial.println("Abonnement aux notifications BLE MIDI...");
  if (!pedalChar->subscribe(true, pedalNotifyCB)) {
    Serial.println("!! Echec subscribe()");
    g_client->disconnect();
    return false;
  }

  Serial.println("subscribe OK, en attente d’appuis sur les boutons.");
  return true;
}

// ================== SCAN CALLBACKS ==================
class MyScanCallbacks : public NimBLEScanCallbacks {
  void onResult(const NimBLEAdvertisedDevice* dev) override {
    Serial.print("Vu périphérique : ");
    Serial.println(dev->toString().c_str());

    bool nameMatch = false;
    if (dev->haveName()) {
      std::string n = dev->getName();
      if (n.find(PEDAL_NAME_1) != std::string::npos ||
          n.find(PEDAL_NAME_2) != std::string::npos) {
        nameMatch = true;
      }
    }

    bool hidMatch = dev->haveServiceUUID() && dev->isAdvertisingService(HID_SERVICE_UUID);

    if (nameMatch || hidMatch) {
      Serial.println("→ Pédalier trouvé, on sauvegarde son adresse et on arrête le scan.");

      g_pedalAddr = dev->getAddress();
      g_havePedal = true;
      g_doConnect = true;

      NimBLEDevice::getScan()->stop();
    }
  }

  void onScanEnd(const NimBLEScanResults& results, int reason) override {
    Serial.print("Scan terminé, reason = ");
    Serial.println(reason);
    if (!g_havePedal) {
      Serial.println("Aucun pédalier trouvé pendant ce scan.");
    }
  }
};

MyScanCallbacks g_scanCallbacks;



// ================== INIT BLE FOOT CTRL ==================
void bleFootCtrlInit() {
  Serial.println("[BLE] Init BLE FootCtrl...");

  if (!NimBLEDevice::init("ESP32-PEDAL")) {
    Serial.println("[BLE] Erreur init NimBLEDevice");
    return;
  }

  NimBLEScan* scan = NimBLEDevice::getScan();
  scan->setScanCallbacks(&g_scanCallbacks, false);  // pas de doublons
  scan->setActiveScan(true);
  scan->setInterval(45);
  scan->setWindow(15);
  scan->setMaxResults(0);

  Serial.println("[BLE] Scan BLE en continu...");
  scan->start(0, true);  // 0 + true = scan continu, NON bloquant
}



// ================== LOOP BLE FOOT CTRL ==================
void bleFootCtrlLoop() {
  // 1) Si on a un périphérique trouvé, et qu'on doit tenter la connexion :
  if (g_doConnect && g_havePedal && !g_connected) {
    g_doConnect = false;
    Serial.println("[BLE] g_doConnect=true -> connectToPedal()");
    if (!connectToPedal()) {
      Serial.println("[BLE] Connexion échouée, on oublie l'adresse et on relance un scan.");
      g_havePedal = false;
      NimBLEScan* scan = NimBLEDevice::getScan();
      if (!scan->isScanning()) {
        scan->start(0, true);
      }
    }
  }

  // 2) Si on n’est plus connecté et qu’on n’a pas d’adresse, on s’assure que le scan tourne
  if (!g_connected && !g_havePedal) {
    NimBLEScan* scan = NimBLEDevice::getScan();
    if (!scan->isScanning()) {
      Serial.println("[BLE] Pas de pédalier connu, relance du scan continu.");
      scan->start(0, true);
    }
  }

  // Rien d’autre à faire : NimBLE tourne en tâche de fond
}