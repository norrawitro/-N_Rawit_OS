// N_Rawit_Box_Arduino.ino

// This is the main Arduino sketch for N Rawit Box

void setup() {
  // initialize serial communication
  Serial.begin(9600);
}

void loop() {
  // read sensor data and process it
  // example code here

  Serial.println("Hello, N Rawit Box!");
  delay(1000); // wait for a second
}