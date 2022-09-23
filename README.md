# pizza-temp

Merges some examples from Twilio's twilio-arduino repository (https://github.com/phundal-twilio/twilio-arduino) to send BLE sensor data over network calls.

## Some things to note

* The ca_cert.h file used for the ssl connection currently contains cert info for the twilio function that acted as our backend.  This will have to be updated with the bearssl utiliy: https://openslab-osu.github.io/bearssl-certificate-utility/ for whatever endpoint your arduino device is attempting to connect to
* This version of the program only searches for a single BLE sensor to send data for (BLACKA_001).  Modify this filter value to find what you're looking for