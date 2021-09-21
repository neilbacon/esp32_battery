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

### Hardware

![ESP-WROOM-32 board with AS312 PIR module](images/ex1.jpg)

This uses an [AS312 (AM312) Mini PIR module](https://unusualelectronics.co.uk/as312-am312-mini-pir-module-review/) to wake up the ESP32 from deep sleep when motion is detected and to send motion detection events. The PIR sensor has to always be powered, but draws very little power. Red and yellow LEDs are available to indicate state. For now I'm using a cheap ESP-WROOM-32 board without particularly good power characteristics.

### Firmware

`esppir1.yaml` is the ESPHome input file for our first firmware, which usies the HA native API.

LED usage:

- Red: switch name `awake`, indicates the ESP32 is awake 
- Yellow: switch name `stay_awake`, indicates deep sleep is inhibited e.g. to facilitate OTA updates. 

### Issues

1. PIR sensor on duration (1 sec) is shorter than the time it takes the ESP32 to startup and the HA api to connect to it. WiFi connection takes 1-2 secs, but it is ~6 secs before the HA API is connected and a change in state is reflected in the HA UI. 
2. State changes before the API is connected are not reflected in the UI after the API connects.
3. Even at the lowest priority, `on_boot` runs before the HA API is connected.
4. The template_binary_sensor `motion` can be programatically controlled, but the gpio binary_sensor `pir` cannot.
5. Awake from deep sleep is the same as RST or first boot; (non-RTC) memory is initialised and `setup()` is run, then `loop()` (these are Aduino IDE entry points implemented under-the-hood by ESPHome). I thought that power cycle would clear RTC memory, but it appears to be non-volatile (at least on my current test board) and survives power cycle, RST, OTA update, as well as deep sleep. This means it cannot be used to distinguish between deep sleep wake up and other types of start up. I had wanted to toggle `motion` (see below) only on deep sleep wake up. 

### Solutions

1. Use template_binary_sensor `motion` as the high level motion sensor.
2. Under normal operation (`state == 3` in the yaml) `pir` state changes are copied to `motion`.
3. When `pir` wakes the ESP32 `on_boot` is run. It waits for the HA api to connect (or timeout because we don't want to flatten the battery waiting too long) then runs `startup_motion_toggle`.
4. `startup_motion_toggle` turns `motion` on then off (to reflect that `pir` has gone on and off before the HA api was connected) then sets normal operation (`state = 3` in the yaml).
5. `sleep_timer` is restarted when `pir` changes state. When it expires we enter deep sleep.

### Conclusion

Waiting for the HA API to connect is too slow for many applications e.g. to turn on a light for safety or start video recording for security. Using a static IP could shave a little off the WiFi connection time, but not enough to change this.

The long connection delay wouldn't matter for some applications e.g. an analogue soil moisture sensor that sends one or more values each time it wakes up then sleeps a configured period (e.g. 30 min). The sensor need not be powered in deep sleep.

## Experiment 2

### Hardware

Same as `Experiment 1`.

### Firmware

`esppir2.yaml` is the ESPHome input file for our second firmware, this time using a static IP address and MQTT instead of the HA native API. LED usage is the same as in `Experiment 1`. As the MQTT connection is under the control of the IoT device (rather that waiting for HA to poll it) it should be quicker to establish. The logic is simpler without any need for the template_binary_sensor `motion` and the `state` variable.

### Issues

1. It still takes ~4s between a motion event and it being reflected in the HA UI.
2. No MQTT message is generated for the PIR on event that awakens the ESP32. A corresponding PIR off MQTT message is generated.
3. Turning the `awake` switch off and then immediately entering deep sleep, the change of switch state is not reflected in the HA UI (the corresponding MQTT message is not sent).   

### Solutions

1. In `on_boot` we manually send the missing PIR on MQTT message. The automatic PIR off MQTT message is transmitted immediately afterwards, so the on duration is very short. 
2. A 300ms delay between turning the `awake` switch off and entering deep sleep appears to be sufficient for the change of switch state to be reflected in the HA UI. 100ms is not sufficient.

### Conclusion

This is marginally faster (and simpler) than `Experiment 1`, but still too slow for many applications.

## Next steps

1. Make a low power binary_sensor using a Bluetooth LE server (no WiFi) and deep sleep. Esphome can't run without WiFi, but it's easy enough with Arduino IDE. 
2. The same connection time issue will arise. A phone app 'nRF Connect' can be used as a test client. Another non-battery powered ESP32 will be needed as a proxy to connect to up to 3 Bluetooth LE servers (Arduino ESP32 BLE stack can't handle more than 3) and relay to HA via WiFi.
3. BLE mesh networking sounds interesting; Arduino IDE can't do it; Exspressif IDE can but it sounds complicated to implement. 
    


