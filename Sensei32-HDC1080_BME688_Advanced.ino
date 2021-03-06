/* 
  SENSEI32 - BME688 Basic Sensor read 
  
  ####################################################################################################################################
  This software, the ideas and concepts is Copyright (c) Davide Raggini & Davide Samori 2021. All rights to this software are reserved.

  Any redistribution or reproduction of any part or all of the contents in any form is prohibited other than the following:
  1. You may print or download to a local hard disk extracts for your personal and non-commercial use only.
  2. You may copy the content to individual third parties for their personal use, but only if you acknowledge the author Davide Raggini & Davide Samori as the source of the material.
  3. You may not, except with our express written permission, distribute or commercially exploit the content.
  4. You may not transmit it or store it in any other website or other form of electronic retrieval system for commercial purposes.

  The above copyright ('as annotated') notice and this permission notice shall be included in all copies or substantial portions of the Software and where the
  software use is visible to an end-user.

  THE SOFTWARE IS PROVIDED "AS IS" FOR PRIVATE USE ONLY, IT IS NOT FOR COMMERCIAL USE IN WHOLE OR PART OR CONCEPT. FOR PERSONAL USE IT IS SUPPLIED WITHOUT WARRANTY
  OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
  IN NO EVENT SHALL THE AUTHOR OR COPYRIGHT HOLDER BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
  FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
  Contact us:   davide.raggini@gmail.com
                davide.samori@gmail.com
*/

/*******************************************************************************************
 *
 * Dependencies
 *
 *******************************************************************************************/

#include <EEPROM.h>

#include <Wire.h>
#include <ClosedCube_HDC1080.h>
#include "bsec.h"

/*******************************************************************************************
 *
 * Private Definitions
 *
 *******************************************************************************************/
 
// Define to ignore the battery voltage - DEBUG ONLY
//#define FAKE_BATTERY 

// Define the amount delay between LED ON and OFF time [ms]
#define DELAY_ms  500

// Define the amount of sleep time after sensor acquisition [s]
#define SLEEP_TIME_s  300

/*******************************************************************************************
 *
 * Global Variables
 *
 *******************************************************************************************/

/*******************************************************************************************
 *
 * Private Variables
 *
 *******************************************************************************************/

// Sensei32 Board Led
static const uint8_t BOARD_LED        = 2; 

// Sensei32 Analog Input for Li-Ion Battery Voltage
static const uint8_t ANALOG_VBAT      = 35;

// Sensei32 Analog Input for VBUS and InCharge Status Read
static const uint8_t ANALOG_VBUS      = 36;
static const uint8_t ANALOG_INCHARGE  = 39;

// Sensei32 I2C Sensors Interface
static const uint8_t I2C_SDA          = 21; // Default I2C SDA line
static const uint8_t I2C_SCL          = 22; // Default I2C SCL line

// Sensei32 battery voltage and battery safety margin
float BATT_voltage;
float BATT_MinimumVoltage               = 2.90;

// Miscellanea and strings/variable to present time and date
bool  VBUS_Status = false, InCharge_Status = false;

// Humdity and Temperature Sensor - HDC1080
ClosedCube_HDC1080 hdc1080;

// Humdity, Temperature, Pressure and Air Quality Sensor - BME688
// BME688 add an AI core, not yet supported in BOSCH BSEC, over the BME680 functions:
// Existing BOSCH BSEC BME680 library are however compatible with BME688  
Bsec iaqSensor;
const uint8_t bsec_config_iaq[] = {
  #include "config/generic_33v_300s_4d/bsec_iaq.txt"
};
// Sensor state is stored to eeprom 4 times a day, as suggested by BOSH
// Between deep-sleeps, sensors state is retained in RTC memory
RTC_DATA_ATTR uint8_t sensor_state[BSEC_MAX_STATE_BLOB_SIZE] = {0};
RTC_DATA_ATTR uint8_t sensor_state_isValid = 0;
// Data retrieved from the BME688 sensor
bsec_virtual_sensor_t sensor_list[] = {
  BSEC_OUTPUT_RAW_TEMPERATURE,
  BSEC_OUTPUT_RAW_PRESSURE,
  BSEC_OUTPUT_RAW_HUMIDITY,
  BSEC_OUTPUT_RAW_GAS,
  BSEC_OUTPUT_IAQ,
  BSEC_OUTPUT_STATIC_IAQ,
  BSEC_OUTPUT_CO2_EQUIVALENT,
  BSEC_OUTPUT_BREATH_VOC_EQUIVALENT,
  BSEC_OUTPUT_SENSOR_HEAT_COMPENSATED_TEMPERATURE,
  BSEC_OUTPUT_SENSOR_HEAT_COMPENSATED_HUMIDITY,
};
String dbg_output;

// Miscellanea for deep-sleep managment
RTC_DATA_ATTR int bootCount                 = 0;
RTC_DATA_ATTR long OnTime_s                 = 0;

/*******************************************************************************************
 *
 * RTC Stored Variables (retained in deep-sleep)
 *
 *******************************************************************************************/

/*******************************************************************************************
 *
 * Arduino Setup
 *
 ******************************************************************************************/

/**
  * @brief	Arduino Environment Setup Callback
  *
  * @param  None
  * @retval None
  */
void setup() {
 
  // Slowdown the cpu
  setCpuFrequencyMhz(80); //Set CPU clock to 80MHz to low the power consumption
  getCpuFrequencyMhz();   //Get CPU clock
  
  // Setup stuff
  Serial.begin(115200);
  while(!Serial);

  // New Line, pretty serial output
  Serial.println("");
  
  // Init BOARD LED
  pinMode(BOARD_LED, OUTPUT);
  
  // Init HDC1080 Sensor, Default settings: 
  //  - Heater off
  //  - 14 bit Temperature and Humidity Measurement Resolutions
  hdc1080.begin(0x40);
  Serial.println("[" + String(millis()) + "] HDC1080: Init: Done");
  
  // BOSH BSEC closed-source library initialization
  
  // a) Init EEPROM for non-volatile sensor state recording
  EEPROM.begin(BSEC_MAX_STATE_BLOB_SIZE + 1); // 1st address for the length
  
  // b) Init I2C comms and BME688 sensor
  Wire.begin();
  iaqSensor.begin(0x76, Wire);
  checkIaqSensorStatus();
  iaqSensor.setConfig(bsec_config_iaq);
  checkIaqSensorStatus();

  // c) Manage Stored Sensor State
  if( bootCount != 0 ){
  
    if( sensor_state_isValid > 0 ){
      // Debug State Dump
      // DumpState("[" + String(millis()) + "] BME688: RTC_setState:", sensor_state);
      // Restore previous sensor status, from RTC memory
      iaqSensor.setState(sensor_state);
    }
    checkIaqSensorStatus();
    // Every 72 cycles (300s per cycle) == 4 times a day
    if( bootCount % 72 == 0 ){
      // Store last valid state to EEPROM
      for (uint8_t i = 0; i < BSEC_MAX_STATE_BLOB_SIZE ; i++) {
        EEPROM.write(i + 1, sensor_state[i]);
      }
      EEPROM.write(0, BSEC_MAX_STATE_BLOB_SIZE);
      EEPROM.commit();
      // Debug State Dump
      // DumpState("[" + String(millis()) + "] BME688: EEPROM_recordState:", sensor_state);
    }
  
  } else {
  
    // Status cannot be restored from RTC after the first boot
    // We must check if we have a non-volatile record in EEPROM
    if (EEPROM.read(0) == BSEC_MAX_STATE_BLOB_SIZE) {
      // Existing state in EEPROM
      Serial.println("[" + String(millis()) + "] BME688: Init: Reading state from EEPROM");
      for (uint8_t i = 0; i < BSEC_MAX_STATE_BLOB_SIZE; i++) {
        sensor_state[i] = EEPROM.read(i + 1);
      }
      // Debug State Dump
      // DumpState("[" + String(millis()) + "] BME688: EEPROM_setState:", sensor_state);
      // Restore previous sensor status, from EEPROM memory
      iaqSensor.setState(sensor_state);
      checkIaqSensorStatus();
    } else {
      // Erase the EEPROM with zeroes, no valid record is found
      Serial.println("[" + String(millis()) + "] BME688: Init: Erasing EEPROM");
      for (uint8_t i = 0; i < BSEC_MAX_STATE_BLOB_SIZE + 1; i++){
        EEPROM.write(i, 0);
      }
      EEPROM.commit();
      Serial.println("[" + String(millis()) + "] BME688: Init: EEPROM Erased");
    }
  
  }

  // d) Finalise BSEC library initialisation
  iaqSensor.updateSubscription(sensor_list, sizeof(sensor_list) / sizeof(sensor_list[0]), BSEC_SAMPLE_RATE_ULP);
  checkIaqSensorStatus();
  Serial.println("[" + String(millis()) + "] BME688: Init: Done");
  
}

/*******************************************************************************************
 *
 * Arduino Loop
 *
 ******************************************************************************************/

/**
  * @brief	Arduino Environment Loop Callback
  *
  * @param  None
  * @retval None
  */
void loop() {
  
  // Read battery voltage
  ReadBatteryOCV();
  // Read Sensei32 status
  ReadVBUS();
  ReadInCharge();
  
  // Execute only if there is enaugh voltage (this should preserve battery if not used for long time)
  // > Exception, if USB VBUS is detected, execute even if the battery is under UVLO.
  if( (BATT_voltage > BATT_MinimumVoltage) | (VBUS_Status == true) ){

    // Blink LED, ON state
    digitalWrite(BOARD_LED, HIGH);
    Serial.println("[" + String(millis()) + "] LED ON");

    // Read HDC1080
    Serial.println("[" + String(millis()) + "] HDC1080: Temperature: " + String(hdc1080.readTemperature()) + "*C");
    Serial.println("[" + String(millis()) + "] HDC1080: Humidity:    " + String(hdc1080.readHumidity()) + "%");
    
    // Read BME688 sample
    readIaqSensor();
    checkIaqSensorStatus();
    
    // Wait a little
    delay(DELAY_ms);
    
    // Blink LED, OFF state
    digitalWrite(BOARD_LED, LOW);
    Serial.println("[" + String(millis()) + "] LED OFF");
        
  } else {
    
    // Must go to deepsleep without a timeout to preserve battery
    // > When USB is plugged back in, execution will be resumed
    esp_sleep_enable_ext0_wakeup(GPIO_NUM_36,1); //1 = High, Hardware pull-down on VBUS
    esp_deep_sleep_start();
     
  }

  // Advance the OnTime for the BSEC Library
  OnTime_s = OnTime_s + (SLEEP_TIME_s * 1000) + millis();
  
  // Start deep-sleep
  Serial.println("[Sensei32 - Core] Starting " + String(SLEEP_TIME_s) + "s of Deep-Sleep");
  esp_sleep_enable_timer_wakeup( SLEEP_TIME_s  * 1000000LL ); 
  esp_deep_sleep_start();
  
}

/*******************************************************************************************
 *
 * Private Functions
 *
 *******************************************************************************************/

/**
  * @brief  BSEC Helper function: Check BME688 sensor status
  *
  * @param  None
  * @retval None
  */
void checkIaqSensorStatus()
{
  if (iaqSensor.status != BSEC_OK) {
    if (iaqSensor.status < BSEC_OK) {
      dbg_output = "[" + String(millis()) + "] BME688: BSEC error code : " + String(iaqSensor.status);
      Serial.println(dbg_output);
    } else {
      dbg_output = "[" + String(millis()) + "] BME688: BSEC warning code : " + String(iaqSensor.status);
      Serial.println(dbg_output);
    }
  }

  if (iaqSensor.bme680Status != BME680_OK) {
    if (iaqSensor.bme680Status < BME680_OK) {
      dbg_output = "[" + String(millis()) + "] BME688: Error code : " + String(iaqSensor.bme680Status);
      Serial.println(dbg_output);
    } else {
      dbg_output = "[" + String(millis()) + "] BME688: Warning code : " + String(iaqSensor.bme680Status);
      Serial.println(dbg_output);
    }
  }
}
/**
  * @brief  BSEC Helper function: Dump to terminal BME688 Sensor State
  *
  * @param  const char* name
  *         const uint8_t* state
  * @retval None
  */
void DumpState(const char* name, const uint8_t* state) {
  Serial.println( String(name) );
  for (int i = 0; i < BSEC_MAX_STATE_BLOB_SIZE; i++) {
    Serial.printf("%02x ", state[i]);
    if (i % 16 == 15) {
      Serial.print("\n");
    }
  }
  Serial.print("\n");
}
/**
  * @brief  BSEC Helper function: Read BME688 sensor values
  *
  * @param  None
  * @retval None
  */
void readIaqSensor()
{

  long time_trigger_ms = OnTime_s * 1000;
  if (iaqSensor.run( time_trigger_ms )) { // If new data is available, time must be given is ms (epoch is in s)
    Serial.println("[" + String(millis()) + "] BME688: OnTime = " + String(OnTime_s) + " ms");
    Serial.println("[" + String(millis()) + "] BME688: Temperature    = " + String(iaqSensor.temperature) + " *C");
    Serial.println("[" + String(millis()) + "] BME688: Humidity       = " + String(iaqSensor.humidity) + " %");
    Serial.println("[" + String(millis()) + "] BME688: Pressure       = " + String(iaqSensor.pressure / 100.0) + " hPa");
    Serial.println("[" + String(millis()) + "] BME688: GasResistance  = " + String(iaqSensor.gasResistance / 1000.0) + " Ohms");
    Serial.println("[" + String(millis()) + "] BME688: eCO2           = " + String(iaqSensor.co2Equivalent) + " ppm");
    Serial.println("[" + String(millis()) + "] BME688: bVOC           = " + String(iaqSensor.breathVocEquivalent) + " ppb");
    Serial.println("[" + String(millis()) + "] BME688: IAQ            = " + String(iaqSensor.iaq));
    Serial.println("[" + String(millis()) + "] BME688: IAQ-Accuracy   = " + String(iaqSensor.iaqAccuracy));

    // Check if the nextCall is coherent, must be exactly 5 minutes.
    // BSEC suffers from internal overflow when managing time, but no error is thrown from the stack
    // When this happens IAQ, bVOC and CO2 measurments freeze and nextCall return random values.
    // If this malfunction is detected, a sersor reset and full state erase is triggered.
    long bme688_nextCall = iaqSensor.nextCall - time_trigger_ms;
    if( (bme688_nextCall == 300000L) ){
      // Store current sensor status into RTC memory
      iaqSensor.getState(sensor_state);
      // Debug State Dump
      // DumpState("[" + String(millis()) + "] BME688: RTC_getState", sensor_state);
      // Signal that the sensor state has been updated
      sensor_state_isValid = 1;
    } else {
      // a) Erase the EEPROM, a sensor reset is need due to an internal BSEC error
      Serial.println("[" + String(millis()) + "] BME688: Init: Erasing EEPROM");
      for (uint8_t i = 0; i < BSEC_MAX_STATE_BLOB_SIZE + 1; i++){
        EEPROM.write(i, 0);
      }
      EEPROM.commit();
      Serial.println("[" + String(millis()) + "] BME688: Init: EEPROM Erased");
      // b) Erase the BSEC RTC-Retained status
      Serial.println("[" + String(millis()) + "] BME688: Init: Erasing RTC-RAM");
      for (uint8_t i = 0; i < BSEC_MAX_STATE_BLOB_SIZE; i++) {
        sensor_state[i] = EEPROM.read(i + 1);
      }
      Serial.println("[" + String(millis()) + "] BME688: Init: RTC-RAM Erased");
      // c) Signal that the sensor state has been invalidated
      sensor_state_isValid = 0;
      // d) Reset the Sensor OnTime variable
      OnTime_s = 0;
    }
    checkIaqSensorStatus();
    
  } else {
    checkIaqSensorStatus();
  }
  
}


/**
  * @brief	Function to get the Li-Ion battery voltage
  *
  * @param  None
  * @retval None
  */
void ReadBatteryOCV() {
  #ifdef FAKE_BATTERY
    BATT_voltage = 3.6f;
  #else
    // Battery should be read as first thing (so it will be similar to OCV value...)
    BATT_voltage = analogRead(ANALOG_VBAT) / 4096.0 * 6.77395927613 * 0.8843421307;  //(1/0.48715970461) * 3.3 - R1= 470K, R2=(680K//1.3M_leak)
                                                                                     //fine_tuning offset was added: 0.8843421307
  #endif
  Serial.println("[Sensei32 - Core] BATT Voltage = " + String(BATT_voltage));
}

/**
  * @brief	Function to get USB BUS Voltage
  *
  * @param  None
  * @retval None
  */
void ReadVBUS() {
  float VBUS_voltage = analogRead(ANALOG_VBUS) / 4096.0 * 3.3 * 2.076923077;  // R1 = 100K, R2 = (100K//1.3M_leak)
  Serial.println("[Sensei32 - Core] VBUS Voltage = " + String(VBUS_voltage));
  // On Sensei32 VBUS is read with a 1/2 divider from USB VBUS
  // Digital status is computed by evaluating the pin voltage
  if( VBUS_voltage >= 3.3 ){
    VBUS_Status = true;
  } else {
    VBUS_Status = false;
  }
  Serial.println("[Sensei32 - Core] VBUS State Digital State = " + String(VBUS_Status) );
}

/**
  * @brief	Function to get the charge status of the Li-Ion Battery
  *
  * @param  None
  * @retval None
  */
void ReadInCharge() {
  float InCharge_voltage = analogRead(ANALOG_INCHARGE) / 4096.0 * 3.3 * 2.098461538;  // R1 = 102K, R2 = (100K//1.3M_leak)
  Serial.println("[Sensei32 - Core] InCharge Voltage = " + String(InCharge_voltage));
  // On Sensei32 InCharge_Status is read with a 1/2 divider from LiIon Charge Pin
  // Digital status is computed by evaluating the pin voltage, with inverted logic
  if( InCharge_voltage >= 3.3 ){
    InCharge_Status = false;
  } else {
    InCharge_Status = true;
  }
  Serial.println("[Sensei32 - Core] InCharge Digital State = " + String(InCharge_Status) );
}
