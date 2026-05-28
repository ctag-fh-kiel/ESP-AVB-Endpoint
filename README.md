# ESP AVB Endpoint

An AVB endpoint implementation using the esp_avb component for ESP-IDF.
Builds for two targets:

- **ESP32-P4** — wired AVB endpoint with on-chip IEEE 1588 hardware
  timestamping. Full talker/listener with codec.
- **ESP32-C6** — wireless AVB endpoint over Wi-Fi STA. Same talker/
  listener stack on the wireless data plane, paired with an
  ESP-AVB-Bridge that bridges AVB Wi-Fi onto a wired AVB
  switch.

Hardware:

- **Wired endpoint (ESP32-P4):** Scramble offers developer hardware
  with this firmware pre-loaded at <www.scramble.tools>. You can also
  get a Waveshare ESP32-P4-ETH from other vendors and flash it
  yourself. This branch's wired audio setup is the ESP32-P4 audio cape
  with an AKM AK4619 codec.
- **Wireless endpoint (ESP32-C6):** any ESP32-C6 dev board. Pair it
  with an ESP-AVB-Bridge (see scrambletools/ESP-AVB-Bridge) to reach
  the wired AVB network. Codec/I2S can be left disabled for boards
  with no audio hardware (the c6 default in `sdkconfig.defaults.esp32c6`
  builds without codec for that reason).

This AVB implementation is based on the following standards:

- IEEE 1722-2016 (AVTP)
- IEEE 1722.1-2021 (ATDECC)
- IEEE 802.1Q-2022 (MSRP, MVRP)
- IEEE 802.1AS-2021 (gPTP, based on ESP-IDF PTPd implementation)
- IEEE 802.11 (FTM peer-delay, beacon Vendor IE FollowUpInformation
  carriage)

Currently supports:

- AVB talker and listener (both targets)
- Simultaneous input and output stream
- Class A or B streams over Ethernet, Class B streams over Wi-Fi
- AK4619 wired audio at 48 kHz with four local ADC channels and four
  local DAC channels carried in 8-channel AVB streams
- AAF PCM INT32, 8 channels, 32-bit depth, 6 samples/frame, 48 kHz
- AAF PCM INT32, 8 channels, 24-bit depth in a 32-bit container,
  6 samples/frame, 48 kHz
- IEC 61883-6 AM824 AM8-24, 48 kHz
- Control via ATDECC controller (tested with Hive)
- Wi-Fi STA endpoint with software-disciplined PTP clock, FTM
  peer-delay initiator, and beacon-IE FollowUpInformation consumer

Anticipated future support:

- Milan 1.3 certification or at least compatibility
- AVB community audio profile support (in draft)
- AVB Lite (works with any switch, no bandwidth guarantee)

## Building

Pick the target before building:

```
# Wired endpoint (default):
idf.py set-target esp32p4
idf.py build flash monitor

# Wireless endpoint:
idf.py set-target esp32c6
idf.py build flash monitor
```

Per-port topology lives in `esp_ptp`'s Kconfig and is set per target
via `sdkconfig.defaults.esp32c6` (Wi-Fi STA endpoint). The matching
`esp_avb` symbols are derived automatically — there is no separate AVB
role switch.

## About this example

This application can operate as talker and/or listener. It uses the
esp_avb component, with this branch's ESP32-P4 default configured for
the AK4619 audio cape. The codec runs as 48 kHz TDM128 with 32-bit
slots; the AK4619 ADC data is 24-bit audio in those slots, and the DAC
path consumes 32-bit slots. The endpoint exposes four usable local
inputs and four usable local outputs while advertising 8-channel AVB
stream formats for interoperability with common macOS and MOTU AVB
endpoints.

The supported wired audio stream formats are:

- AAF PCM INT32, 8 channels, 32-bit depth, 6 samples/frame, 48 kHz
- AAF PCM INT32, 8 channels, 24-bit depth in a 32-bit container,
  6 samples/frame, 48 kHz
- IEC 61883-6 AM824 AM8-24, 48 kHz

The esp_avb component still contains ES8311 and ES8388 codec support,
but the actively tested hardware configuration for this branch is the
AK4619 ESP32-P4 audio cape. The example demonstrates how esp_avb can be
dropped into an ESP-IDF audio application to add AVB connectivity for
realtime low-latency audio routing.

The same `main/avb_endpoint.c` source serves both targets — the
medium-specific bring-up (Ethernet vs Wi-Fi STA + FTM + beacon-IE
consumer) is gated by `CONFIG_ESP_PTP_PORT0_MEDIUM_*`.

## Controller

There is a simple command-line ATDECC controller in the
avbcommunity/tools repo for basic connectivity testing. The main
application is intended to showcase the talker and listener
functionality of AVB, so it requires a controller to make a
connection. It has been tested with the Hive AVB controller
(<https://github.com/christophe-calmejane/hive>), the avb_tools
controller, and the Apple ATDECC controller (built into MacOS).

## Hardware notes

- **ESP32-P4 wired endpoint:** tested on an ESP32-P4 board with the
  AK4619 audio cape. The firmware powers the cape LDO, configures the
  AK4619 over I2C, and uses I2S0 in TDM128 mode with these pins:
  MCLK 53, BCLK 47, WS 48, DOUT 46, DIN 6, I2C SCL 8, I2C SDA 7, and
  codec reset 22. The ESP32-P4 is required for wired operation because
  it has the on-chip IEEE 1588 hardware timestamping; older ESP32 SoCs
  with EMAC do not support hardware timestamping and will not meet the
  AVB sync precision requirements.
- **ESP32-C6 wireless endpoint:** tested on a generic ESP32-C6 dev
  board. The c6 has no on-chip MAC, so the PTP daemon disciplines a
  software clock backed by `esp_timer` (`ptp_clock_sw.c`). Time sync
  arrives out-of-band: 802.11 beacon Vendor IE for FollowUpInformation
  and FTM for peer-delay. The wireless endpoint requires an AVB bridge
  to terminate Wi-Fi AVB onto a wired AVB switch — see
  scrambletools/ESP-AVB-Bridge.

Compatibility testing for this branch has been performed with:

- macOS Audio MIDI Setup and the Apple virtual AVB endpoint device
- Sonnet Technologies Thunderbolt AVB adapter connected to a MacBook Pro
- MOTU UltraLite AVB as AVB listener for ESP32-P4 talker tests
- MOTU 624 and MOTU AVB devices for discovery/routing checks
- Netgear M4250 switch in mixed AVB/RAVENNA/AES67 lab networks
- Direct Sonnet-to-ESP32-P4 wired AVB links for low-latency and PTP
  stability testing

## Open source

This application and the esp_avb component were initially developed by
Scramble Tools LLC, and they are provided as open source software for
the AVB/Milan developer community under the MIT software license to
encourage further development of low cost products and solutions for
AVB audio networking.

## Feedback

Please provide feedback or pull requests via the Github repository.
