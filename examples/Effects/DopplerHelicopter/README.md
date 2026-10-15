# Doppler Helicopter

This example demonstrates variable playback-rate control with `AudioPlaySdWav`.
The helicopter approaches from the right, passes the listener, recedes to the left,
turns around at a distance, and returns along the same path.

## Sound file

Download the original WAV from Freesound:

- **Title:** Helicopter
- **Author:** mrspivey
- **Source:** https://freesound.org/people/mrspivey/sounds/805192/
- **License:** Creative Commons 0 (CC0)
- **Format:** WAV, 44.1 kHz, 16-bit, stereo, about 15.5 seconds

Copy the WAV to the SD card as `/Helicopter.wav`. The download may require a Freesound account.
The sound itself is not committed to this repository; obtain it from the source page.

## Motion model

The example uses a cosine flight path between a distant point on the right and a distant
point on the left. The radial component of velocity determines the playback rate using
the Doppler relation:

`rate = c / (c + radialVelocity)`

Here `c` is the speed of sound. A negative radial velocity means the source is approaching
and makes the sample play faster/higher; a positive radial velocity means it is receding
and makes the sample play slower/lower. Near the closest pass the radial velocity changes
sign through zero.

A small example-local stereo panner uses the WAV's left channel as its mono source signal.
Pan follows the helicopter's horizontal position, and gain decreases with distance.
At the far left the model reverses direction and repeats the same path back to the right.

## Hardware and build

The example uses the same SPI SD-card and I2S codec pin definitions as the repository's
`platformio.ini` ESP32 Audio Kit environment. Build it with that environment and copy
`Helicopter.wav` to the card.

The example has not been compiled or verified on physical hardware in this change.
