#include <Wire.h>               // Biblioteca para comunicação I2C
#include <LiquidCrystal_I2C.h>
#include <Adafruit_Sensor.h>
#include <Adafruit_BME280.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <math.h>

// Configurações WiFi e MQTT
#define WIFI_SSID "Inteli.Iot"
#define WIFI_PASS "@Intelix10T#"
#define MQTT_SERVER "industrial.api.ubidots.com"
#define MQTT_CLIENT_ID "ipt_iot"
#define MQTT_USER "BBUS-XEfAqgdsFQrC15wB7hkSHXaxFocCWP"
#define TOPIC "/v1.6/devices/ipt_iot"

// Endereços I2C
#define MMA845x_ADDRESS 0x1C
#define BME280_ADDRESS 0x76

// Pinos
#define LED_R 32
#define LED_G 33
#define LED_B 34
#define BOTAO 4
#define BUZZER 15
#define BOTAO_RESET 5
#define LED_RESET 26

// Intervalos e Sensibilidades
#define INTERVALO_ENVIO 2000
#define SENSIBILIDADE_VIBRACAO 200

// Variáveis do Sistema
bool sistemaLigado = false;
unsigned long ultimoEnvio = 0;
float magnitudeAnterior = 0;
unsigned long inicioUsoCompressor = 0;  // Armazena o momento em que o compressor foi ligado
unsigned long tempoTotalUsoCompressor = 0;  // Tempo total de uso do compressor
float tempPadrao = 25;

// Sensores
Adafruit_BME280 bme;
LiquidCrystal_I2C lcd(0x27, 16, 2);
WiFiClient espClient;
PubSubClient mqttClient(espClient);

// Classes Auxiliares
class Botao {
  private:
    int pino;
    unsigned long ultimoPressionamento, debounceDelay;
    bool estadoAnterior;

  public:
    Botao(int pino, unsigned long debounceDelay = 150)
      : pino(pino), debounceDelay(debounceDelay), ultimoPressionamento(0), estadoAnterior(HIGH) {
      pinMode(pino, INPUT_PULLUP);
    }

    bool foiPressionado() {
      bool estadoAtual = digitalRead(pino);
      if (estadoAtual == LOW && estadoAnterior == HIGH) {
        if (millis() - ultimoPressionamento > debounceDelay) {
          ultimoPressionamento = millis();
          estadoAnterior = estadoAtual;
          return true;
        }
      }
      estadoAnterior = estadoAtual;
      return false;
    }
};

class Vibracao {
  private:
    int endereco;

  public:
    Vibracao(int endereco) : endereco(endereco) {}

    void iniciar() {
      Wire.beginTransmission(endereco);
      Wire.write(0x2A);
      Wire.write(0x01);
      if (Wire.endTransmission() != 0) {
        Serial.println("Erro ao inicializar o MMA845x.");
      }
    }

    float calcularMagnitude() {
      Wire.beginTransmission(endereco);
      Wire.write(0x01);
      Wire.endTransmission(false);
      Wire.requestFrom(endereco, 6);
      if (Wire.available() < 6) return -1;

      int16_t acelX = (Wire.read() << 8 | Wire.read()) >> 2;
      int16_t acelY = (Wire.read() << 8 | Wire.read()) >> 2;
      int16_t acelZ = (Wire.read() << 8 | Wire.read()) >> 2;
      return sqrt(sq(acelX) + sq(acelY) + sq(acelZ));
    }
};

// Instâncias das Classes
Vibracao vibracao(MMA845x_ADDRESS);
Botao botao(BOTAO);

void setColorBasedOnState(float temperatura, bool sistemaLigado, int tempPadrao) {
  int vermelho = 0, verde = 0, azul = 0;
  
  // Limites em porcentagem baseados em tempPadrao
  float limiteBaixo = tempPadrao * 0.5;  // 50% da tempPadrao
  float limiteMedioBaixo = tempPadrao * 0.8;  // 80% da tempPadrao
  float limiteMedioAlto = tempPadrao * 1.2;  // 120% da tempPadrao
  float limiteAlto = tempPadrao * 1.5;  // 150% da tempPadrao

  if (sistemaLigado) {
    if (temperatura <= limiteBaixo) {
      // Faixa de temperatura baixa (azul)
      float frac = temperatura / limiteBaixo;
      vermelho = 0;
      verde = int(255 * frac);
      azul = 255;
    } else if (temperatura > limiteBaixo && temperatura <= limiteMedioBaixo) {
      // Faixa de temperatura média-baixa (verde-azul)
      float frac = (temperatura - limiteBaixo) / (limiteMedioBaixo - limiteBaixo);
      vermelho = 0;
      verde = 255;
      azul = int(255 * (1 - frac));
    } else if (temperatura > limiteMedioBaixo && temperatura <= limiteMedioAlto) {
      // Faixa de temperatura média-alta (verde-vermelho)
      float frac = (temperatura - limiteMedioBaixo) / (limiteMedioAlto - limiteMedioBaixo);
      vermelho = int(255 * frac);
      verde = 255;
      azul = 0;
    } else if (temperatura > limiteMedioAlto && temperatura <= limiteAlto) {
      // Faixa de temperatura alta (vermelho-laranja)
      float frac = (temperatura - limiteMedioAlto) / (limiteAlto - limiteMedioAlto);
      vermelho = 255;
      verde = int(255 * (1 - frac));
      azul = 0;
    } else {
      // Acima do limite (vermelho)
      vermelho = 255;
      verde = 0;
      azul = 0;
    }
  }

  // Aplica as cores diretamente nos LEDs
  analogWrite(LED_R, vermelho);
  analogWrite(LED_G, verde);
  analogWrite(LED_B, azul);
}

void conectarWiFi() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("Conectando ao WiFi...");
    WiFi.begin(WIFI_SSID, WIFI_PASS);
    while (WiFi.status() != WL_CONNECTED) delay(500);
    Serial.println("WiFi conectado!");
  }
}

void conectarMQTT() {
  static unsigned long ultimoTempoTentativaMQTT = 0;
  const unsigned long intervaloTentativaMQTT = 5000;

  if (!mqttClient.connected()) {
    unsigned long tempoAtual = millis();
    if (tempoAtual - ultimoTempoTentativaMQTT >= intervaloTentativaMQTT) {
      ultimoTempoTentativaMQTT = tempoAtual; 
      Serial.print("Conectando ao MQTT...");
      if (mqttClient.connect(MQTT_CLIENT_ID, MQTT_USER, "")) {
        Serial.println("MQTT conectado!");
      } else {
        Serial.print("Erro MQTT: ");
        Serial.println(mqttClient.state());
      }
    }
  }
}

void enviarDados(float temperatura, int compressor) {
  String payload = "{\"temp\":" + String(temperatura) + 
                   ",\"compressor\":" + String(compressor) + 
                   ",\"tempo-de-uso\":" + String(tempoTotalUsoCompressor) + "}"; // Adiciona o tempo de uso
  mqttClient.publish(TOPIC, payload.c_str());
  Serial.println("Dados enviados: " + payload);
}

void atualizarLCD(float temperatura, float magnitude, int compressor) {
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("Temp:");
  lcd.print(temperatura);
  lcd.print("C");
  lcd.setCursor(0, 1);
  lcd.print("Vib:");
  lcd.print(magnitude);
  lcd.print(compressor ? " ON" : " OFF");
}

void configurarReset() {
  pinMode(BOTAO_RESET, INPUT_PULLUP);
  pinMode(LED_RESET, OUTPUT);
}

void verificarReset() {
  static unsigned long tempoPressionado = 0;

  // Verifica o estado do botão
  if (digitalRead(BOTAO_RESET) == LOW) { // Botão pressionado
    if (tempoPressionado == 0) {
      tempoPressionado = millis(); // Marca o tempo inicial
    }

    // Mantém o LED piscando enquanto o botão está pressionado
    if ((millis() - tempoPressionado) % 500 < 250) {
      digitalWrite(LED_RESET, HIGH);
    } else {
      digitalWrite(LED_RESET, LOW);
    }

    // Se o botão for mantido pressionado por mais de 3 segundos, reseta
    if (millis() - tempoPressionado > 3000) {
      Serial.println("Resetando o ESP32...");
      digitalWrite(LED_RESET, LOW);
      ESP.restart(); // Comando para resetar o ESP32
    }
  } else { // Botão liberado
    tempoPressionado = 0; // Reseta o tempo pressionado
    digitalWrite(LED_RESET, LOW); // Garante que o LED fique apagado
  }
}

unsigned long atualizarTempoCompressor(int compressor, unsigned long &inicioUso, unsigned long &tempoTotalHoras) {
  if (compressor == 1) {
    if (inicioUso == 0) {
      // Inicia a contagem de tempo quando o compressor é ligado pela primeira vez
      inicioUso = millis();
    }
    // Atualiza o tempo total continuamente enquanto o compressor está ligado
    tempoTotalHoras += (millis() - inicioUso) / (3600000);
    inicioUso = millis(); // Atualiza o início para o próximo cálculo
  } else {
    // Reseta o marcador de início quando o compressor é desligado
    inicioUso = 0;
  }
  return tempoTotalHoras;
}

// Configuração Inicial
void setup() {
  Serial.begin(115200);
  Wire.begin();

  lcd.init();
  lcd.backlight();

  pinMode(LED_R, OUTPUT);
  pinMode(LED_G, OUTPUT);
  pinMode(LED_B, OUTPUT);
  pinMode(BUZZER, OUTPUT);

  conectarWiFi();
  mqttClient.setServer(MQTT_SERVER, 1883);

  if (!bme.begin(BME280_ADDRESS)) {
    Serial.println("Erro ao inicializar BME280.");
    while (1);
  }
  vibracao.iniciar();
  configurarReset();
}

// Loop Principal
void loop() {
  conectarWiFi();
  if (!mqttClient.connected()) conectarMQTT();
  mqttClient.loop();

  if (botao.foiPressionado()) {
    sistemaLigado = !sistemaLigado;
    Serial.println(sistemaLigado ? "Sistema Ligado" : "Sistema Desligado");
  }

  if (sistemaLigado && millis() - ultimoEnvio >= INTERVALO_ENVIO) {
    ultimoEnvio = millis();
    float temperatura = bme.readTemperature();
    float magnitude = vibracao.calcularMagnitude();
    
    float diferencaMagnitude = fabs(magnitude - magnitudeAnterior);
    int compressor = diferencaMagnitude > SENSIBILIDADE_VIBRACAO ? 1 : 0;
    magnitudeAnterior = magnitude;
    
    setColorBasedOnState(temperatura, sistemaLigado, 20);
    
    unsigned long tempoUso = atualizarTempoCompressor(compressor, inicioUsoCompressor, tempoTotalUsoCompressor);

    digitalWrite(BUZZER, temperatura > 30);
    // Exibe o tempo total de uso do compressor no Serial Monitor
    Serial.print("Tempo total de uso do compressor: ");
    Serial.print(tempoUso); // Exibe em horas
    Serial.println(" Horas");

    atualizarLCD(temperatura, magnitude, compressor);
    enviarDados(temperatura, compressor);
    verificarReset();
  }
}