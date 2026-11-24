#include <Arduino.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <ESP32Servo.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include "config.h"

// Definición de Pines
const int PIN_SERVO = 18;
const int PIN_POT = 34;   // Pin analógico del potenciómetro (ADC1)
const int PIN_BUZZER = 23; // Pin para el zumbador

// Objetos
Servo servoCompuerta;
LiquidCrystal_I2C lcd(0x27, 16, 2);

WiFiClient espClient;
PubSubClient client(espClient);

unsigned long ultimoEnvio = 0;
const long intervaloEnvio = 5000; // Enviar lectura cada 5 segundos

// --- BUFFER OFFLINE (RESILIENCIA) ---
#define BUFFER_SIZE 30
float offlineBuffer[BUFFER_SIZE];
int bufferHead = 0;
int bufferTail = 0;
int bufferCount = 0;

void guardarEnBuffer(float valor) {
  if (bufferCount < BUFFER_SIZE) {
    offlineBuffer[bufferHead] = valor;
    bufferHead = (bufferHead + 1) % BUFFER_SIZE;
    bufferCount++;
    Serial.println("[Resiliencia] Conexión perdida. Dato guardado en buffer offline (RAM).");
  } else {
    offlineBuffer[bufferHead] = valor;
    bufferHead = (bufferHead + 1) % BUFFER_SIZE;
    bufferTail = (bufferTail + 1) % BUFFER_SIZE;
    Serial.println("[Resiliencia] Buffer lleno, sobrescribiendo dato antiguo.");
  }
}

void vaciarBuffer() {
  while (bufferCount > 0 && client.connected()) {
    float valorGuardado = offlineBuffer[bufferTail];
    char buffer[10];
    dtostrf(valorGuardado, 4, 1, buffer);

    if (client.publish(MQTT_TOPIC_PESO, buffer)) {
      Serial.print("[Resiliencia] Sincronizando dato offline: ");
      Serial.println(buffer);
      bufferTail = (bufferTail + 1) % BUFFER_SIZE;
      bufferCount--;
      delay(100);
    } else {
      break;
    }
  }
}
// ----------------------------------------------

float leerPotenciometro0a100() {
  int lecturaRaw = analogRead(PIN_POT); // 0 a 4095 en ESP32
  long mapeado = map(lecturaRaw, 0, 4095, 0, 100);
  if (mapeado < 0) mapeado = 0;
  if (mapeado > 100) mapeado = 100;
  return (float)mapeado;
}

void setup_wifi() {
  delay(10);
  Serial.println();
  Serial.print("Conectando a Wi-Fi: ");
  Serial.println(WIFI_SSID);

  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }

  Serial.println("\nWiFi conectado.");
}

void callback(char* topic, byte* payload, unsigned int length) {
  String mensaje = "";
  for (unsigned int i = 0; i < length; i++) {
    mensaje += (char)payload[i];
  }

  Serial.print("Mensaje recibido [");
  Serial.print(topic);
  Serial.print("]: ");
  Serial.println(mensaje);

  // AQUÍ SUCEDE LA ACCIÓN DE SERVIR Y EL BUZZER SONARÁ ÚNICAMENTE AQUÍ
  if (String(topic) == MQTT_TOPIC_CMD && mensaje == "DISPENSAR") {
    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print("Sirviendo comida");

    // Enciende el buzzer mientras sirve la comida
    digitalWrite(PIN_BUZZER, HIGH);

    servoCompuerta.attach(PIN_SERVO, 500, 2400);
    servoCompuerta.write(90); // Abre compuerta
    delay(3000);              // Tiempo de caída

    // Apaga el buzzer al terminar de servir
    digitalWrite(PIN_BUZZER, LOW);

    servoCompuerta.write(0);  // Cierra compuerta
    delay(200);
    servoCompuerta.detach();

    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print("Sistema Listo");
  }
}

void reconnect() {
  while (!client.connected()) {
    Serial.print("Intentando conexión MQTT...");
    if (client.connect("ESP32DispensadorClient", MQTT_USER, MQTT_PASSWORD)) {
      Serial.println("conectado");
      client.subscribe(MQTT_TOPIC_CMD);

      if (bufferCount > 0) {
        Serial.println("[Resiliencia] Sincronizando datos almacenados offline...");
        vaciarBuffer();
      }
    } else {
      Serial.print("falló, rc=");
      Serial.print(client.state());
      Serial.println(" reintentando en 5s");
      delay(5000);
    }
  }
}

void setup() {
  Serial.begin(115200);
  delay(1000);

  pinMode(PIN_POT, INPUT);
  pinMode(PIN_BUZZER, OUTPUT);
  digitalWrite(PIN_BUZZER, LOW);

  Wire.begin(21, 22);
  lcd.init();
  lcd.backlight();
  lcd.clear();
  delay(200);
  lcd.setCursor(0, 0);
  lcd.print("Iniciando...");

  ESP32PWM::allocateTimer(0);
  servoCompuerta.setPeriodHertz(50);
  servoCompuerta.attach(PIN_SERVO, 500, 2400);
  servoCompuerta.write(0);
  delay(300);
  servoCompuerta.detach();

  setup_wifi();
  client.setServer(MQTT_BROKER_IP, MQTT_BROKER_PORT);
  client.setCallback(callback);

  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("Sistema Listo");
}

void loop() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("Reconectando a Wi-Fi...");
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    while (WiFi.status() != WL_CONNECTED) {
      delay(500);
      Serial.print(".");
    }
    Serial.println("\nWi-Fi reconectado.");
  }

  if (!client.connected()) {
    reconnect();
  }
  client.loop();

  unsigned long ahora = millis();
  if (ahora - ultimoEnvio > intervaloEnvio) {
    ultimoEnvio = ahora;

    float nivelPot = leerPotenciometro0a100();
    Serial.print("Valor potenciómetro (0-100): ");
    Serial.println(nivelPot, 0);

    lcd.setCursor(0, 1);
    lcd.print("Pot: ");
    lcd.print(nivelPot, 0);
    lcd.print("     ");

    if (client.connected()) {
      char buffer[10];
      dtostrf(nivelPot, 4, 1, buffer);
      if (client.publish(MQTT_TOPIC_PESO, buffer)) {
        if (bufferCount > 0) {
          vaciarBuffer();
        }
      } else {
        guardarEnBuffer(nivelPot);
      }

      // Publica alerta si el valor simulado está por debajo de 20
      if (nivelPot < 20.0) {
        client.publish(MQTT_TOPIC_ALERTA, "PLATO_VACIO");
      }
    } else {
      guardarEnBuffer(nivelPot);
    }
  }
}
