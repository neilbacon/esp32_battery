# Battery Powered IoT Devices

## Goal
To build ESP32 based IoT devices that run as long as possible on a battery. 

## Introduction

Having the ESP32 in deep sleep mode most of the time and paying attention to the power draw of any components still powered in this mode seems like a good place to start.

[Guide to Reduce the ESP32 Power Consumption by 95%](https://diyi0t.com/reduce-the-esp32-power-consumption/) shows that of the boards tested, DFRobot's FireBeetle has the lowest deep sleep power draw. It also has a connector and charger (from USB power) for a 3.7V Lipoly battery. 

We are using [Home Assistant (HA)](https://www.home-assistant.io/) as the IoT hub/controller/user interface and generate firmware with:

 - [ESPHome](https://esphome.io/) for IoT using Wifi; and 
 - [Arduino IDE](https://www.arduino.cc/en/Main/Software_) for IoT firmware using Bluetooth LE (without WiFi). 

## Experiment 1

`esppir1.yaml` is the ESPHome input file for our first firmware. This uses an [AS312 (AM312) Mini PIR module](https://unusualelectronics.co.uk/as312-am312-mini-pir-module-review/) to wake up the ESP32 from deep sleep when motion is detected and send the updated state to HA using its native API.

The PIR sensor has to always be powered, but draws very little power. For now I'm using a cheap ESP-WROOM-32 board without particularly good power characteristics.

![ESP-WROOM-32 board with AS312 PIR module](images/ex1.jpg)

Issues:

1. PIR sensor on duration (1 sec) is shorter than the time it takes the ESP32 to startup and the HA api to connect to it. WiFi connection takes 1-2 secs, but it is ~6 secs before the HA API is connected. 
2. State changes before the API is connected are not reflected in the UI after the API connects.
3. Even at the lowest priority, `on_boot` runs before the API is connected.
4. The template_binary_sensor `motion` can be programatically controlled, but the gpio binary_sensor `pir` cannot.
5. Awake from deep sleep is the same as RST or first boot; (non-RTC) memory is initialised from scratch and `setup()` is run, then `loop()` (these are Aduino IDE entry points implemented under-the-hood by ESPHome). I thought that power cycle would clear RTC memory, but it appears to be non-volatile (at least on my current test board) and survive power cycle, RST, OTA update, as well as deep sleep. This means it cannot be used to distinguish between deep sleep wake up and other types of start up. I had wanted to toggle 'motion' (see below) only on deep sleep wake up. 

Solutions:

1. Use template_binary_sensor `motion` as the high level motion sensor.
2. Under normal operation (`state = 3` in the yaml) `pir` state changes are copied to `motion`.
3. When `pir` wakes the ESP32 `on_boot` is run. It waits for the HA api to connect (or timeout because we don't want to flatten the battery waiting too long) then runs `startup_motion_toggle`.
4. `startup_motion_toggle` turns `motion` on then off (to reflect that `pir` has gone on and off before the HA api was connected) then sets normal operation (`state == 3` in the yaml).
5. `sleep_timer` is restarted when `pir` changes state. When it expires we enter deep sleep.

Waiting for the HA API to connect is too slow for many applications e.g. to turn on a light for safety or start video recording for security. Using a static IP could shave a little off the WiFi connection time, but not enough to change this.

The long connection delay wouldn't matter for some applications e.g. an analogue soil moisture sensor that sends one or more values each time it wakes up then sleeps a configured period (e.g. 30 min). The sensor need not be powered in deep sleep.

## Experiment 2

Try using MQTT instead of the HA native API. As the MQTT connection is under the control of the IoT device (rather that waiting for HA to poll it) it should be quicker to establish. The logic should be simpler without any need for the template_binary_sensor `motion`.

## Next steps

1. Make a low power binary_sensor using a Bluetooth LE server (no WiFi) and deep sleep. Esphome can't run without WiFi, but it's easy enough with Arduino IDE. 
2. The same connection time issue will arise. A phone app 'nRF Connect' can be used as a test client. Another non-battery powered ESP32 will be needed as a proxy to connect to up to 3 Bluetooth LE servers (Arduino ESP32 BLE stack can't handle more than 3) and relay to HA via WiFi.
3. BLE mesh networking sounds interesting; Arduino IDE can't do it; Exspressif IDE can but it sounds complicated to implement. 
    

