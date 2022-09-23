/*
 * Example of initiating HTTPS sessions & instantiating BLE scans for sending BLE data over network calls
 *
 */

#define TINY_GSM_MODEM_SIM7000
#define TINY_GSM_RX_BUFFER 1024 // Set RX buffer to 1Kb
#define SerialAT Serial1

// See all AT commands as they are being run (this is how the sausage is made, very noisy)
//#define DUMP_AT_COMMANDS

/*
 * Sections enabled
 */
#define TINY_GSM_TEST_GPRS    true

// set GSM PIN, if any
#define GSM_PIN ""

// Support libraries
#include <TinyGsmClient.h>
#include <SSLClient.h>
#include <ArduinoHttpClient.h>
#include <SPI.h>
#include <SD.h>
#include <Ticker.h>
#include <UrlEncode.h>
#include <ArduinoJson.h>

#include <sstream>
#include <vector>
#include <sstream>

#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEScan.h>
#include <BLEAdvertisedDevice.h>
#include <BLEBeacon.h>


#define ENDIAN_CHANGE_U16(x) ((((x)&0xFF00) >> 8) + (((x)&0xFF) << 8))

/*
 * The pvvx (Custom) format has the following Service Data structure:
 * 
 *    uint8_t     MAC[6]; // [0] - lo, .. [6] - hi digits
 *    int16_t     temperature;    // x 0.01 degree
 *    uint16_t    humidity;       // x 0.01 %
 *    uint16_t    battery_mv;     // mV
 *    uint8_t     battery_level;  // 0..100 %
 *    uint8_t     counter;        // measurement count
 *    uint8_t     flags;  // GPIO_TRG pin (marking "reset" on circuit board) flags: 
 *                        // bit0: Reed Switch, input
 *                        // bit1: GPIO_TRG pin output value (pull Up/Down)
 *                        // bit2: Output GPIO_TRG pin is controlled according to the set parameters
 *                        // bit3: Temperature trigger event
 *                        // bit4: Humidity trigger event
 * 
 * From this we get the #define lines below:
 */
 
#define TEMP_LSB    6
#define TEMP_MSB    7
#define HUMID_LSB   8
#define HUMID_MSB   9
#define BATTV_LSB   10
#define BATTV_MSB   11
#define BATTP_BYTE  12

/*
 * How long do we want to wait to collect advertisements?
 *  By default the Xiaomi sensors are sending advertisements every 2.5 seconds.
 *  This can be reconfigured in the firmware using the web tool above.
 *  When we scan for 5 seconds we ought to catch all of the sensors, but not for sure!
 *  
 *  NOTE: During the scan our code is halted in the loop!
 */
#define SCAN_TIME  10 // seconds

/*
 * Do you want to see all advertisements that are detected during the scan?
 * This flag will enable high-level logging of all of the advertisements seen.
 */
#define SHOW_ALL  false

/*
 * Do you want the temperature value in C or F?
 * Set true for metric system; false for imperial
 */
boolean METRIC = false;

BLEScan *pBLEScan;

/*
 * Your GPRS credentials, if any
 */
const char apn[]  = "super";     //SET TO YOUR APN
const char gprsUser[] = "";
const char gprsPass[] = "";


/*
 * Endpoint server details.  Note this must be defined AFTER including SSLClient.h
 */

//Uncomment to send payload to webhook.site for testing
//#define TEST_URL_PAYLOAD


// Deliver data to proper endpoint
const char server[] = "pizza-temp-service-1337.twil.io";
// Setup services below via the Twilio API or Twilio Console
const char resource[] = "/tempEvent";
const int  port = 443;
const char twilioAPIKey[] = "";
const char twilioAPISecret[] = "";

struct SensorData {
  String deviceName;
  float temperature;
};

int xiaomi_device_count = 0;

/*
 * The file below you will need to create based on the website you would like to connect to securely.
 * This file contains the trust anchor to ensure the site you are connecting to is valid.
 *
 * Use the website: https://openslab-osu.github.io/bearssl-certificate-utility/
 * Enter the website to pull the certs from the URL, cut and past the result into the file below.
 */
#include "ca_cert.h" // sync.twilio.com site cert


TinyGsm modem(SerialAT);

/*
 * HTTPS Transport setup: This next section sets up for HTTPS traffic.
 */

TinyGsmClient modem_client(modem);
SSLClient secure_presentation_layer(modem_client, TAs, (size_t)TAs_NUM, A7);
HttpClient https_client = HttpClient(secure_presentation_layer, server, port);


/*
 * Set a bunch of necessary PIN outs to make modem operate correctly.  Don't mess with this section unless you know what you are doing.
 */

#define uS_TO_S_FACTOR 1000000ULL  // Conversion factor for micro seconds to seconds
#define TIME_TO_SLEEP  2          // Time ESP32 will go to sleep (in seconds)

#define UART_BAUD   115200
#define PIN_DTR     25
#define PIN_TX      27
#define PIN_RX      26
#define PWR_PIN     4

#define SD_MISO     2
#define SD_MOSI     15
#define SD_SCLK     14
#define SD_CS       13
#define LED_PIN     12

/*
 * Let's get going!
 */

 /**************************************************************************************
 *
 * Helper Functions
 *
 **************************************************************************************/

 String addJsonText (String jsonString, String newLabel, String newData) {
  // Expecting minimum jsonString of "", will add code to better identify string validation later
  // Quotes added to data value
  if (jsonString == "") {
    String newJson = "{\"" + newLabel + "\":\"" + newData + "\"}";
    jsonString = newJson;
  }
  else {
    jsonString.remove(jsonString.length()-1,1);
    String newJson = ", \"" + newLabel + "\":\"" + newData + "\"}";
    jsonString = jsonString + newJson;
  }
  return jsonString;
 }

void setup()
{
    // Set console baud rate for Serial Window from IDE
    Serial.begin(115200);
    delay(10);

    // Set LED OFF
    pinMode(LED_PIN, OUTPUT);
    digitalWrite(LED_PIN, HIGH);

    // Reset Modem
    pinMode(PWR_PIN, OUTPUT);
    digitalWrite(PWR_PIN, HIGH);
    delay(300);
    digitalWrite(PWR_PIN, LOW);

    // Check if SD card present
    Serial.println("\n\nChecking for SD Card...");
    SPI.begin(SD_SCLK, SD_MISO, SD_MOSI, SD_CS);
    if (!SD.begin(SD_CS)) {
        Serial.println("No SD Card found.");
    } else {
        uint32_t cardSize = SD.cardSize() / (1024 * 1024);
        String str = "SDCard Size: " + String(cardSize) + "MB";
        Serial.println(str);
    }

    Serial.println("\nWait 2 seconds for bootstrap to complete...");
    delay(2);

    SerialAT.begin(UART_BAUD, SERIAL_8N1, PIN_RX, PIN_TX);

    // Restart takes quite some time
    // To skip it, call init() instead of restart()
    Serial.println("Initializing modem...");
    if (!modem.restart()) {
        Serial.println("Failed to restart modem, attempting to continue without restarting");
    }

    initBluetooth();
}

void initBluetooth()
{
    BLEDevice::init("");
    pBLEScan = BLEDevice::getScan(); //create new scan
    pBLEScan->setActiveScan(true); //active scan uses more power, but get results faster
    pBLEScan->setInterval(0x50);
    pBLEScan->setWindow(0x30);
}


/*
 * Start main loop
 */

void loop()
{
  String name = modem.getModemName();
  delay(500);
  Serial.println("Modem Name: " + name);

  String modemInfo = modem.getModemInfo();
  delay(500);
  Serial.println("Modem Info: " + modemInfo);

  // Turn of GPS for this example as it is not needed
  // Set SIM7000G GPIO4 LOW ,turn off GPS power
  // CMD:AT+SGPIO=0,4,1,0
  // Only in version 20200415 is there a function to control GPS power
  modem.sendAT("+SGPIO=0,4,1,0");
  if (modem.waitResponse(10000L) != 1) {
      DBG(" SGPIO=0,4,1,0 false ");
  }

#if TINY_GSM_TEST_GPRS
  // Unlock your SIM card with a PIN if needed
  if ( GSM_PIN && modem.getSimStatus() != 3 ) {
      modem.simUnlock(GSM_PIN);
      DBG("SIM Check...");
  }
#endif


  /*
   * setNetworkMode:
    2 Automatic
    13 GSM only
    38 LTE only
    51 GSM and LTE only
   */
  String res;
  res = modem.setNetworkMode(38);
  if (res != "1") {
      DBG("setNetworkMode  false ");
      return ;
  }
  delay(200);

  /*
   * setPreferredMode:
    1 CAT-M
    2 NB-Iot
    3 CAT-M and NB-IoT
  */
  res = modem.setPreferredMode(1);
  if (res != "1") {

      DBG("setPreferredMode  false ");
      return ;
  }
  delay(200);

  
  
  Serial.println("\n\n\nWaiting for network...");
  if (!modem.waitForNetwork()) {
      delay(10000);
      return;
  }
  
  if (modem.isNetworkConnected()) {
      Serial.println("Network connected");
  }
  
  Serial.println("\n---Setting up GPRS connection---\n");
  Serial.println("Connecting to: " + String(apn));
  if (!modem.gprsConnect(apn, gprsUser, gprsPass)) {
      delay(10000);
      return;
  }
  
  Serial.print("GPRS status: ");
  if (modem.isGprsConnected()) {
      Serial.println("connected!");
  } else {
      Serial.println("not connected");
  }
  
  //Serial.print("Adding 8 sec delay for connectivity");
  //delay(8000);
  
  String cop = modem.getOperator();
  Serial.println("Operator:" + String(cop));
  
  int csq = modem.getSignalQuality();
  Serial.println("Signal quality:" + String(csq) + "\n\n");
  
  /*
   * At this point, you can pull data from pins/sensors/GPS, etc...
   */
  
  SensorData sensorData = executeBLEScan();
  
  // Only send the network request if one of the specified sensors was picked up
  if (xiaomi_device_count > 0) {
    /*
     * Assemble data into a JSON payload
     * 
     * Our team just skipped over this part -- for simplicity we just added values as query parameters in the request string
     */
    //String jsonData = "";
    //jsonData = addJsonText(jsonData, "build", "1.05");
    //jsonData = addJsonText(jsonData, "operator", cop);
    //jsonData = addJsonText(jsonData, "signal", String(csq));
    //jsonData = addJsonText(jsonData, "latitude", "47.321");
    //jsonData = addJsonText(jsonData, "longitude", "-122.331");
    
    
    // Add some meta data
    //jsonData = "{created_by: lilygo_tracker, data:" + jsonData + "}";
    
    // URL encode JSON string:
    //String jsonDataEnc = urlEncode(jsonData);
    
    // Add "Data=" header needed for Twilio Sync, and a TTL to expire the data (10 minutes)
    //jsonDataEnc = "Data=" + jsonDataEnc + "&Ttl=600";
    
    // Package ready to deliver to Twilio!
    
    /*
     * Start HTTPS request
     */
    Serial.println("Building HTTPS request");
    https_client.beginRequest();
    
    // https://pizza-temp-service-1337.twil.io/tempEvent?PizzaTempInF=65&SensorId=sensor02%27
    std::stringstream ss;
    ss << resource << "?PizzaTempInF=" << sensorData.temperature << "&SensorId=" << sensorData.deviceName;
    
    https_client.post(ss.str().c_str());
    https_client.sendBasicAuth(twilioAPIKey,twilioAPISecret);
    https_client.sendHeader("Content-Type", "application/json");
    //https_client.sendHeader("Content-Type", "application/x-www-form-urlencoded");
    //https_client.sendHeader("Content-Length", jsonDataEnc.length());
    //https_client.print(jsonDataEnc);
    https_client.endRequest();
    
    
    // read the status code and body of the response
    Serial.print("\nAwaiting response...\n\n");
    
    int statusCode = https_client.responseStatusCode();
    String response = https_client.responseBody();
    Serial.print("Status code: ");
    Serial.println(statusCode);
    Serial.print("Response: ");
    Serial.println(response);
  }


  Serial.print("\nWaiting 2 seconds to make the call again...");
  delay(2000); // Wait 60 seconds before going again

#if TINY_GSM_TEST_GPRS
  modem.gprsDisconnect();
  if (!modem.isGprsConnected()) {
      Serial.println("GPRS disconnected");
  } else {
      Serial.println("GPRS disconnect: Failed.");
  }
#endif


  Serial.println("Putting modem to sleep");
  esp_sleep_enable_timer_wakeup(TIME_TO_SLEEP * uS_TO_S_FACTOR);
  delay(200);
  esp_deep_sleep_start();

  // Do nothing forevermore
  while (true) {
      modem.maintain();
  }

}

struct SensorData executeBLEScan() {
    Serial.println("============================================================================");
    Serial.printf("Starting BLE scan for %d seconds...\n", SCAN_TIME);

    BLEScan* pBLEScan = BLEDevice::getScan(); //create new scan
    pBLEScan->setActiveScan(true); //active scan uses more power, but get results faster
    BLEScanResults foundDevices = pBLEScan->start(SCAN_TIME);

    xiaomi_device_count = 0;

    int count = foundDevices.getCount();

    SensorData sensorData;

    float battery_voltage = -100;
    float battery_percent = 0;
    float humidity = -100;
    float temperature = -100;

    for (int i = 0; i < count - 1; i++) {
      BLEAdvertisedDevice advertisedDevice = foundDevices.getDevice(i);
      /*
       *  Is this a device we are looking for?
       */
      if (advertisedDevice.haveName() && advertisedDevice.haveServiceData() && (advertisedDevice.getName().find("BLACKA_001") != -1))
      {
          xiaomi_device_count++;
          int serviceDataCount = advertisedDevice.getServiceDataCount();
          std::string strServiceData = advertisedDevice.getServiceData(0);
  
          uint8_t cServiceData[100];
          char charServiceData[100];
  
          strServiceData.copy((char *)cServiceData, strServiceData.length(), 0);
  
          Serial.printf("Advertised Device: %s\n", advertisedDevice.toString().c_str());
  
          for (int i=0;i<strServiceData.length();i++) {
              sprintf(&charServiceData[i*2], "%02x", cServiceData[i]);
          }
  
          std::stringstream ss;
          ss << charServiceData;
  
          Serial.print("    BLE Service Data:");
          Serial.println(ss.str().c_str());
  
          unsigned long value;
  
          // extract the temperature
          value = (cServiceData[TEMP_MSB] << 8) + cServiceData[TEMP_LSB];
          if(METRIC)
          {
            temperature = (float)value/100;
          } else {
            temperature = CelciusToFahrenheit((float)value/100);
          }
  
          // extract the humidity
          value = (cServiceData[HUMID_MSB] << 8) + cServiceData[HUMID_LSB];
          humidity = (float)value/100;
  
          // extract the battery voltage
          value = (cServiceData[BATTV_MSB] << 8) + cServiceData[BATTV_LSB];
          battery_voltage = (float)value/1000;
  
          // extract the battery percentage
          value = cServiceData[BATTP_BYTE];
          battery_percent = (float)value;

          sensorData.temperature = temperature;
          sensorData.deviceName = advertisedDevice.getName().c_str();
  
          printf("    Temperature: %.2f  Humidity: %.2f%%  Battery: %.4fmV  ~%.0f%%\n", temperature, humidity, battery_voltage, battery_percent);
          return sensorData;
      }
    }

    printf("\nTotal found device count : %d\n", count);
    printf("Total Xiaomi device count: %d\n", xiaomi_device_count);

    return sensorData;
}


float CelciusToFahrenheit(float Celsius)
{
 float Fahrenheit=0;
 Fahrenheit = Celsius * 9/5 + 32;
 return Fahrenheit;
}
