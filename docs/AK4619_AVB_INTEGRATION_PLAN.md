# AK4619 Four-Channel AVB Endpoint at 48 kHz

## Summary

Add the AK4619 as a selectable codec backend in the local `esp_avb` component and select it from `ESP-AVB-Endpoint` for the P4 audio cape hardware.

The initial supported mode will be:

- AVB format: AAF `INT32`, 4 channels, 48 kHz only
- Codec bus: I2S-compatible TDM128, 4 slots x 32 bits
- ESP32-P4 role: audio clock master using APLL and `MCLK = 384fs`
- ADC mapping: 24 meaningful ADC bits carried left-aligned in the 32-bit AAF sample; low 8 bits are padding
- DAC mapping: 32-bit AAF samples written directly to the four 32-bit DAC slots
- AM824: not advertised for the AK4619 backend

Do not advertise 96 or 192 kHz in this implementation. The AK4619 clock requirements and the ESP-IDF TDM receive-master constraint are incompatible for four-channel full-duplex operation above 48 kHz without external audio clocks.

## Interface Changes

- Extend `avb_codec_type_t` with `avb_codec_type_ak4619`.
- Make `avb_config_s::codec_type` caller-selectable by removing its `const` qualifier.
- Extend codec hardware configuration with:
  - configurable I2C controller number
  - optional codec power-down/reset GPIO for AK4619 sequencing
- Preserve ES8311/ES8388 behavior and defaults; AK4619 is selected explicitly by the endpoint application.
- Add AK4619 codec capabilities:
  - `sample_rates = {48000}`
  - `bit_rates = {32}`
  - `max_input_channels = 4`
  - `max_output_channels = 4`
  - DAC volume range `+12.0 dB` to `-115.0 dB` in `0.5 dB` steps, with mute handling
  - microphone gain range `-6 dB` to `+27 dB` in `3 dB` steps

## Implementation Changes

### Codec Backend

- Add a private AK4619 backend inside `esp_avb`, derived from the working `p4_audio_cape` driver rather than changing `esp_codec_dev`.
- Refactor it to consume `avb_config_s` pins, rate, volume, and gain instead of hardcoded demo constants.
- Implement:
  - PDN/reset assertion and release timing
  - I2C probe at address `0x10`
  - register read/write helpers
  - TDM128 I2S-compatible register configuration
  - `48 kHz / 384fs` system-clock register configuration
  - four-channel DAC volume updates
  - four-channel ADC microphone-gain updates
- Keep `esp_codec_dev` in use for the existing ES codec backends; dispatch AK4619 controls through its private backend.

### I2S And Audio Transport

- Select TDM initialization when `codec_type == avb_codec_type_ak4619`; retain standard stereo I2S for existing codecs.
- Configure AK4619 TDM as:
  - slots `0..3`
  - 32-bit slot width and 32-bit DMA samples
  - Philips/I2S-compatible TDM128 framing
  - `big_endian = true` to maintain direct AAF byte ordering
  - APLL clock source with `MCLK = 384fs`
- Generalize AVTP audio conversion so codec geometry is not hardcoded as stereo 24-bit:
  - AK4619 talker reads four 32-bit TDM samples and emits four AAF `INT32` samples unchanged; ADC padding bits remain zero.
  - AK4619 listener accepts four AAF `INT32` samples and writes four 32-bit TDM samples unchanged.
  - Existing ES codec stereo/24-bit conversion remains unchanged.
- Update listener media-clock byte-rate accounting from the active codec format; for AK4619 it is `48000 * 4 * 4`.

### Advertising And Control

- For AK4619, advertise only one audio stream format per direction: four-channel AAF `INT32` at 48 kHz.
- Do not add AM824 entries to the AK4619 supported-format list.
- Configure the AEM audio unit and channel maps as four inputs and four outputs.
- Reject unsupported rate or format requests through AECP rather than accepting a format the hardware cannot play.
- Runtime sample-rate switching is out of scope because the initial AK4619 capability list contains only 48 kHz.

### Endpoint Application Configuration

- In `ESP-AVB-Endpoint`, select `avb_codec_type_ak4619` for the wired P4 audio-cape endpoint and configure:
  - `i2s_port = 0`
  - `MCLK = GPIO53`
  - `BCLK = GPIO47`
  - `WS/LRCK = GPIO48`
  - host `DOUT` to codec `SDIN1 = GPIO46`
  - codec `SDOUT1` to host `DIN = GPIO6`
  - `I2C SDA = GPIO7`
  - `I2C SCL = GPIO8`
  - `I2C port = 1`
  - `PDN/reset = GPIO22`
  - no PA enable GPIO unless the cape schematic identifies one
- Set application audio policy to 4 input channels, 4 output channels, 4 channels per stream, AAF `INT32`, and allowed sample rates `{48000}`.

## Test Plan

- Build validation:
  - Build `esp_avb` through the local `ESP-AVB-Endpoint` override.
  - Confirm ES8311/ES8388 builds still compile with their unchanged paths.
- Codec bring-up validation:
  - Confirm AK4619 responds at I2C address `0x10`.
  - Read back interface, system clock, and power registers after initialization.
  - Verify measured clocks at 48 kHz: LRCK `48 kHz`, BCLK `6.144 MHz` (`128fs`), MCLK `18.432 MHz` (`384fs`).
- Local audio validation:
  - Generate a four-channel test tone and confirm output on each DAC channel.
  - Loop or inject analog input into each ADC channel and verify nonzero independent capture data.
  - Confirm ADC AAF samples preserve 24 meaningful bits with padded low bits.
- AVB validation:
  - Confirm Audio MIDI Setup/controller enumerates one four-channel AAF `INT32`, 48 kHz talker and listener format.
  - Send four-channel audio from the Mac to the endpoint and verify DAC channel ordering.
  - Receive all four ADC channels on the Mac and verify channel ordering and stable streaming through the M4250 switch.
  - Confirm requests for AM824, 96 kHz, or 192 kHz are rejected as unsupported.
- Regression validation:
  - Confirm gPTP lock and stream stability remain intact during bidirectional four-channel streaming.
  - Confirm existing default ES codec configuration remains available when AK4619 is not selected.

## Assumptions

- The P4 audio cape wiring matches the working demo project pinout and uses AK4619 `SDIN1`/`SDOUT1` in TDM mode.
- The AK4619 is operated as a slave; ESP32-P4 supplies MCLK, BCLK, and LRCK.
- 48 kHz is the only supported rate in this phase because it is the only four-channel full-duplex TDM master mode compatible with both the AK4619 clock table and the current ESP-IDF TDM driver.
- Future 96/192 kHz support requires a separate investigation using an external synchronous audio clock source and ESP TDM slave mode, or validated changes below the current ESP-IDF driver constraint.
