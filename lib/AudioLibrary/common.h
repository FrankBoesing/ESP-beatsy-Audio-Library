#pragma once

#ifndef AUDIO_BLOCK_SAMPLES
#define AUDIO_BLOCK_SAMPLES 128
#endif

#ifndef AUDIO_SAMPLE_RATE_EXACT
#define AUDIO_SAMPLE_RATE_EXACT 44100.0f
#endif

//Enable connections sorting feature
#ifndef AUDIO_STREAM_SORT_IO
#define AUDIO_STREAM_SORT_IO 1
#endif

//Cores:
#ifndef AUDIO_DECODER_CORE
#define AUDIO_DECODER_CORE 0
#endif

#ifndef AUDIO_PROCESSING_CORE
#define AUDIO_PROCESSING_CORE 1
#endif

//Additional metrics (disable for production)
//#define SOFTCODEC_METRICS 1

#ifndef SOFTCODEC_METRICS
#define SOFTCODEC_METRICS 0
#endif



//-------------------------------------------------------------------------------------------

#define ALWAYS_INLINE __attribute__((always_inline)) inline
#define OSIZE __attribute__((optimize("Os")))
#define OSPEED __attribute__((optimize("O3")))

#ifndef SOFTCODECS_COLD
#define SOFTCODECS_COLD OSIZE
#endif

#ifndef SOFTCODECS_HOT
#define SOFTCODECS_HOT OSPEED
#endif

#define AUDIO_SAMPLE_RATE AUDIO_SAMPLE_RATE_EXACT
