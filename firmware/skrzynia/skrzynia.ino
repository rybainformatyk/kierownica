#include <Joystick.h>

Joystick_ Joystick;

void setup() {
  // put your setup code here, to run once:
  Serial.begin(9600);
  Joystick.begin();

}

void loop() {
  // put your main code here, to run repeatedly:
  int x = analogRead(A0);
  int y = analogRead(A1);
  Serial.print("x: "); Serial.print(x);
  Serial.print("y: "); Serial.println(y);
  

  if (x>600){
    if(y>600){
      Joystick.pressButton(1);
    }else if (y>500 && y<600) {
      Joystick.pressButton(2);
    }
    else if (y<500) {
      Joystick.pressButton(3);
    }
  }
  else {
  Joystick.releaseButton(1);
  Joystick.releaseButton(2);
  Joystick.releaseButton(3);
  }
  if (x<500){
    if(y>600){
      Joystick.pressButton(4);
    }else if (y>500 && y<600) {
      Joystick.pressButton(5);
    }
    else if (y<500) {
      Joystick.pressButton(6);
    }
  }
  else {
  Joystick.releaseButton(4);
  Joystick.releaseButton(5);
  Joystick.releaseButton(6);
  }


}