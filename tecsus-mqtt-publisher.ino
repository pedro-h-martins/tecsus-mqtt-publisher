#include <esp_mac.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <time.h>
#include <DHT.h> 
#include "config.h"

#define pino 13
#define type DHT22

DHT dht(pino, type);

// WiFi
char* ssid = WIFI_SSID;
char* pwd = WIFI_PASSWORD;

// Broker EMQX
char* mqtt_server = MQTT_SERVER;
int mqtt_port = MQTT_PORT;
char* mqtt_user = MQTT_USER;
char* mqtt_pass = MQTT_PASS;

char* ntpServer = "br.pool.ntp.org";
long gmtOffset = -3 * 3600;
int daylight = 0;

char uid[13];

char topico[40];

typedef struct {
  float tem;
  float umi;
  float plu;
} Medidas_t;

Medidas_t med;

TaskHandle_t taskSensores;
TaskHandle_t taskMonWifi;

SemaphoreHandle_t mutex;

WiFiClientSecure wclient;
PubSubClient mqttClient(wclient);

void taskLerSensores(void *pvParameters) {
  Serial.println("[Sensores] Task iniciado no core " + String(xPortGetCoreID()));

  while (true) {
    xSemaphoreTake(mutex, portMAX_DELAY);

    float temperatura = dht.readTemperature();
    float umidade = dht.readHumidity();

    if (!isnan(temperatura)) med.tem = temperatura;
    if (!isnan(umidade)) med.umi = umidade;
    med.plu = random(0, 12)  + (random(100) / 100.0);

    xSemaphoreGive(mutex);
    delay(300000);
  }
}

void taskMonitorWifi(void *pvParameters) {
  Serial.println("[WiFi] Task monitor iniciada.");

  while (true) {
    if (WiFi.status() != WL_CONNECTED) {
      Serial.println("[WiFi] Conexão perdida, reconectando...");

      WiFi.begin(ssid, pwd);

      while (WiFi.status() != WL_CONNECTED) {
        delay(500);
        Serial.print(".");
      }
      Serial.println("\n[WiFi] Reconectado.");
    }
    delay(30000);
  }
}

void connectMqtt() {
  while (!mqttClient.connected()) {
    Serial.print("[MQTT] Conectando ao broker EMQX...");

    if (mqttClient.connect(uid, mqtt_user, mqtt_pass)) {
      Serial.println("[MQTT] Conectado");
    } else {
      Serial.print("[MQTT] Conexão falhou. Código: ");
      Serial.print(mqttClient.state());
      Serial.println("[MQTT] Tentando novamente em 5 segundos...");
      delay(5000);
    }
  }
}

void sincronizarTempo() {
  configTime(gmtOffset, daylight, ntpServer);
  struct tm timeinfo;

  if (!getLocalTime(&timeinfo)) {
    Serial.println("[NTP] Erro ao sincronizar o tempo.");
  } else {
    Serial.print("[NTP] Tempo sincronizado: ");
    Serial.println(time(nullptr));
  }
}

void setup() {
  Serial.begin(115200);
  dht.begin();
  delay(3000);
  randomSeed(analogRead(0));
  
  uint8_t mac[6];
  esp_read_mac(mac, ESP_MAC_WIFI_STA);
  snprintf(uid, sizeof(uid), "%02X%02X%02X%02X%02X%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

  Serial.print("[Setup] UID da placa: ");
  Serial.println(uid);

  snprintf(topico, sizeof(topico), "estacoes/%s/dados", uid);
  Serial.print("[Setup] Tópico: ");
  Serial.println(topico);

  Serial.print("[WiFi] Conectado a rede: ");
  Serial.println(ssid);
  WiFi.begin(ssid, pwd);

  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }

  Serial.print("\n[WiFi] Conectado. IP: ");
  Serial.println(WiFi.localIP());

  sincronizarTempo();
  wclient.setInsecure();

  mqttClient.setServer(mqtt_server, mqtt_port);
  mqttClient.setBufferSize(512);
  connectMqtt();

  mutex = xSemaphoreCreateMutex();
  if (mutex == NULL) {
    Serial.println("[Setup] Erro ao cruar mutex.");
  }

  xTaskCreatePinnedToCore(
    taskLerSensores,
    "TaskSensores",
    10000,
    NULL,
    1,
    &taskSensores,
    0
  );

  xTaskCreatePinnedToCore(
    taskMonitorWifi,
    "TaskMonWifi",
    10000,
    NULL,
    1,
    &taskMonWifi,
    1
  );
}

void loop() {
  if (!mqttClient.connected()) {
    connectMqtt();
  }
  mqttClient.loop();

  xSemaphoreTake(mutex, portMAX_DELAY);

  DynamicJsonDocument payload(256);
  payload["uid"] = uid;
  payload["uxt"] = (uint32_t)time(nullptr);
  payload["tem"] = med.tem;
  payload["umi"] = med.umi;
  payload["plu"] = med.plu;

  xSemaphoreGive(mutex);

  char payloadStr[256];
  serializeJson(payload, payloadStr);

  bool ok = mqttClient.publish(topico, payloadStr, false);
  Serial.print("[MQTT] Publicado em ");
  Serial.println(topico);
  Serial.print(" → ");
  Serial.print(ok ? "OK" : "Falhou");
  Serial.print(" | ");
  Serial.println(payloadStr);

  delay(30000);
}