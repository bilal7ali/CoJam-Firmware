// Stop and raise the door if an obstruction is
// encountered while lowering.

#include <Bounce2.h>  // To handle switch closure

// Instantiate a Bounce object
// Bounce switchInputdebouncer = Bounce();
Bounce debouncedSwitch = Bounce();

const unsigned char switchInput = 10;  //        Arduino pin 10 --| |--SW-- GND 
const unsigned char obstructionSwitch = 11;  //  +5V--| |--SW--DIO11
const unsigned char openContactor = 9; //        +5--/\/\/- 330Ω -->|--DIO9
const unsigned char closeContactor = 7; //       +5--/\/\/- 330Ω -->|--DIO7

#define motorRun LOW
#define motorStop HIGH
#define accumulatedMillis millis() - timerMillis

const unsigned long motorTimerPreset = 2000;  // two seconds
unsigned long timerMillis;  // For counting time increments

// The door has four possible states it can be in
// Let's give the states descriptive names
enum {doorIsDown, doorIsUp, doorOpening, doorClosing};
unsigned char doorState = doorIsDown;  // What the door is doing at any given moment.

bool switchClosed;
bool switchClosedSetup;
int counter;

void setup() {
  Serial.begin(115200);

  // After setting up the button, setup the Bounce instance :
  debouncedSwitch.attach(switchInput);
  debouncedSwitch.interval(5); // interval in ms

  pinMode(switchInput, INPUT_PULLUP);
  pinMode(obstructionSwitch, INPUT_PULLUP);
  pinMode(openContactor, OUTPUT);
  digitalWrite(openContactor, HIGH);
  pinMode(closeContactor, OUTPUT);
  digitalWrite(closeContactor, HIGH);
}

void loop() {

  // Update the Bounce instance :
  debouncedSwitch.update();
  // Get the updated value :
  bool switchValue = debouncedSwitch.read();

  switchClosed = (!switchValue and switchClosedSetup); //
  switchClosedSetup = switchValue;

  switch (doorState) {

    case doorIsDown: // Nothing happening, waiting for switchInput
      Serial.println("door down");
      if (switchClosed) {
        // Notice at this point the switchInput is a don't care
        timerMillis = millis(); // reset the timer
        doorState = doorOpening; // Advance to the next state
        break;
      }
      else {
        break; // Continue with rest of the program
      }

    case doorOpening:
      Serial.println("door opening");
      digitalWrite(openContactor, motorRun);
      //
      if (accumulatedMillis >= motorTimerPreset) { // Door up?
        digitalWrite( openContactor, motorStop); // Stop the motor
        doorState = doorIsUp;
        break;
      }
       else {
        break; // Continue with rest of the program
      }

    case doorIsUp:
      Serial.println("door up");
      if (switchClosed) {
        // After this point the switchInput is a don't care
        timerMillis = millis(); // reset the timer
        doorState = doorClosing; // Advance to the next state
        break;
      }
      else { // switchInput was pressed
        break;
      }

    case doorClosing:
      Serial.println("door closing");
      if (digitalRead(obstructionSwitch) == 0) { // Have we met an obstruction?
        // Yes, abort the closing sequence and go to the opening sequence
        digitalWrite(closeContactor, motorStop);
        doorState = doorOpening;
        break;
      }
      digitalWrite(closeContactor, motorRun); // Down contactor on
      if (accumulatedMillis >= motorTimerPreset) {
        digitalWrite(closeContactor, motorStop); // Stop the motor
        doorState = doorIsDown;  // Back to start point
        break;
      }
      else {
        break;
      }
    default:
      doorState = doorIsDown;
      break;
  }
}