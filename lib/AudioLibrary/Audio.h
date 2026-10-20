#ifndef Audio_h_
#define Audio_h_

#include <Arduino.h>
#include <esp_log.h>
#include <cstring>


#include "common.h"

#include "AudioStream.h"
#include "AudioSampleBuffer.h"
#include "softcodecs/AudioSourceFile.h"

#include "analyze_fft1024.h"
#include "analyze_fft256.h"
#include "analyze_peak.h"
#include "analyze_print.h"
#include "analyze_rms.h"
#include "control_es8311.h"
#include "control_es8388.h"
#include "effect_bitcrusher.h"
#include "effect_chorus.h"
#include "effect_combine.h"
#include "effect_delay.h"
#include "effect_envelope.h"
#include "effect_fade.h"
#include "effect_freeverb.h"
#include "effect_flange.h"
#include "effect_granular.h"
#include "effect_midside.h"
#include "effect_multiply.h"
#include "effect_reverb.h"
#include "effect_rectifier.h"
#include "effect_waveshaper.h"
#include "effect_wavefolder.h"
#include "filter_biquad.h"
#include "filter_dcblock.h"
#include "filter_fir.h"
#include "filter_ladder.h"
#include "filter_variable.h"
#include "input_i2s.h"
#include "input_i2s2.h"
#include "input_i2s3.h"
#include "mixer.h"
#include "output_i2s.h"
#include "output_i2s2.h"
#include "output_i2s3.h"
#include "record_file.h"
#include "output_spdif.h"
#include "output_spdif2.h"
#include "output_spdif3.h"
#include "output_pdm.h"
#include "input_usb.h"
#include "output_usb.h"
#include "play_aac.h"
#include "play_memory.h"
#include "play_mp3.h"
#include "play_queue.h"
#include "record_queue.h"
#include "play_sd_wav.h"
#include "synth_dc.h"
#include "synth_sine.h"
#include "synth_tonesweep.h"
#include "synth_whitenoise.h"
#include "synth_pinknoise.h"
#include "synth_waveform.h"
#include "synth_wavetable.h"
#include "synth_pwm.h"
#include "synth_simple_drum.h"
#include "synth_karplusstrong.h"
#include "synth_handpan.h"


// Always route Serial to the library-managed USB CDC port when native
// USB Audio is supported. This does not depend on AUDIO_USB_REDIRECT_SERIAL
// or another sketch-level PlatformIO flag. Use Serial0 for physical UART.
#if defined(SOC_USB_OTG_SUPPORTED) && SOC_USB_OTG_SUPPORTED && \
    defined(CONFIG_TINYUSB_AUDIO_ENABLED) && CONFIG_TINYUSB_AUDIO_ENABLED
#undef Serial
#define Serial AudioUSBSerial()
#endif

#endif
