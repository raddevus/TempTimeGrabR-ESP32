#include <TMP36.h>
#include <Wire.h>
#include <ds3231.h>
#include <SD.h>
#include <EEPROM.h>
#include <HardwareSerial.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

#define SCREEN_WIDTH 128 // OLED display width, in pixels
#define SCREEN_HEIGHT 64 // OLED display height, in pixels
#define OLED_RESET     4 // Reset pin # (or -1 if sharing Arduino reset pin)

#define SCREEN_ADDRESS 0x3C ///< See datasheet for Address; 0x3D for 128x64, 0x3C for 128x32
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

TMP36 tmp36_3v(A3, 3.29); //PIN 39

struct ts t;
const int DATA_BTN = 2;
const int DATA_LED = 36; // was A2
const int ROOM_BTN = 6;

// EEPROM Memory location (index) where last room idx is stored
const int ROOMIDX_FIRST_BYTE = 0;

bool dataBtnPrev = LOW;
bool dataBtnCurrent = LOW;

bool roomBtnPrev = LOW;
bool roomBtnCurrent = LOW;

unsigned long lastWriteTime = 0;

const int CS_PIN = 8;
bool isSDCardInitialized = false;
bool isWritingData = false;
// ROOM_COUNT must match number of rooms defined in allRooms array.
const int ROOM_COUNT = 7;
const char* allRooms[ROOM_COUNT]= {"basement","master","office","living","laundry","dining", "master bath"};
int currentRoomIdx = 0;
char currentRoom[26]; // allow 25 bytes for room name (1 for \0)
bool changeRoomBtnCurrent = false;
unsigned long lastTempReadMillis = 0;
float currentTemp = 0;
float prevTemp = 0;
byte writeFlag = 0;
byte command = 0;
String outputStr = "";

typedef void (*ButtonPressHandler) (void);

void setup() {
  Serial.begin(9600);
  Serial1.begin(38400);  // this is what the bluetooth device writes to

  if(!display.begin(SSD1306_SWITCHCAPVCC, SCREEN_ADDRESS)) {
    Serial.println(F("SSD1306 allocation failed"));
  }

  //analogReference(VDD);
  // analogReference(EXTERNAL); // TODO FIND A replacement
  
  Wire.begin();
  DS3231_init(DS3231_CONTROL_INTCN);
  /*----------------------------------------------------------------------------
  In order to synchronise your clock module, insert timetable values below !
  ----------------------------------------------------------------------------*/
  DS3231_get(&t);
  if (t.year == 1900){
    t.hour=12;
    t.min=9;
    t.sec=30;
    t.mday=21;
    t.mon=05;
    t.year=2021;
 
    DS3231_set(t);
  }

  //CS pin must be configured as an output
  pinMode(CS_PIN, OUTPUT);
  pinMode(DATA_BTN, INPUT);
  pinMode(ROOM_BTN, INPUT);
  pinMode(DATA_LED, OUTPUT);
  
  //strcpy(currentRoom,allRooms[currentRoomIdx]);
  
  loadLastRoomUsed();
  // Serial1.begin(38400); // TODO ADD back in
  initSDCard();

}

void loop() {
  // #### Display All Info #####
  initOled();
  displayDateTime();
  displayTemp();
  displaySDCardMessage();
  // display.display() is called
  // once for all display.
  display.display();
  // ##########################
  
  if (!isWritingData){
    // do not allow the room to change
    // (if data is being written),
    // even if user clicks button
    checkButton(ROOM_BTN,roomBtnPrev,roomBtnCurrent,checkChangeRoomButton);
    setRoom();
  }
  else{
    if (millis() - lastWriteTime > 3000){
      readTemp();
      // next line insures that the temp is only written
      // if it changed in the last 5 seconds
      if (currentTemp != prevTemp){
        writeTempData();
      }
    }
  }

  checkButton(DATA_BTN, dataBtnPrev, dataBtnCurrent, checkWriteDataButton);
  
  // Handle BT Commands
  // Always initialize command to 0 (no-command)
  command = 0;
  // Get the latest command
  if (Serial1.available()){
    command = Serial1.read();
  }
  switch (command){
    case 49: { // ASCII Char 1 - get Temperature
      // It seemed as if this code returned too fast
      // when I had no Serial.println() so I'm adding a delay.
      // It could be the Android program itself.
      delay(5);
      Serial1.println(getString(currentRoom) + " : " + currentTemp);
      break;
    }
    case 50: { // ASCII char 2 - start data write
      // #### Allow Data Write start/stop to be done via BT
      // turns on logging of temp data (to SD card)
      // turn on data writing and LED
      delay(5);
      isWritingData = true;
      analogWrite(DATA_LED, 255);
      Serial1.println(getString(currentRoom) + " : Writing data.");
      break;
    }
    case 51: { // ASCII char 3 - stop data write
      // turns of logging of temp data
      // turn off data writing and LED
      isWritingData = false;
      analogWrite(DATA_LED, 0);
      Serial1.println(getString(currentRoom) + " : Stopped writing.");
      break;
    }
    case 52: { // ASCII char 4 - getStatus
      //get device date/time, room name, temp and status of data writing
      // To save memory I'm reusing the outputStr String instead of 
      // instantiating a new one.
      
      getTime(); // concats to outputStr -- just needs an \n;
      outputStr.concat("\n" + getString(currentRoom) + "\n");
      char buf[5];
      dtostrf(currentTemp, 4, 2, buf);  //4 is mininum width, 2 is precision
      outputStr.concat(buf);
      outputStr.concat("\nisWritingData: ");
      outputStr.concat(isWritingData ? "true" : "false");
      Serial1.println(outputStr);    
      break;
    }
    case 53: { // ASCII char 5 - retreive temperatue file
      if (isWritingData){
        // Don't want to retrieve data while program is capturing.
        Serial1.println("Program is writing to SD Card. Can't retrieve data right now.");
        return;
      }
      else{
        if (isSDCardInitialized){
        File dataFile = SD.open("2021T.csv", FILE_READ);

        if (dataFile) { 
          while (dataFile.available()) { //execute while file is available
              char letter = dataFile.read(); //read next character from file
              // I strip off the 13 found on each line of the 
              // text file, then I put just one back on (below 
              // in the Serial1.prinln(output) so that the SoBtEx Android
              // app will recognize the end of transmission. It's odd
              // but it works.
                if (letter != 13){
                  Serial1.print(letter);
                }
              }
              Serial1.println("");
              dataFile.close(); //close file
          }
        }
        else{
          Serial1.println("There doesn't seem to be an SD Card available to read from.");
        }
      }
      break;
    }
    case 54: { // ASCII char 6 - set device time
      String s;
      while (Serial1.available()){
        s.concat(char(Serial1.read()));
      }
      t.hour=atoi(s.substring(0,2).c_str());
      t.min=atoi(s.substring(2,4).c_str());
      t.sec=atoi(s.substring(4,6).c_str());
      t.mon=atoi(s.substring(6,8).c_str());
      t.mday=atoi(s.substring(8,10).c_str());
      t.year=atoi(s.substring(10).c_str());
 
      DS3231_set(t);
      break;
    }
  }

}

void loadLastRoomUsed(){
  currentRoomIdx = EEPROM.read(ROOMIDX_FIRST_BYTE);
  // insuring the currentRoomIdx is always a valid value.
  if (currentRoomIdx >= (ROOM_COUNT) || currentRoomIdx < 0){
    currentRoomIdx = 0;
  }
}

void writeTempData(){
  lastWriteTime = millis();
  //Write the room to EEPROM so it'll be loaded the next
  // time the device is restarted.
  writeDataToEEProm((byte)currentRoomIdx);
   File dataFile = SD.open("2021T.csv", FILE_WRITE);
   if (dataFile)
   {
    dataFile.print(currentRoom);
    dataFile.print(",");
    dataFile.print(getTime());
    dataFile.print(",");
    dataFile.println(currentTemp);
    dataFile.close(); //Data isn't written until we run close()!
   }
}

void checkChangeRoomButton(void){
  if (roomBtnPrev == LOW && roomBtnCurrent == HIGH){
    changeRoomBtnCurrent = !changeRoomBtnCurrent;
  }

  roomBtnPrev = roomBtnCurrent;
  if (changeRoomBtnCurrent){
    changeRoomBtnCurrent = false;
      if (currentRoomIdx == ROOM_COUNT-1)
      {
        currentRoomIdx = 0;
        return;
      }
      currentRoomIdx++;
  }
}

void checkWriteDataButton(){
  if (dataBtnPrev == LOW && dataBtnCurrent == HIGH){
    isWritingData = !isWritingData;
  }
  dataBtnPrev = dataBtnCurrent;
  if (isWritingData){
    // turn on data writing and LED
    analogWrite(DATA_LED, 255);
  }
  else{
    //turn off data writing and LED
    //isWritingData = false;
    analogWrite(DATA_LED, 0);
  }
}

void checkButton(const int BUTTON,  bool &last, bool &current, ButtonPressHandler handler ){
  current = debounce(last, BUTTON);              // Read debounced state
  if (last == LOW && current == HIGH)    // If it was pressed…
  {
    // This runs the code that we want to happen 
    // when the user presses the button.  The function ptr 
    // allows us to do different types of work
     handler();
  }
  last = current;                        // Reset button value
}

boolean debounce(boolean last, int button)
{
 boolean current = digitalRead(button);    // Read the button state
 if (last != current)                      // If it's different…
 {
  delay(5);                                // Wait 5ms
  current = digitalRead(button);           // Read it again
 }
 return current;                           // Return the current value
}

void setRoom(){
  strcpy(currentRoom,allRooms[currentRoomIdx]);
  display.setCursor(0,10);
  display.setTextSize(1);
  display.println(currentRoom);
  unsigned int roomNameLength = getString(currentRoom).length();
  byte displaySpaces = (byte)(20 - roomNameLength);
  char spaces[displaySpaces];
  memset(spaces, ' ', displaySpaces-1);
  spaces[displaySpaces] = '\0';
  //display.print(spaces);
  //display.display();
}

void initSDCard(){
  if (!SD.begin(CS_PIN))
  {
    return;
  }
  isSDCardInitialized = true;
}

void initOled(){
  display.clearDisplay();
  display.setTextSize(1);      // Normal 1:1 pixel scale
  display.setTextColor(SSD1306_WHITE); // Draw white text
  display.setCursor(0, 0);     // Start at top-left corner
  display.cp437(true);         // Use full 256 char 'Code Page 437' font
}

void displayTemp(){
  if (millis() < 1000){
    // We don't want to read or display
    // the temp when the device boots up
    // because temp module isn't ready yet.
    return;
  }
  display.setCursor(30,24);
  display.setTextSize(2);
  if (!isWritingData){
    // I'm controlling how often the temp module
    // is read from in an effort to determine if it
    // becomes more accurate.
    readTemp();
  }
  display.println(currentTemp);
  //display.display();
}

void displaySDCardMessage(){
  display.setCursor (0,46);
  display.setTextSize(1);
  if (isSDCardInitialized){
    if (isWritingData){
      display.println("Writing data...");
    }
    else{
      display.println("SD Card is ready.");
    }
  }
  else{
    display.println("Please insert SD Card");
    // try initializing again -- in case
    // sd card is added.
    initSDCard();
  }
}

void displayDateTime(){
  DS3231_get(&t);
  
  //Print Date
  display.println(getTime());
  display.println(currentRoom);
  display.setTextSize(2);
  display.setCursor(30,24);
  display.println(currentTemp);
}

String getTime(){
  DS3231_get(&t);
  outputStr = "";

  if (t.mon < 10){
    outputStr += "0";
  }
  outputStr += String(t.mon) + "/";
  
  if (t.mday < 10){
    outputStr += "0";
  }
  outputStr += String(t.mday) + "/" + String(t.year);
  outputStr += " ";
  if (t.hour < 10){
    outputStr += "0";
  }
  outputStr += String(t.hour) + ":";
  if (t.min < 10){
    outputStr += "0";
  }
  outputStr +=  String(t.min) + ".";
  if (t.sec < 10){
    outputStr += "0";
  }
  outputStr+= String(t.sec);
  // NOTE: do not concat a \n or it will 
  // show up on the lcd screen
  return outputStr;
}

void readTemp(){
  // only allowing the temp module to be read from every X seconds
  if ((millis() - lastTempReadMillis) < 3000){
    return;
  }
  float temperature = tmp36_3v.getTempF();
  // store currentTemp in prevTemp for later use.
  prevTemp = currentTemp;
  currentTemp = temperature;
  lastTempReadMillis = millis();
}

void writeDataToEEProm(byte targetValue){
  if (EEPROM.read(ROOMIDX_FIRST_BYTE) == targetValue){
    // Nothing to do since the value is already set.
    // We only want to write to EEPROM when it has actually
    // changed since we want to save EEPROM writes.
    return;
  }
  EEPROM.write(ROOMIDX_FIRST_BYTE, targetValue);
}

String getString(char arr[]) 
{
    String s = String(arr);
    return s;
}
