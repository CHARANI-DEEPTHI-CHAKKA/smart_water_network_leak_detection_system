#include <WiFi.h>
#include <WiFiUdp.h>

#define NODE_ID 1
#define FLOW_SENSOR_PIN 27
#define FLOW_FACTOR 7.5
#define INTERVAL 5000

const char* ssid = "Charani";
const char* password = "12345678";
const char* raspberryPiIP = "10.87.94.6";
const unsigned int udpPort = 5000;

WiFiUDP udp;
volatile unsigned long pulseCount = 0;

void IRAM_ATTR countFlowPulse()
{
  pulseCount++;
}

void connectWiFi()
{
  WiFi.begin(ssid, password);

  while (WiFi.status() != WL_CONNECTED)
  {
    delay(500);
    Serial.print(".");
  }

  Serial.println();
  Serial.println("Wi-Fi Connected!");
  Serial.print("ESP32 IP: ");
  Serial.println(WiFi.localIP());
}

void setup()
{
  Serial.begin(115200);

  pinMode(FLOW_SENSOR_PIN, INPUT);

  attachInterrupt(
    digitalPinToInterrupt(FLOW_SENSOR_PIN),
    countFlowPulse,
    RISING
  );

  Serial.println("SMART WATER NETWORK - NODE 1");

  connectWiFi();

  udp.begin(udpPort);
  Serial.println("UDP Ready");
}

void loop()
{
  static unsigned long previousTime = 0;

  if (millis() - previousTime >= INTERVAL)
  {
    previousTime = millis();

    noInterrupts();
    unsigned long pulses = pulseCount;
    pulseCount = 0;
    interrupts();

    float frequency = pulses / (INTERVAL / 1000.0);
    float flowRate = frequency / FLOW_FACTOR;

    Serial.printf(
      "Node %d Flow: %.2f mL/sec\n",
      NODE_ID,
      flowRate
    );

    if (WiFi.status() == WL_CONNECTED)
    {
      String packet =
        "NODE" + String(NODE_ID) + "," + String(flowRate, 2);

      udp.beginPacket(raspberryPiIP, udpPort);
      udp.print(packet);

      if (udp.endPacket() == 1)
      {
        Serial.println("Sent: " + packet);
      }
      else
      {
        Serial.println("UDP Send Failed!");
      }
    }
    else
    {
      connectWiFi();
      udp.begin(udpPort);
    }
  }
}
