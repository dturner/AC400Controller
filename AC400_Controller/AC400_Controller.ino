/*********************************************************************
Replacement controller board for Record Power AC400 air filtering system.

Target board: Arduino Nano ESP32 (arduino:esp32:nano_nora).

Requires:
  IRremote 4.x (https://github.com/Arduino-IRremote/Arduino-IRremote)
  secrets.h - copy secrets.example.h and fill in the Wi-Fi details

Web API, served at http://ac400.local once on Wi-Fi:
  GET  /status            current state as JSON
  POST /speed?value=0..3  0 is off, then low, medium, high
  POST /timer?value=0..3  0 cancels, then 1, 2 or 4 hours
Both POSTs reply with the new state.
*********************************************************************/

#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include "secrets.h"

#define HOSTNAME "ac400"

// The frames are decoded by decodeNecFromRawBuffer() below, so only one
// of IRremote's own decoders needs compiling in.
#define DECODE_NEC
#include <IRremote.hpp>

// Header pin labels, unchanged from the AVR wiring. The Nano ESP32 remaps
// Dx/Ax to its own GPIOs, so always use the names rather than GPIO numbers.
#define LED_PIN     D13
#define IR_PIN      D2
#define BEEP_PIN    D5

// KEY_OFF_PIN must be KEY_ON_PIN + 1: pressed() indexes its debounce
// arrays by (pin - KEY_ON_PIN).
#define KEY_ON_PIN  D3
#define KEY_OFF_PIN D4

// The speed relays must be consecutive pins: setSpeed() writes
// SPEED_ZERO_PIN + speed. Zero is never written (setSpeedOff() handles
// that case), so sharing a pin with BEEP_PIN is harmless.
#define SPEED_ZERO_PIN D5
#define SPEED_LOW_PIN D6
#define SPEED_MEDIUM_PIN D7
#define SPEED_HIGH_PIN D8
#define SPEED_COMMON_PIN D9

#define LED_LOW_PIN A0
#define LED_MEDIUM_PIN A1
#define LED_HIGH_PIN A2
#define LED_ONE_PIN A5
#define LED_TWO_PIN A4
#define LED_THREE_PIN A3

#define RELAY_OFF LOW
#define RELAY_ON HIGH
// IRLremote reported the NEC address as 16 bits (0xFF00); IRremote splits
// off the inverted byte, leaving 0x00.
#define KEY_ADDRESS 0x00
#define KEY_OFF_IR  0x90
#define KEY_ON_IR   0x30
#define KEY_TIME_IR 0xA0

#define SPEED_OFF 0
#define SPEED_LOW 1
#define SPEED_MEDIUM 2
#define SPEED_HIGH 3

#define AUTO_OFF 0
#define AUTO_ONE 1
#define AUTO_TWO 2
#define AUTO_THREE 3
#define AUTO_ONE_DURATION 3600000
#define AUTO_TWO_DURATION 7200000
#define AUTO_THREE_DURATION 14400000
//#define AUTO_ONE_DURATION 10000
//#define AUTO_TWO_DURATION 20000
//#define AUTO_THREE_DURATION 40000


int speed = SPEED_OFF;
int auto_off = AUTO_OFF;
unsigned long lastAutoTime;
unsigned long auto_delay;

int buttonState[2] = { LOW, LOW };       // the current reading from the input pin
int lastButtonState[2] = { LOW, LOW };   // the previous reading from the input pin
unsigned long lastDebounceTime[2] = { 0, 0 };  // the last time the output pin was toggled
unsigned long debounceDelay = 50;    // the debounce time; increase if the output flickers

bool lastChangeFromWeb = false;   // was the last change made through the web API?
bool mdnsStarted = false;

WebServer server(80);

void speedOff();

/*
 * This remote chains NEC frames with ~500us between them - shorter than the
 * 1690us space of a data bit - so IRremote never sees an end-of-frame gap and
 * concatenates several frames into one capture it then reports as UNKNOWN.
 * Instead of relying on its framing, scan the raw buffer for a NEC header and
 * decode the first valid frame after it. Returns true and fills aAddress /
 * aCommand on success.
 */
bool decodeNecFromRawBuffer(uint16_t *aAddress, uint8_t *aCommand)
{
  const uint16_t rawlen = IrReceiver.decodedIRData.rawlen;

  for(uint16_t i = 1; i + 2 + 64 <= rawlen; i++)
  {
    unsigned long headerMark = IrReceiver.irparams.rawbuf[i] * MICROS_PER_TICK;
    unsigned long headerSpace = IrReceiver.irparams.rawbuf[i + 1] * MICROS_PER_TICK;
    if(headerMark < 7500 || headerMark > 10500) continue;
    if(headerSpace < 3500 || headerSpace > 5500) continue;

    uint32_t data = 0;
    bool valid = true;
    for(uint8_t bit = 0; bit < 32; bit++)
    {
      unsigned long space = IrReceiver.irparams.rawbuf[i + 3 + (bit * 2)] * MICROS_PER_TICK;
      if(space > 1200 && space < 2200)
        data |= (1UL << bit);          // NEC sends LSB first
      else if(space < 300 || space > 900)
      {
        valid = false;                 // neither a 0 nor a 1
        break;
      }
    }
    if(!valid) continue;

    uint8_t address = data & 0xFF;
    uint8_t addressInv = (data >> 8) & 0xFF;
    uint8_t command = (data >> 16) & 0xFF;
    uint8_t commandInv = (data >> 24) & 0xFF;
    if(addressInv != (uint8_t)~address || commandInv != (uint8_t)~command)
      continue;                        // checksum bytes disagree

    *aAddress = address;
    *aCommand = command;
    return true;
  }
  return false;
}

unsigned long lastKeyTime = 0;   // when the last IR key was acted on

void setup()
{
  // put your setup code here, to run once:
  //Serial.begin(9600);
  //Serial.println("AC400 controller");
  IrReceiver.begin(IR_PIN, DISABLE_LED_FEEDBACK);
  
  pinMode(BEEP_PIN, OUTPUT);
  digitalWrite(BEEP_PIN, LOW);
  
  pinMode(KEY_ON_PIN, INPUT_PULLUP);
  pinMode(KEY_OFF_PIN, INPUT_PULLUP);

  pinMode(LED_LOW_PIN, OUTPUT);
  pinMode(LED_MEDIUM_PIN, OUTPUT);
  pinMode(LED_HIGH_PIN, OUTPUT);
  pinMode(LED_ONE_PIN, OUTPUT);
  pinMode(LED_TWO_PIN, OUTPUT);
  pinMode(LED_THREE_PIN, OUTPUT);
  
  digitalWrite(LED_LOW_PIN, LOW);
  digitalWrite(LED_MEDIUM_PIN, LOW);
  digitalWrite(LED_HIGH_PIN, LOW);
  digitalWrite(LED_ONE_PIN, LOW);
  digitalWrite(LED_TWO_PIN, LOW);
  digitalWrite(LED_THREE_PIN, LOW);

  pinMode(SPEED_LOW_PIN, OUTPUT);
  pinMode(SPEED_MEDIUM_PIN, OUTPUT);
  pinMode(SPEED_HIGH_PIN, OUTPUT);
  pinMode(SPEED_COMMON_PIN, OUTPUT);
  digitalWrite(SPEED_COMMON_PIN, RELAY_OFF);
  speedOff();

  // Don't wait for the connection: the buttons and remote must work
  // without Wi-Fi. The core reconnects on its own if the link drops.
  WiFi.setHostname(HOSTNAME);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  server.on("/status", HTTP_GET, handleStatus);
  server.on("/speed", HTTP_POST, handleSpeed);
  server.on("/timer", HTTP_POST, handleTimer);
  server.onNotFound([]() { server.send(404, "text/plain", "Not found\n"); });
  server.begin();
}

void beep(unsigned long duration, unsigned long pause = 0)
{
  digitalWrite(BEEP_PIN, HIGH);
  delay(duration);
  digitalWrite(BEEP_PIN, LOW);
  if(pause > 0)
    delay(pause);
}

void speedOff()
{
  digitalWrite(SPEED_LOW_PIN, RELAY_OFF);
  digitalWrite(SPEED_MEDIUM_PIN, RELAY_OFF);
  digitalWrite(SPEED_HIGH_PIN, RELAY_OFF);
}

void setSpeed(int new_speed)
{
  digitalWrite(LED_LOW_PIN, LOW);
  digitalWrite(LED_MEDIUM_PIN, LOW);
  digitalWrite(LED_HIGH_PIN, LOW);

  switch(new_speed)
  {
    case SPEED_HIGH:
      //digitalWrite(BEEP_PIN, HIGH);
      //delay(200);
      //digitalWrite(BEEP_PIN, LOW);
      //delay(100);
      beep(200, 100);
    case SPEED_MEDIUM:
      //digitalWrite(BEEP_PIN, HIGH);
      //delay(200);
      //digitalWrite(BEEP_PIN, LOW);
      //delay(100);
      beep(200, 100);
    case SPEED_LOW:
      //digitalWrite(BEEP_PIN, HIGH);
      //delay(200);
      //digitalWrite(BEEP_PIN, LOW);
      speedOff();
      beep(200);
  }
  
  speed = new_speed;
  
  digitalWrite(SPEED_COMMON_PIN, RELAY_ON);
  digitalWrite(SPEED_ZERO_PIN+speed, RELAY_ON);

  switch(speed)
  {
    case SPEED_LOW:
      digitalWrite(LED_LOW_PIN, HIGH);
      break;
    case SPEED_MEDIUM:
      digitalWrite(LED_MEDIUM_PIN, HIGH);
      break;
    case SPEED_HIGH:
      digitalWrite(LED_HIGH_PIN, HIGH);
      break;
  }
}

void setNextSpeed()
{
  int s = speed+1;
  if(s > SPEED_HIGH)
    s = SPEED_LOW;
  setSpeed(s);
}

void setSpeedOff()
{
  speed = SPEED_OFF;
  auto_off = AUTO_OFF;
  //digitalWrite(BEEP_PIN, HIGH);
  //delay(500);
  //digitalWrite(BEEP_PIN, LOW);
  beep(500);
  speedOff();
  digitalWrite(SPEED_COMMON_PIN, RELAY_OFF);
  digitalWrite(LED_LOW_PIN, LOW);
  digitalWrite(LED_MEDIUM_PIN, LOW);
  digitalWrite(LED_HIGH_PIN, LOW);
  digitalWrite(LED_ONE_PIN, LOW);
  digitalWrite(LED_TWO_PIN, LOW);
  digitalWrite(LED_THREE_PIN, LOW);
}

void setAuto(int new_auto)
{
  if(speed == SPEED_OFF)
    new_auto = AUTO_OFF;
  digitalWrite(LED_ONE_PIN, LOW);
  digitalWrite(LED_TWO_PIN, LOW);
  digitalWrite(LED_THREE_PIN, LOW);

  switch(new_auto)
  {
    case AUTO_THREE:
      //digitalWrite(BEEP_PIN, HIGH);
      //delay(50);
      //digitalWrite(BEEP_PIN, LOW);
      //delay(100);
      beep(50, 100);
      beep(200, 100);
      beep(200, 100);
      beep(200);
      break;
    case AUTO_TWO:
      //digitalWrite(BEEP_PIN, HIGH);
      //delay(50);
      //digitalWrite(BEEP_PIN, LOW);
      //delay(100);
      //beep(50, 50);
      //beep(200, 100);
      //beep(50, 50);
      //beep(200);
      beep(50, 100);
      beep(200, 100);
      beep(200);
      break;
    case AUTO_ONE:
      //digitalWrite(BEEP_PIN, HIGH);
      //delay(50);
      //digitalWrite(BEEP_PIN, LOW);
      //delay(100);
      //digitalWrite(BEEP_PIN, HIGH);
      //delay(50);
      //digitalWrite(BEEP_PIN, LOW);
      beep(50, 100);
      beep(200);
      break;
    case AUTO_OFF:
      //digitalWrite(BEEP_PIN, HIGH);
      //delay(50);
      //digitalWrite(BEEP_PIN, LOW);
      beep(50, 100);
      beep(50);
      break;
  }

  auto_off = new_auto;
  lastAutoTime = millis();
  
  switch(auto_off)
  {
    case AUTO_ONE:
      digitalWrite(LED_ONE_PIN, HIGH);
      auto_delay = AUTO_ONE_DURATION;
      break;
    case AUTO_TWO:
      digitalWrite(LED_TWO_PIN, HIGH);
      auto_delay = AUTO_TWO_DURATION;
      break;
    case AUTO_THREE:
      digitalWrite(LED_THREE_PIN, HIGH);
      auto_delay = AUTO_THREE_DURATION;
      break;
  }
  //Serial.print("Auto off at ");
  //Serial.print(auto_delay);
  //Serial.print(" current time ");
  //Serial.println(lastAutoTime);
}

void setNextAuto()
{
  int a = auto_off + 1;
  if(a > AUTO_THREE)
    a = AUTO_OFF;
  setAuto(a);
}

void sendStatus()
{
  unsigned long timerRemaining = 0;
  unsigned long timerElapsed = millis() - lastAutoTime;
  if(auto_off != AUTO_OFF && timerElapsed < auto_delay)
    timerRemaining = (auto_delay - timerElapsed) / 1000;

  String json = "{\"speed\":" + String(speed) +
                ",\"timer\":" + String(auto_off) +
                ",\"timer_remaining_s\":" + String(timerRemaining) +
                ",\"source\":\"" + (lastChangeFromWeb ? "web" : "manual") + "\"}\n";
  server.send(200, "application/json", json);
}

// Reads the "value" argument (query string or form body) as 0..3.
// Returns -1 and sends a 400 if it is missing or out of range.
int levelArg()
{
  String value = server.arg("value");
  if(value.length() != 1 || value[0] < '0' || value[0] > '3')
  {
    server.send(400, "text/plain", "value must be 0, 1, 2 or 3\n");
    return -1;
  }
  return value[0] - '0';
}

void handleStatus()
{
  sendStatus();
}

void handleSpeed()
{
  int level = levelArg();
  if(level < 0)
    return;
  if(level == SPEED_OFF)
    setSpeedOff();
  else
    setSpeed(level);
  lastChangeFromWeb = true;
  sendStatus();
}

void handleTimer()
{
  int level = levelArg();
  if(level < 0)
    return;
  setAuto(level);
  lastChangeFromWeb = true;
  sendStatus();
}

bool pressed(uint8_t key_pin)
{
  // read the state of the switch into a local variable:
  int reading = !digitalRead(key_pin);
  // If the switch changed, due to noise or pressing:
  if (reading != lastButtonState[key_pin - KEY_ON_PIN])
  {
    // reset the debouncing timer
    lastDebounceTime[key_pin - KEY_ON_PIN] = millis();
    // save the reading. Next time through the loop, it'll be the lastButtonState:
    lastButtonState[key_pin - KEY_ON_PIN] = reading;
    return false;
  }

  if ((millis() - lastDebounceTime[key_pin - KEY_ON_PIN]) > debounceDelay)
  {
    // whatever the reading is at, it's been there for longer than the debounce
    // delay, so take it as the actual current state:

    // if the button state has changed:
    if (reading != buttonState[key_pin - KEY_ON_PIN])
    {
      buttonState[key_pin - KEY_ON_PIN] = reading;

      // only if the new button state is HIGH
      if (buttonState[key_pin - KEY_ON_PIN] == HIGH)
      {
        return true;
      }
    }
  }
  return false;
}

void loop()
{
  if(pressed(KEY_ON_PIN))
  {
    lastChangeFromWeb = false;
    setNextSpeed();
  }
  if(pressed(KEY_OFF_PIN))
  {
    lastChangeFromWeb = false;
    setSpeedOff();
  }
  
  if(IrReceiver.decode()) // have we received an IR signal?
  {
    uint16_t address = 0;
    uint8_t command = 0;
    bool decoded = decodeNecFromRawBuffer(&address, &command);

    //Serial.print("rawlen=");
    //Serial.print(IrReceiver.decodedIRData.rawlen);
    //Serial.print(" address=");
    //Serial.print(address, HEX);
    //Serial.print(" command=");
    //Serial.println(command, HEX);

    // One capture holds several repeats of the same frame, and holding a
    // button produces captures back to back, so ignore anything arriving
    // shortly after an accepted key.
    bool toosoon = (millis() - lastKeyTime) < 400;
    if(decoded && !toosoon && address == KEY_ADDRESS)
    {
      lastKeyTime = millis();
      lastChangeFromWeb = false;
      switch(command)
      {
        case KEY_ON_IR:
          setNextSpeed();
          break;
        case KEY_OFF_IR:
          setSpeedOff();
          break;
        case KEY_TIME_IR:
          setNextAuto();
          break;
      }
    }
    IrReceiver.resume();
  }

  // mDNS needs an IP address, so start it once the first connection is up.
  if(!mdnsStarted && WiFi.status() == WL_CONNECTED)
    mdnsStarted = MDNS.begin(HOSTNAME);
  server.handleClient();

  if(auto_off != AUTO_OFF)
  {
    unsigned long auto_elapsed = millis() - lastAutoTime;
    if(auto_elapsed > auto_delay)
    {
      setSpeedOff();
      //Serial.print("Auto off at ");
      //Serial.print(mil);
      //Serial.print(" after waiting for ");
      //Serial.print(auto_elapsed);
      //Serial.println(" millis");
    }
    else
    {
      unsigned long auto_remaining = auto_delay - auto_elapsed;
      if(auto_off == AUTO_THREE && auto_remaining < AUTO_TWO_DURATION)
      {
        setAuto(AUTO_TWO);
      }
      else if(auto_off == AUTO_TWO && auto_remaining < AUTO_ONE_DURATION)
      {
        setAuto(AUTO_ONE);
      }
    }
  }
}
