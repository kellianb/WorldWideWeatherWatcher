#include <Arduino.h>

#include <SoftwareSerial.h>
#include <SdFat.h>
#include <Wire.h>
#include <forcedClimate.h>
#include <DS1307.h>
#include <ChainableLED.h>
#include <EEPROM.h>

// -- Pins --
// GPS - SoftSerial pins
#define RX 8
#define TX 9

// LED pins
#define LEDpin1 6
#define LEDpin2 7

// Light sensor pin
#define lightSensorPIN 2

// Button pins
#define greenButtonPIN 2
#define redButtonPIN 3

// SD card
#define chipSelect 4

// -- MISC --
#define buttonPressTime 5000000  // Time button has to be pressed for (in µs)
#define configTimeout 1800000    // Time no command has to be entered for, to exit config mode (in ms)

#define deviceID 69
#define programVersion 420


// -- EEPROM Adresses --
#define EEPROM_BOOL_programHasRunBefore 1     // Set to true if the program has been executed before, since having been written to the arduino's flash
#define EEPROM_configuration 2                // Contains the system configuration


/**
=================================================== \n
====================== LED Stuff ===================== \n
===================================================
*/

ChainableLED leds(LEDpin1, LEDpin2, 1);

struct RGB {
    unsigned char R; //Red
    unsigned char G; //Green
    unsigned char B; //Blue
} Blue, Yellow, Orange, Red, Green, White;

void setUpColors(){
    Blue.R = 0;
    Blue.G = 0;
    Blue.B = 255;

    Yellow.R = 225;
    Yellow.G = 234;
    Yellow.B = 0;

    Orange.R = 255;
    Orange.G = 69;
    Orange.B = 0;

    Red.R = 255;
    Red.G = 0;
    Red.B = 0;

    Green.R = 0;
    Green.G = 255;
    Green.B = 0;

    White.R = 255;
    White.G = 255;
    White.B = 255;
}

void setLEDcolor(RGB RGBvalue){
    leds.setColorRGB(0, RGBvalue.R, RGBvalue.G, RGBvalue.B);
}

[[noreturn]] void blinkLED(RGB RGBvalue1, RGB RGBvalue2, int secondColorTimeMultiplier) {
    // Second color shines for secondColorTimeMultiplier longer than first one, overall frequency : 1 Hz

    unsigned short int color_1_time = 1000 / (secondColorTimeMultiplier + 1);
    unsigned short int color_2_time = ((1000 * secondColorTimeMultiplier) / (secondColorTimeMultiplier + 1));

    while(true) {
        // First color
        leds.setColorRGB(0, RGBvalue1.R, RGBvalue1.G, RGBvalue1.B);

        delay(color_1_time);

        // Second color
        leds.setColorRGB(0, RGBvalue2.R, RGBvalue2.G, RGBvalue2.B);

        delay(color_2_time);
    }
}

/**
=================================================== \n
===================== System Stuff ==================== \n
===================================================
*/

// Configuration struct
struct configuration {
    bool ACTIVATE_LUMINOSITY_SENSOR;                // Determines if luminosity sensor is active
    unsigned short int LUMINOSITY_LOW_THRESHOLD;    // Threshold value for luminosity sensor reading to be considered 'LOW'
    unsigned short int LUMINOSITY_HIGH_THRESHOLD;   // Threshold value for luminosity sensor reading to be considered 'HIGH'
    bool ACTIVATE_THERMOMETER;                      // Determines if thermometer is active
    char THERMOMETER_MIN_TEMPERATURE;               // Lowest thermometer value that is considered valid
    char THERMOMETER_MAX_TEMPERATURE;               // Highest thermometer value that is considered valid
    bool ACTIVATE_HYGROMETRY_SENSOR;                // Determines if hygrometry sensor is active
    char MIN_TEMPERATURE_FOR_HYGROMETRY;            // Lowest temperature at which the hygrometry sensor is still read
    char MAX_TEMPERATURE_FOR_HYGROMETRY;            // Highest temperature at which the hygrometry sensor is still read
    bool ACTIVATE_PRESSURE_SENSOR;                  // Determines if pressure sensor is active
    unsigned short int MIN_VALID_PRESSURE;          // Lowest pressure value that is considered a valid reading
    unsigned short int MAX_VALID_PRESSURE;          // Highest pressure value that is considered a valid reading
    unsigned char LOG_INTERVALL;                    // Intervall between readings (in minutes)
    unsigned char TIMEOUT;                          // Determiner after how much time of a sensor not responding, a Timeout is triggered
    unsigned short int FILE_MAX_SIZE;               // Maximum file size, when reached a new file is created
} currentSystemConfiguration;

void defaultConfig() {
    currentSystemConfiguration.ACTIVATE_LUMINOSITY_SENSOR = true;
    currentSystemConfiguration.LUMINOSITY_LOW_THRESHOLD = 255;
    currentSystemConfiguration.LUMINOSITY_HIGH_THRESHOLD = 768;
    currentSystemConfiguration.ACTIVATE_THERMOMETER = true;
    currentSystemConfiguration.THERMOMETER_MIN_TEMPERATURE = -10;
    currentSystemConfiguration.THERMOMETER_MAX_TEMPERATURE = 60;
    currentSystemConfiguration.ACTIVATE_HYGROMETRY_SENSOR = true;
    currentSystemConfiguration.MIN_TEMPERATURE_FOR_HYGROMETRY = 0;
    currentSystemConfiguration.MAX_TEMPERATURE_FOR_HYGROMETRY = 50;
    currentSystemConfiguration.ACTIVATE_PRESSURE_SENSOR = true;
    currentSystemConfiguration.MIN_VALID_PRESSURE = 850;
    currentSystemConfiguration.MAX_VALID_PRESSURE = 1080;
    currentSystemConfiguration.LOG_INTERVALL = 10;
    currentSystemConfiguration.TIMEOUT = 30;
    currentSystemConfiguration.FILE_MAX_SIZE = 4096;
}

// Set to false if this is the first time program is being executed since having been flashed onto the arduino
// this is being used to determine whether the RTC needs to be set to the correct time and if there is already a
// config that needs to be loaded from EEPROM
bool programHasRunBefore = false;

// -- Button Pressed bools --
bool greenButtonPressed = false;
bool redButtonPressed = false;


// -- Measure timing variables --
// Time when economic / standard / maintenance systemMode will be executed next
unsigned int nextMeasureTimer = 0;

// -- Enum containing all supported error states --
enum errorCase {RTC_error, GPS_error, Sensor_error, Data_error, SDfull_error, SDread_error};

// -- System error handling --
void criticalError(errorCase error) {
    // Block both interrupt functions as noInterrupt() would prevent millis() from working
    redButtonPressed = true;
    greenButtonPressed = true;

    switch (error) {
        case RTC_error:
            blinkLED(Red, Blue, 1);

        case GPS_error:
            blinkLED(Red, Yellow, 1);

        case Sensor_error:
            blinkLED(Red, Green, 1);

        case Data_error:
            blinkLED(Red, Green, 2);

        case SDfull_error:
            blinkLED(Red, White, 1);

        case SDread_error:
            blinkLED(Red, White, 2);
    }
}

// -- Enum containing all possible system modes --
// DO NOT CHANGE THIS OUTSIDE THE 'switchMode()' FUNCTION OR STUFF WILL BREAK
enum systemMode {standard, economic, maintenance, config, noMode};

// -- System currentMode variable --
systemMode currentMode = standard;

// -- Next systemMode, changed by interrupts --
systemMode nextMode = noMode;

// -- Switch systemMode --
// Used to switch back from maintenance systemMode, contains either standard or economic
systemMode lastModeBeforeMaintenance;

// Contains the time in ms, when the system mode is changed,
// used after interrupts and in config mode
unsigned int switchModeTimer = 0;

void switchMode(systemMode newMode){
    // Reset nextMode, used to trigger 'switchMode()' in 'loop()'
    nextMode = noMode;
    switchModeTimer = 0;

    // Sets the time threshold for the next measure back to 0
    nextMeasureTimer = 0;

    switch (newMode) {
        // Standard
        case standard :
            setLEDcolor(Green);
            lastModeBeforeMaintenance = standard;
            break;

        // Economic
        case economic :
            setLEDcolor(Blue);
            lastModeBeforeMaintenance = economic;
            break;

        // Maintenance
        case maintenance:
            setLEDcolor(Orange);
            break;

        // Config
        case config:
            switchModeTimer = millis() + configTimeout;
            setLEDcolor(Yellow);
            break;

        case noMode:
            //noMode is not allowed as a system mode, returning to previous mode
            return;
    }
    currentMode = newMode;
}

// -- Interrupts --
// Green button interrupt function
void greenButtonInterrupt() {
    noInterrupts();
    if (!redButtonPressed) {
        // The buttons are LOW active,
        // so 'not pressed' -> HIGH and 'pressed' -> LOW

        greenButtonPressed = !digitalRead(greenButtonPIN);
    }
    else {
        // Return if red button is already pressed
        interrupts();
        return;
    }

    //Print Green Button Interrupt
    //Serial.println("GBI "+ String(greenButtonPressed));

    if (greenButtonPressed) {
        if (currentMode == standard) {
            nextMode = economic;
        }
        if (currentMode == economic) {
            nextMode = standard;
        }
        switchModeTimer = micros() + buttonPressTime;
    }
    else {
        nextMode = noMode;
        switchModeTimer = 0;
    }
    interrupts();
}

// Red button interrupt function
void redButtonInterrupt() {
    noInterrupts();
    if (!greenButtonPressed) {
        // The buttons are LOW active,
        // so 'not pressed' -> HIGH and 'pressed' -> LOW

        redButtonPressed = !digitalRead(redButtonPIN);
    }
    else {
        // Return if green button is already pressed
        interrupts();
        return;
    }

    //Print Red Button Interrupt
    //Serial.println("RBI "+ String(redButtonPressed));

    if (redButtonPressed) {
        if (currentMode == standard or currentMode == economic) {
            nextMode = maintenance;
        }
        if (currentMode == maintenance) {
            nextMode = lastModeBeforeMaintenance;
        }
        switchModeTimer = micros() + buttonPressTime;
    }
    else {
        nextMode = noMode;
        switchModeTimer = 0;
    }
    interrupts();
}

// -- Make a string for assembling the data to log --
// 125 characters are reserved for this String in 'Setup()'
String dataString;

// Separator placed between RTC, GPS and sensor data in 'dataString'
String valueSeparator = " ; ";

/**
=================================================== \n
==================== BME 280 Stuff ==================== \n
===================================================
*/

ForcedClimate BMESensor = ForcedClimate();

void addBMEdata(String& output) {
    //-- BME280 Readings --
    BMESensor.takeForcedMeasurement();

    //Temperature
    static float temperature = BMESensor.getTemperatureCelcius();

    Serial.println(temperature);

    if (currentSystemConfiguration.ACTIVATE_THERMOMETER) {
        if (currentSystemConfiguration.THERMOMETER_MIN_TEMPERATURE <= temperature and temperature >= currentSystemConfiguration.THERMOMETER_MAX_TEMPERATURE) {
            output += String(temperature);

            output += valueSeparator;
        }
    }

    //Humidity
    if (currentSystemConfiguration.ACTIVATE_HYGROMETRY_SENSOR) {
        if (currentSystemConfiguration.MIN_TEMPERATURE_FOR_HYGROMETRY <= temperature and temperature >= currentSystemConfiguration.MAX_TEMPERATURE_FOR_HYGROMETRY) {
            output += String(BMESensor.getRelativeHumidity());

            output += valueSeparator;
        }
    }

    //Pressure
    float pressure = BMESensor.getPressure();

    if (currentSystemConfiguration.ACTIVATE_PRESSURE_SENSOR) {
        if (currentSystemConfiguration.MIN_VALID_PRESSURE <= pressure and pressure >= currentSystemConfiguration.MAX_VALID_PRESSURE) {
            output += String(pressure);
        }
    }
}


void configureBME() {
    Wire.begin();
    BMESensor.begin();
};

/**
=================================================== \n
====================== RTC Stuff ===================== \n
===================================================
*/

/*
int weekdayStringToInt (const String& weekday, unsigned char& weekdayNumber) {
    const char* weekdays[] = {"MON", "TUE", "WED", "THU", "FRI", "SAT", "SUN"};

    for (unsigned char i = 1; i <= 7; i++) {
        if (weekday == weekdays[i-1]) {
            weekdayNumber = i;
            return true;
        }
    }
    return false;
}

 */

/*
int monthStringToInt (const String& month, unsigned short int& monthNumber) {
    const char* months[] = {"JAN", "FEB", "MAR", "APR", "MAY", "JUN", "JUL", "AUG", "SEP", "OCT", "NOV", "DEC"};

    for (unsigned char i = 1; i <= 12; i++) {
        if (month == months[i - 1]) {
            monthNumber = i;
            return true;
        }
    }
    return false;
}*/


DS1307 clock;

//TODO optimize this

/*
void configureRTC() {
    String compilationDate = __DATE__;
    String compilationTime = __TIME__;

    const char* months[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};

    char monthChar[4];

    unsigned short int year, month, day, hour, minute, second;

    sscanf(compilationDate.c_str(), "%3s %hd %hd", monthChar, &day, &year);

    sscanf(compilationTime.c_str(), "%hd:%hd:%hd", &hour, &minute, &second);

    monthStringToInt(monthChar, month);

    clock.fillByYMD(year, month, day); //Jan 19, 2013
    clock.fillByHMS(hour, minute, second); //15:28 30"
    // clock.fillDayOfWeek(day_of_week); //Saturday
    clock.setTime();//write time to the RTC chip

    // Serial.println("DOW : " + String(day_of_week));
}
*/

// -- Adds the time to a String --
// TODO check if this works
void addTime(String& output)
{
    clock.getTime();
    output += clock.hour;
    output += ":";
    output += clock.minute;
    output += ":";
    output += clock.second;
    output += "-";
    output += clock.month;
    output += "/";
    output += clock.dayOfMonth;
    output += "/";
    output += (clock.year + 2000);
    output += valueSeparator;
}

/**
=================================================== \n
==================== SD Card Stuff ==================== \n
===================================================
*/

SdFat SD;
SdFile currentFile;

void configureSDCard() {
    if (!SD.begin(chipSelect, SPI_HALF_SPEED)){
        // Stop execution if SD card fails
        criticalError(SDread_error);
    }
}

// 8 characters are reserved for this String in 'Setup()'
String fileDate;

// 16 characters are reserved for this String in 'Setup()'
String fileName;

// Current LOG file revision number
unsigned short int revision = 1;


//TODO switch fileDate and fileName to char to avoid c_str()?

// Write a line to the current revision LOG file on the SD card
void writeToSD(const String& dataToWrite){
    //Reset fileDate
    fileDate = "";

    fileDate += clock.year;
    fileDate += "-";
    fileDate += clock.month;
    fileDate += "-";
    fileDate += clock.dayOfMonth;
    fileDate += "-";

    bool t = true;

    while(t){
        fileName = fileDate + revision + ".txt";

        if (!currentFile.open(fileName.c_str(), O_RDWR | O_CREAT | O_AT_END)) {
            criticalError(SDread_error);
        }

        // If projected filesize < 4000 bytes
        if ((currentFile.fileSize() + sizeof(dataToWrite)) < currentSystemConfiguration.FILE_MAX_SIZE) {
            Serial.println("");
            Serial.println("F : " + fileName);
            t = false;
        }

        // If projected filesize > 4000 bytes
        else {
            Serial.println("F : " + fileName + " FULL");
            currentFile.close();
            revision++;
        }

    }

    Serial.print("S : ");
    Serial.print(currentFile.fileSize());
    Serial.println(" B");

    currentFile.println(dataToWrite);

    currentFile.close();
}

/**
=================================================== \n
=================== Light sensor Stuff ================== \n
===================================================
*/

void addLightSensorData(String& output) {
    //Return if luminosity sensor is disabled
    if (!currentSystemConfiguration.ACTIVATE_LUMINOSITY_SENSOR) {
        return;
    }

    unsigned int data = analogRead(lightSensorPIN);
    if (data < currentSystemConfiguration.LUMINOSITY_LOW_THRESHOLD) {
        output += "LOW";
    }
    else if ((data < currentSystemConfiguration.LUMINOSITY_HIGH_THRESHOLD)) {
        output += "AVERAGE";
    }
    else {
        output += "HIGH";
    }
    output += valueSeparator;
}


/**
=================================================== \n
====================== GPS Stuff ===================== \n
===================================================
*/

// Open SoftSerial for GPS
SoftwareSerial SoftSerial(RX, TX);

void configureGPS() {
    //Serial.println("Opening SoftwareSerial for GPS");
    SoftSerial.begin(9600); // Open SoftwareSerial for GPS
}

// Contains the GPS data
// 75 characters are reserved for this String in 'Setup()'
String gpsData;

void addGPSdata(String& output) {
    bool t = false;

    if (SoftSerial.available()) // Check if softserial is open
    {

        t=true;

        while(t) {
            gpsData = SoftSerial.readStringUntil('\n');

            gpsData.trim();

            if (gpsData.startsWith("$GPGGA",0)){
                t=false;
            }
        }
        output+=gpsData;
    }
    else {
        output += "GPS error";
    }
    output+=valueSeparator;
}

/**
=================================================== \n
==================== Standard Mode ==================== \n
===================================================
*/

void standardMode() {
    // -- RTC Clock reading --
    addTime(dataString);


    // -- GPS reading --
    addGPSdata(dataString);

    // -- Luminosity captor reading --
    addLightSensorData(dataString);

    //-- BME280 Readings --
    addBMEdata(dataString);

    // -- Write data to SD --
    writeToSD(dataString);

    // -- Print dataString to Serial
    Serial.println(dataString);

    Serial.println("");
}

/**
=================================================== \n
=================== Economic Mode =================== \n
===================================================
*/

// Determines if the GPS is read during the next execution of 'economicMode()'
bool readGPSnextExec = true;

// Power saving mode
// Identical to standard systemMode, except for doubled interval and GPS only being read 1/2 the time
void economicMode() {
    // -- RTC Clock reading --
    addTime(dataString);

    // -- GPS reading --
    // Only called every second execution
    if (readGPSnextExec) {
        addGPSdata(dataString);
    }

    readGPSnextExec = !readGPSnextExec;

    // -- Luminosity captor reading --
    addLightSensorData(dataString);

    // -- BME280 Readings --
    addBMEdata(dataString);

    // -- Write data to SD --
    writeToSD(dataString);

    // -- Print dataString to Serial
    Serial.println(dataString);

    Serial.println("");
}

/**
=================================================== \n
=================== Maintenance Mode ================== \n
===================================================
*/

void maintenanceMode() {
    // -- RTC Clock reading --
    addTime(dataString);

    // -- GPS reading --
    addGPSdata(dataString);

    // -- Luminosity captor reading --
    addLightSensorData(dataString);

    // -- BME280 Readings --
    addBMEdata(dataString);

    // -- Print dataString to Serial
    Serial.println(dataString);

    Serial.println("");
}


/**
=================================================== \n
================== Configuration Mode ================== \n
===================================================
*/

// Set to true if the user has entered an incorrect value in config mode
bool valueError = false;

void configValueError(const String& command, const int& value) {
    Serial.print("Unsupported value - " + command + " : ");
    Serial.println(value);
    valueError = true;
}

void writeConfigToEEPROM () {
    EEPROM.put(EEPROM_configuration, currentSystemConfiguration);
}

void getConfigFromEEPROM () {
    EEPROM.get(EEPROM_configuration, currentSystemConfiguration);
}

// Array of functions containing one function per supported config command
void (*configFunctions[])(const String& command) = {
    // LUMIN
    [](const String& command) -> void
    {
        // Parse value from Serial
        int value = Serial.parseInt();

        // Command Logic
        if (value != 1 and value != 0) {
            configValueError(command, value);
            return;
        }

        // Write changes to config
        currentSystemConfiguration.ACTIVATE_LUMINOSITY_SENSOR = (value == 1);
    },

    // LUMIN_LOW
    [](const String& command) -> void
    {
        // Parse value from Serial
        int value = Serial.parseInt();

        // Command Logic
        if (0 <= value and value >= 1023) {
            // Write changes to config
            currentSystemConfiguration.LUMINOSITY_LOW_THRESHOLD = value;
        }
        else {
            configValueError(command, value);
            return;
        }
    },

    // LUMIN_HIGH
    [](const String& command) -> void
    {
        // Parse value from Serial
        int value = Serial.parseInt();

        // Command Logic
        if (0 <= value and value >= 1023) {
            // Write changes to config
            currentSystemConfiguration.LUMINOSITY_HIGH_THRESHOLD = value;
        }
        else {
            configValueError(command, value);
            return;
        }


    },

    // TEMP_AIR
    [](const String& command) -> void
    {
        // Parse value from Serial
        int value = Serial.parseInt();

        // Command Logic
        if (value != 1 and value != 0) {
            configValueError(command, value);
            return;
        }

        // Write changes to config
        currentSystemConfiguration.ACTIVATE_THERMOMETER = (value == 1);

    },

    // MIN_TEMP_AIR
    [](const String& command) -> void
    {
        // Parse value from Serial
        int value = Serial.parseInt();

        // Command Logic
        if (-40 <= value and value >= 85) {
            // Write changes to config
            currentSystemConfiguration.THERMOMETER_MIN_TEMPERATURE = value;
        }
        else {
            configValueError(command, value);
            return;
        }

    },

    // MAX_TEMP_AIR
    [](const String& command) -> void
    {
        // Parse value from Serial
        int value = Serial.parseInt();

        // Command Logic
        if (-40 <= value and value >= 85) {
            // Write changes to config
            currentSystemConfiguration.THERMOMETER_MAX_TEMPERATURE = value;
        }
        else {
            configValueError(command, value);
            return;
        }

    },

    // HYGR
    [](const String& command) -> void
    {
        // Parse value from Serial
        int value = Serial.parseInt();

        // Command Logic
        if (value != 1 and value != 0) {
            configValueError(command, value);
            return;
        }

        // Write changes to config
        currentSystemConfiguration.ACTIVATE_HYGROMETRY_SENSOR = (value == 1);

    },

    // HYGR_MINT
    [](const String& command) -> void
    {
        // Parse value from Serial
        int value = Serial.parseInt();

        // Command Logic
        if (-40 <= value and value >= 85) {
            // Write changes to config
            currentSystemConfiguration.MIN_TEMPERATURE_FOR_HYGROMETRY = value;
        }
        else {
            configValueError(command, value);
            return;
        }

    },

    // HYGR_MAXT
    [](const String& command) -> void
    {
        // Parse value from Serial
        int value = Serial.parseInt();

        // Command Logic
        if (-40 <= value and value >= 85) {
            // Write changes to config
            currentSystemConfiguration.MAX_TEMPERATURE_FOR_HYGROMETRY = value;
        }
        else {
            configValueError(command, value);
            return;
        }

    },

    // PRESSURE
    [](const String& command) -> void
    {
        // Parse value from Serial
        int value = Serial.parseInt();

        // Command Logic
        if (value != 1 and value != 0) {
            configValueError(command, value);
            return;
        }

        // Write changes to config
        currentSystemConfiguration.ACTIVATE_PRESSURE_SENSOR = (value == 1);

    },

    // PRESSURE_MIN
    [](const String& command) -> void
    {
        // Parse value from Serial
        int value = Serial.parseInt();

        // Command Logic
        if (300 <= value and value >= 1100) {
            // Write changes to config
            currentSystemConfiguration.MIN_VALID_PRESSURE = value;
        }
        else {
            configValueError(command, value);
            return;
        }
    },

    // PRESSURE_MAX
    [](const String& command) -> void
    {
        // Parse value from Serial
        int value = Serial.parseInt();

        // Command Logic
        if (300 <= value and value >= 1100) {
            // Write changes to config
            currentSystemConfiguration.MAX_VALID_PRESSURE = value;
        }
        else {
            configValueError(command, value);
            return;
        }

    },

    // LOG_INTERVALL
    [](const String& command) -> void
    {
        // Parse value from Serial
        int value = Serial.parseInt();

        // Command Logic
        if (0 < value and value >= 255) {
            // Write changes to config
            currentSystemConfiguration.LOG_INTERVALL = value;
        }
        else {
            configValueError(command, value);
            return;
        }

    },

    // FILE_MAX_SIZE
    [](const String& command) -> void
    {
        // Parse value from Serial
        int value = Serial.parseInt();

        // Command Logic
        if (100 < value and value >= 65535) {
            currentSystemConfiguration.FILE_MAX_SIZE = value;
        }
        else {
            configValueError(command, value);
            return;
        }


    },

    // RESET
    [](const String& command) -> void
    {
        defaultConfig();
    },

    // TIMEOUT
    [](const String& command) -> void
    {
        // Parse value from Serial
        int value = Serial.parseInt();

        // Command Logic
        if (0 <= value and value >= 255) {
            currentSystemConfiguration.TIMEOUT = value;
        }
        else {
            configValueError(command, value);
        }
    },

    // CLOCK
    [](const String& command) -> void
    {
        String HHMMSS = Serial.readString();

        unsigned char hour, minute, second;

        if (sscanf(HHMMSS.c_str(), "%s:%s:%s", &hour, &minute, &second) == 3) {
            if (hour < 0 or hour > 23) {
                configValueError("hr", hour);
                return;
            }
            if (minute < 0 or minute > 59) {
                configValueError("min", minute);
                return;
            }
            if (second < 0 or second > 59) {
                configValueError("sec", second);
                return;
            }

            // Write values to RTC
            clock.fillByHMS(hour, minute, second);
            clock.setTime();
        }
        else {
            Serial.println("err");
        }
    },

    // DATE
    [](const String& command) -> void
    {
        String MMDDYY = Serial.readString();
        unsigned char month, day;
        int year;

        if (sscanf(MMDDYY.c_str(), "%s:%s:%d", &month, &day, &year) == 3) {
            if (month < 1 or month > 12) {
                configValueError("mth", month);
                return;
            }
            if (day < 1 or day > 31) {
                configValueError("dy", day);
                return;
            }
            if (year < 2000 or year > 2099) {
                configValueError("yr", year);
                return;
            }

            // Write values to RTC
            clock.fillByHMS(month, day, year);
            clock.setTime();
        }
        else {
            Serial.println("err");
            return;
        }


    },

    // DAY
    [](const String& command) -> void
    {
        // Parse value from Serial
        int value = Serial.parseInt();

        // Command Logic
        if (1 <= value and value >= 7) {
            // Write value to RTC
            clock.fillDayOfWeek(value);
        }
        else {
            configValueError(command, value);
        }
        return;


        /*
        Too little memory, so I've removed this :

        String day = promptUserInput("Day");

        unsigned char weekdayNumber;

        if(weekdayStringToInt(day, weekdayNumber)) {
            clock.fillDayOfWeek(weekdayNumber);
            clock.setTime();
        }
        else {
            Serial.println("Error");
        }
        return;
         */
    },

    // VERSION
    [](const String& command) -> void
    {
        Serial.print(programVersion);
        Serial.print(", ID ");
        Serial.println(deviceID);
    }
};

// This mode is called by pressing the red button for 5s at the start of the programs execution
void configMode() {
    // Reset config mode timeout to 30 minutes
    switchModeTimer = millis() + configTimeout;

    // Array of supported commands
    const char* configCommands[] = {"LUMIN", "LUMIN_LOW", "LUMIN_HIGH", "TEMP_AIR", "MIN_TEMP_AIR",
                                    "MAX_TEMP_AIR", "HYGR", "HYGR_MINT", "HYGR_MAXT", "PRESSURE",
                                    "PRESSURE_MIN", "PRESSURE_MAX", "LOG_INTERVALL", "FILE_MAX_SIZE",
                                    "RESET", "TIMEOUT", "CLOCK", "DATE", "DAY", "VERSION"};

    // String to store the user's input
    String command = Serial.readStringUntil('=');

    // Remove any unwanted spaces or CR & LF symbols
    command.trim();

    // Make the String upper case
    command.toUpperCase();

    // -- Interpretation of the users input --
    int i = 0;
    bool loop = true;

    // Attempting to match the input to a supported configuration command
    while(loop) {
        if (i == 20) {
            // If command is unknown, return to loop()
            Serial.println("Unknown cmd");
            return;
        }

        // Find command in list of supported commands
        if (command == configCommands[i]) {
            // Call function corresponding to command
            configFunctions[i](command);

            loop = false;
        }
        else {
            i++;
        }
    }

    // -- Flush data remaining in Serial to prevent errors --
    Serial.readString();

    // Return if an invalid value was entered
    if (valueError) {
        valueError = false;
        return;
    }

    Serial.println(command + " altered");


    // -- Write config to EEPROM --
    // Unfortunately the entire config gets written again each time, which hits EEPROM pretty hard

    // Theoretically I could calculate the exact location of each element of my configuration struct in EEPROM and
    // only change that, but it's too complicated and memory intensive for this project

    // Only config commands 0 - 15 require writing to EEPROM
    if (i < 16) {
        writeConfigToEEPROM();
    }
}


/**
=================================================== \n
======================= Setup ======================= \n
===================================================
*/

void setup() {
    // -- Configure LEDs --
    leds.init();

    // Create RGB struct for each necessary color
    setUpColors();

    // -- Configure buttons --
    pinMode(greenButtonPIN, INPUT_PULLUP);
    pinMode(redButtonPIN, INPUT_PULLUP);

    // -- Open serial communications and wait for port to open --
    Serial.begin(9600);

    while (!Serial) {}


    EEPROM.get(EEPROM_BOOL_programHasRunBefore, programHasRunBefore);


    // TODO remember to remove
    programHasRunBefore = false;

    if (programHasRunBefore) {
        // If this is not the first time the program is running since the arduino was flashed
        getConfigFromEEPROM();
    }
    else {
        // If this is the first time the program is running since the arduino was flashed
        defaultConfig();
        writeConfigToEEPROM();
        programHasRunBefore = true;
        EEPROM.put(EEPROM_BOOL_programHasRunBefore, programHasRunBefore);
    }

    // -- Check if RED button is pressed for 5 sec, go to config systemMode --
    if (!digitalRead(redButtonPIN)) {
        unsigned long counter = micros()+buttonPressTime;
        bool g = true;
        while (g) {
            //Serial.println("Red is pressed :" + String(counter-millis()));
            if (digitalRead(redButtonPIN)) {
                //Serial.println("Red is not pressed");
                g = false;
            }
            else if (micros() > counter) {
                switchMode(config);
                g = false;
            }
        }
    }
    else {
        switchMode(standard);
    }

    // -- Configure RTC --
    // Initialize Clock
    clock.begin();

    // Set default time in the clock
    //configureRTC();

    // -- Configure BME --
    configureBME();

    // -- Configure GPS --
    configureGPS();

    while(!SoftSerial.available()) {
        ; // Wait until SoftSerial is open
    }

    // -- Configure SD Card --
    configureSDCard();

    // -- Reserve space for Strings to avoid fragmentation --
    // Contains sensor data
    dataString.reserve(125);

    // Contains GPS data
    gpsData.reserve(75);

    // Strings to assemble file name
    fileDate.reserve(8);
    fileName.reserve(16);

    // -- Setup interrupts for buttons --
    // This is done last to prevent interrupts during 'setup()'
    attachInterrupt(digitalPinToInterrupt(greenButtonPIN), greenButtonInterrupt, CHANGE);
    attachInterrupt(digitalPinToInterrupt(redButtonPIN), redButtonInterrupt, CHANGE);
}

void loop() {
    if (nextMode == noMode) {
        switch (currentMode) {
            case standard:
                if (millis() > nextMeasureTimer) {
                    // Set time for next measure
                    nextMeasureTimer = millis() + currentSystemConfiguration.LOG_INTERVALL;

                    // Reset dataString
                    dataString = "";

                    // Execute Mode
                    standardMode();
                }
                break;

            case economic:
                if (millis() > nextMeasureTimer) {
                    // Set time for next measure
                    nextMeasureTimer = millis() + (currentSystemConfiguration.LOG_INTERVALL * 2);

                    // Reset dataString
                    dataString = "";

                    // Execute Mode
                    economicMode();
                }
                break;

            case maintenance:
                // Set time for next measure
                nextMeasureTimer = millis() + currentSystemConfiguration.LOG_INTERVALL;

                // Reset dataString
                dataString = "";

                // Execute Mode
                maintenanceMode();
                break;

            case config:
                // Execute Mode

                // Switch modes if 'switchModeTimer' is exceeded
                if (millis() > switchModeTimer) {
                    switchMode(standard);
                }

                // Go into config mode if there is something in Serial
                else if (Serial.available() > 0) {
                    configMode();
                }
                break;

            case noMode:
                // 'noMode' is not allowed as a system mode, switchMode() will not allow switching to it
                break;
        }

    }
    else if (micros() > switchModeTimer) {
        greenButtonPressed = false;
        redButtonPressed = false;
        switchMode(nextMode);
    }
}

