#ifndef Audio_h_
#define Audio_h_
#include <cstring>

#include "common.h"

#include "AudioStream.h"
#include "softcodecs/AudioSourceFile.h"

#include "analyze_peak.h"
#include "analyze_print.h"
#include "analyze_rms.h"
#include "control_es8311.h"
#include "control_es8388.h"
#include "effect_bitcrusher.h"
#include "effect_delay.h"
#include "effect_envelope.h"
#include "effect_fade.h"
#include "effect_granular.h"
#include "effect_multiply.h"
#include "filter_biquad.h"
#include "filter_fir.h"
#include "mixer.h"
#include "output_i2s.h"
#include "play_aac.h"
#include "play_memory.h"
#include "play_mp3.h"
#include "play_queue.h"
#include "play_sd_wav.h"
#include "synth_dc.h"
#include "synth_sine.h"
#include "synth_whitenoise.h"
#include "synth_waveform.h"

#endif
