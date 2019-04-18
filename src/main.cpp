#include <Arduino.h>
#include <SPI.h>
#include <PID_v1.h>
#include <ESP8266WiFi.h>
//#include <DNSServer.h>
#include <WiFiClient.h>
#include <ESP8266WebServer.h>
#include <ESP8266HTTPUpdateServer.h>
//#include <ESP8266mDNS.h>

#include <EEPROM.h>
#include "EEPROMAnything.h"

#include <ESP8266HTTPUpdateServer.h>

// Configuration Start

// These buttons are exposed on the nodemcu dev board
const uint8_t key_user = 16; // What can we do with this button?
const uint8_t key_flash = 13; // If pressed within 5 seconds of power on, enter admin mode

const uint8_t ledHTTP = 4;     // Toggled on HTTP Status
const uint8_t ledCONNECTED = 12; // Toggled on when AP connected

const uint8_t SSR_OUTPUT = 5; // This is where the SSR is connected

// End Pin Assignment


// This structure should not grow larger than 1024 bytes.
struct settings_t
{
  uint8_t initialized;       // If not "1", then we have not yet initialized with defaults
  char ssid[33];         // One more byte than required; String needs to be null terminated
  char ssidPassword[65]; // One more byte than required; String needs to be null terminated
  uint8_t ipMode; // 0 = Dynamic, 1 = Static
  uint8_t ipAddress[4]; // 255.255.255.255
  uint8_t ipGateway[4]; // 255.255.255.255
  uint8_t ipSubnet[4];  // 255.255.255.255
} settings;


int requestTTL = 120;

const char* host = "eflow";

const byte DNS_PORT = 53;
ESP8266WebServer httpServer ( 80 );
ESP8266HTTPUpdateServer httpUpdater;
//DNSServer dnsServer;

boolean deviceAdmin = 0;

// Define the LED state for ledHTTP
//   This is used for blinking the LED with a non-blocking method
boolean ledHTTPState = LOW;
unsigned long    ledHTTPStateMills = 0;
long    ledHTTPStateInterval = 250; // How fast to blink the LED

unsigned long secretRandNumber; // We will generate a new secret on startup.

const int numReadings = 5;
int readIndex_A = 0;                // the index of the current reading
int readIndex_B = 0;                // the index of the current reading

//Input: The variable we're trying to control (double)
//Output: The variable that will be adjusted by the pid (double)
//Setpoint: The value we want to Input to maintain (double)
double Setpoint, Input, Output;

//Specify the links and initial tuning parameters
//double Kp = 300, Ki = 0.05, Kd = 20;
//double Kp = 100, Ki = 0.01, Kd = 0;
//double Kp = 30.0, Ki = 0.1, Kd = 14; // This works. There's some overshoot, but it works.
double Kp = 40.0, Ki = 0.01, Kd = 5;

double Kp_agressive = 999.0, Ki_agressive = 0.01, Kd_agressive = 0;
PID myPID(&Input, &Output, &Setpoint, Kp, Ki, Kd, DIRECT);

uint16_t startup_sec = 90; // Default 90
uint16_t startup_temp = 80;



int WindowSize = 1000; //
unsigned long windowStartTime;


float readings_A[numReadings];      // the readings from the analog input
float readings_B[numReadings];      // the readings from the analog input

float sensorA = 0;
float sensorB = 0;

float sensorTemperature = 0;

unsigned long previousMillis1000 = 0;
unsigned long previousMillis100 = 0;

bool heaterDuty[10] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
int8_t heaterDutyIndex = -1;

/*
 * 0 = Nothing going on
 * 1 = Init
 * 2 = In progress
 */
uint8_t processEnable = 0;

uint16_t safeTemperature = 50; // Don't allow oven to be enabled unless it first cools to this temperature

struct reflowStatsProfile_t
{
  uint16_t sensorA;  // Reserved
  uint16_t sensorB;   // Reserved
  uint16_t Setpoint;   // Reserved
  uint16_t time;   // Reserved
  uint16_t reflowTime;   // Time since start of reflow process
};

const uint16_t reflowStatsProfileLength = 500;
struct reflowStats_t
{
  uint8_t run; // 0 = Unexecuted, 1 = Completed, 2, In Progress, 3 = Aborted, 4 = Abnormal Error
  uint8_t reflowProfilePrevious; // What reflow profile was selected?
  uint8_t reflowProfileNext; // What reflow profile is selected?
  reflowStatsProfile_t profile[reflowStatsProfileLength]; // 900 positions to save up to 1200 seconds (15 minutes). uInt for each temerature sensor, Input and Setpoint.
} ;

reflowStats_t reflowStats;

const uint8_t reflowProfileNameLength = 50;

struct reflowProfile_t
{
  char name[reflowProfileNameLength];                 // Name of profile
  uint16_t sort;                 // Sort index
  uint16_t profileRamp[2];       // Time / Temperature
  uint16_t profilePreheat[2];    // Time / Temperature
  uint16_t profileRampToPeak[2]; // Time / Temperature
  uint16_t profileReflow[2];     // Time / Temperature
  uint16_t profileCooling[2];    // Time / Temperature
  uint16_t profileFinishing[2];  // Time / Temperature -- Ramp down to temperature that would be safe to touch the board.
};

reflowProfile_t reflowProfile[4];

String systemMessage = "";
#include "defaults.h"
#include "sensors.h"
#include "reflow.h"
#include "ssrControl.h"
#include "utility.h"
#include "webAdmin.h"
#include "webApplication.h"
#include "webUtilities.h"

void setup() {

  Serial.begin(115200);

  EEPROM.begin(1024); // 512 bytes should be more than enough (famous last words)
  loadSettings();

  pinMode( SSR_OUTPUT, OUTPUT);
  pinMode( key_flash, INPUT_PULLUP );

  //-- Start PID Setup
  windowStartTime = millis();

  //initialize the variables we're linked to
  Setpoint = 1;

  //tell the PID to range between 0 and the full window size
  myPID.SetOutputLimits(0, WindowSize);
  //-- END PID Setup

  //turn the PID on
  myPID.SetMode(AUTOMATIC);


  delay(5000);
  // Set deviceAdmin to one if key_flash is depressed. Otherwise, use defaults.
  if (digitalRead( key_flash ) == 0) {
    deviceAdmin = 1;
    pinMode( key_flash, OUTPUT );
  } else {
    pinMode( key_flash, OUTPUT );
  }


  if (deviceAdmin) {
    WiFi.mode(WIFI_AP);
    WiFi.softAP("eflow_admin", "eflow_admin");
    WiFi.mode(WIFI_AP);
    WiFi.config ( IPAddress(192, 168, 4, 1), IPAddress(192, 168, 4, 1), IPAddress(255, 255, 255, 0)) ;
    delay(10);

    // if DNSServer is started with "*" for domain name, it will reply with
    // provided IP to all DNS request
    //dnsServer.start(DNS_PORT, "*", IPAddress(192, 168, 4, 1));

    //WiFi.printDiag(Serial);
    Serial.println ( "Entering admin mode." );

    Serial.print ( "IP address: " );
    Serial.println ( WiFi.softAPIP() );
    printAPMacAddress();

  //  if ( MDNS.begin ( host ) ) {
  //    Serial.println ( "MDNS responder started" );
  //  } else {
  //    Serial.println ( "MDNS responder NOT started" );
  //  }

    // We are using the amount of time required to connect to the AP as the seed to a random number generator.
    //   We should look for other ways to improve the seed. This should be "good enough" for now.

    httpServer.on ( "/", handleAdminFrameset );
    httpServer.on ( "/leftnav", handleAdminNav );
    httpServer.on ( "/conf/wifi", handleAdminConfWifi );
    httpServer.on ( "/conf/network", handleAdminConfNetwork );
    httpServer.on ( "/conf/accounts", handleAdminConfAccounts );
    httpServer.on ( "/conf/sensors", handleAdminConfSensors );
    httpServer.on ( "/system/defaults", handleAdminDefaults );
    httpServer.on ( "/system/settings", handleAdminSettings );
    httpServer.on ( "/system/restart", handleAdminRestart);
    httpServer.on ( "/system/apply", handleAdminApply);
    httpServer.on ( "/eflow.css", handleCSS);

    httpServer.onNotFound ( handleNotFound );

    httpUpdater.setup(&httpServer);

    httpServer.begin();
    Serial.println ( "HTTP server started" );

  //  MDNS.addService("http", "tcp", 80);
    Serial.printf("HTTPUpdateServer ready! Open http://%s.local/update in your browser\n", host);

  } else {

    Serial.print("Connecting to SSID : ");
    Serial.println (settings.ssid);

    WiFi.begin ( settings.ssid, settings.ssidPassword );
    WiFi.mode ( WIFI_STA );



    // Documentation says this is supposed to come before WiFi.begin, but when it is there -- it doesn't work. WHY?!?!?!
    if (settings.ipMode == 1) { // 0 = Dynamic, 1 = Static
      WiFi.config ( settings.ipAddress, settings.ipGateway, settings.ipSubnet) ;
    }

    //Serial.println ( "" );

    //EEPROM_readAnything(0, settings);

    // Wait for connection
    while ( WiFi.status() != WL_CONNECTED ) {
      delay ( 500 );
      Serial.print ( "." );
    }

    digitalWrite ( ledCONNECTED, 1 );

    WiFi.printDiag(Serial);

    Serial.print ( "IP address: " );
    Serial.println ( WiFi.localIP() );
    //printMacAddress();

  //  if ( MDNS.begin ( host ) ) {
  //    Serial.println ( "MDNS responder started" );
    //} else {
    //  Serial.println ( "MDNS responder NOT started" );
    //}


    // We are using the amount of time required to connect to the AP as the seed to a random number generator.
    //   We should look for other ways to improve the seed. This should be "good enough" for now.
    randomSeed(micros());
    secretRandNumber = random(2147483646); // Full range of long 2147483647
    Serial.println("Secret: " + String(secretRandNumber));

    //httpServer.on ( "/", handleRoot );
    httpServer.on ( "/", handleReflowFrameset );
    httpServer.on ( "/topnav", handleReflowNav );
    httpServer.on ( "/process/start", handleProcessStart );
    httpServer.on ( "/process/stop", handleProcessStop );
    httpServer.on ( "/process/conf", handleProcessConfigure );
    httpServer.on ( "/process/conf/save/global", handleProcessConfigureSaveGlobal );
    httpServer.on ( "/process/chart", handleReflowChart );
    httpServer.on ( "/process/data.csv", handleProcessData );
    httpServer.on ( "/restart", handleSystemRestart );

    httpServer.on ( "/externalScript.js", handleExternalScriptJS );
    httpServer.on ( "/json/sensors", handleJSONSensors );
    httpServer.on ( "/eflow.css", handleCSS);
    httpServer.on ( "/blank.html", handleBlank);

    httpServer.onNotFound ( handleNotFound );

    httpUpdater.setup(&httpServer);

    httpServer.begin();
    Serial.println ( "HTTP server started" );

    //MDNS.addService("http", "tcp", 80);
    Serial.printf("HTTPUpdateServer ready! Open http://%s.local/update in your browser\n", host);

  }



}

void handleRoot2() {
  httpServer.send(200, "text/plain", "hello from esp8266!");
}
void dispatchSecond( void ) {
  // We may use this for debug output

}
void dispatchers ( void ) {
  // Call dispatchSecond once a second
  unsigned long currentMillis1000 = millis();
  if (currentMillis1000 - previousMillis1000 >= 1000) {
    previousMillis1000 = currentMillis1000;

    dispatchSecond();
    dispatchProcessPerSecond();
  }

  // Call dispatch100ms every 100ms (1/10 sec)
  unsigned long currentMillis100 = millis();
  if (currentMillis100 - previousMillis100 >= 500) {
    previousMillis100 = currentMillis100;

    dispatch100ms();
  }
}

void loop() {


  updateSensors();

  // Call the timer dispatchers
  dispatchers();

  // Start Pid Control
  Input = (sensorA + sensorB) / 2;
  Serial.println ( Input );
  Serial.println ( analogRead(0) );

  myPID.Compute();

  // Handle TCP Server
  httpServer.handleClient();
  //dnsServer.processNextRequest();
  delay ( 50 );


  if (deviceAdmin) {
    unsigned long ledHTTPCurrentMills = millis();

    if (ledHTTPCurrentMills - ledHTTPStateMills > ledHTTPStateInterval) {
      ledHTTPStateMills = ledHTTPCurrentMills;

      if (ledHTTPState) {
        ledHTTPState = 0;
      } else {
        ledHTTPState = 1;
      }
      digitalWrite( ledCONNECTED, ledHTTPState );
    }

    // If we've been in admin mode for 30 minutes, reboot ESP to get out of
    //   admin mode.
    if (millis() > 1800000) {
      ESP.reset();
    }
  }



}
