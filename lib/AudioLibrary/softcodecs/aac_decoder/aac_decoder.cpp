/*
 * aac_decoder.cpp
 * libhelix_HAACDECODER
 *
 *  Created on: 26.10.2018
 *  Updated on: 22.05.2024
 *  Updated on: 03.10.2026 //FB
 ************************************************************************************/

#include "aac_decoder.h"
#include "aac_decoder_consts.h"

//----------------------------------------------------------------------------------------------------------------------
static inline int32_t MULSHIFT32(int32_t x, int32_t y) {
    int32_t z;
    z = (int64_t)x * (int64_t)y >> 32;
    return z;
}
static inline int32_t CLZ(int32_t x) {
    return x == 0 ? 32 : __builtin_clz(x);
} // calls(0) exist.
static inline int32_t FASTABS(int32_t x) {
    return __builtin_abs(x);
} //FB
static inline uint16_t REV16(uint16_t value) {
    return __builtin_bswap16(value);
} //FB
static inline uint32_t REV32(uint32_t value) {
    return __builtin_bswap32(value);
}; //FB
static inline int64_t MADD64(int64_t sum64, int32_t x, int32_t y) {
    sum64 += (int64_t)x * (int64_t)y;
    return sum64;
}
static inline int16_t CLIPTOSHORT(int32_t x) {
    if (x > 32767)
        return 32767;
    if (x < -32768)
        return -32768;
    return (int16_t)x;
} // Heutige compiler erkennen das und nutzen den Saturierungs-Assembler-Befehl // fb

static inline int32_t CLIP_2N(int32_t y, int32_t n) {
#ifdef __XTENSA__ //fb
    int32_t x = 1 << n;
    if (y < -x)
        y = -x;
    x--;
    if (y > x)
        y = x;
    return y;
#else
    int32_t sign = y >> 31;
    if (sign != (y >> n))
        y = sign ^ ((1 << n) - 1);
    return y;
#endif
}
static inline int32_t CLIP_2N_SHIFT30(int32_t y, int32_t n) {
    int32_t sign = y >> 31;
    if (sign != (y >> (30 - n)))
        y = sign ^ (0x3fffffff);
    else
        y = (y << n);
    return y;
}

static inline int32_t CLIP_2N_SHIFT30_4(int32_t y) {
    int32_t sign = y >> 31;
    if (sign != (y >> (30 - 4)))
        y = sign ^ (0x3fffffff);
    else
        y = (y << 4);
    return y;
}

//----------------------------------------------------------------------------------------------------------------------

/***********************************************************************************************************************
 * Function:    AACDecoder_AllocateBuffers
 *
 * Description: allocate all the memory needed for the AAC decoder
 *              try heap first, because it's faster
 *
 * Inputs:      none
 *
 * Outputs:     none
 *
 * Return:      false if not enough memory, otherwise true
 *
 **********************************************************************************************************************/

#ifdef CONFIG_IDF_TARGET_ESP32S3
// ESP32-S3: If there is PSRAM, prefer it
#define __malloc_heap_psram(size)                                                                                      \
    heap_caps_malloc_prefer(size, 2, MALLOC_CAP_DEFAULT | MALLOC_CAP_SPIRAM, MALLOC_CAP_DEFAULT | MALLOC_CAP_INTERNAL)
#else
// ESP32, PSRAM is too slow, prefer SRAM
#define __malloc_heap_psram(size)                                                                                      \
    heap_caps_malloc_prefer(size, 2, MALLOC_CAP_DEFAULT | MALLOC_CAP_INTERNAL, MALLOC_CAP_DEFAULT | MALLOC_CAP_SPIRAM)
#endif

bool AACDecoder::AACDecoder_AllocateBuffers(void) {
    /* here, sizes are: AACDecInfo_t:96 PSInfoBase_t:27364 ProgConfigElement_t*16:1312 PSInfoSBR_t:50788 */
#ifdef AAC_ENABLE_SBR
    if (!m_PSInfoSBR) {
        m_PSInfoSBR = (PSInfoSBR_t *)__malloc_heap_psram(sizeof(PSInfoSBR_t));
    }

    if (!m_PSInfoSBR) {
        log_e("OOM in SBR, can't allocate %d bytes\n", sizeof(PSInfoSBR_t));
        return false; // ERR_AAC_SBR_INIT;
    } else {
        log_d("AAC Spectral Band Replication enabled, %d additional bytes allocated", sizeof(PSInfoSBR_t));
    }
#endif

    /* these could fall back to PSRAM if not enough heap available */
    if (!m_AACDecInfo) {
        m_AACDecInfo = (AACDecInfo_t *)__malloc_heap_psram(sizeof(AACDecInfo_t));
    }
    if (!m_PSInfoBase) {
        m_PSInfoBase = (PSInfoBase_t *)__malloc_heap_psram(sizeof(PSInfoBase_t));
    }
    if (!m_pce[0]) {
        m_pce[0] = (ProgConfigElement_t *)__malloc_heap_psram(sizeof(ProgConfigElement_t) * 16);
    }

    if (!m_AACDecInfo || !m_PSInfoBase || !m_pce[0]) {
        log_e("not enough memory to allocate aacdecoder buffers");
        AACDecoder_FreeBuffers();
        return false;
    }

    // Clear Buffer
    memset(m_AACDecInfo, 0, sizeof(AACDecInfo_t));                //Clear AACDecInfo
    memset(m_PSInfoBase, 0, sizeof(PSInfoBase_t));                //Clear PSInfoBase
    memset(&m_AACFrameInfo, 0, sizeof(AACFrameInfo_t));           //Clear AACFrameInfo
    memset(&m_fhADTS, 0, sizeof(ADTSHeader_t));                   //Clear fhADTS
    memset(&m_fhADIF, 0, sizeof(ADIFHeader_t));                   //Clear fhADIS
    memset(m_pce[0], 0, sizeof(ProgConfigElement_t) * 16);        //Clear ProgConfigElement
    memset(&m_pulseInfo[0], 0, sizeof(PulseInfo_t) * 2);          //Clear PulseInfo
    memset(&m_aac_BitStreamInfo, 0, sizeof(aac_BitStreamInfo_t)); //Clear aac_BitStreamInfo
#ifdef AAC_ENABLE_SBR
    memset(m_PSInfoSBR, 0, sizeof(PSInfoSBR_t)); //Clear PSInfoSBR
    InitSBRState();
#endif

    m_AACDecInfo->prevBlockID = AAC_ID_INVALID;
    m_AACDecInfo->currBlockID = AAC_ID_INVALID;
    m_AACDecInfo->currInstTag = -1;
    for (int32_t ch = 0; ch < MAX_NCHANS_ELEM; ch++)
        m_AACDecInfo->sbDeinterleaveReqd[ch] = 0;
    m_AACDecInfo->adtsBlocksLeft = 0;
    m_AACDecInfo->tnsUsed = 0;
    m_AACDecInfo->pnsUsed = 0;

    return true;
}

/**************************************************************************************
 * Function:    AACFlushCodec
 *
 * Description: flush internal codec state (after seeking, for example)
 *
 * Inputs:      valid AAC decoder instance pointer (HAACDecoder)
 *
 * Outputs:     updated state variables in aacDecInfo
 *
 * Return:      0 if successful, error code (< 0) if error
 **************************************************************************************/
int32_t AACDecoder::AACFlushCodec() {
    int32_t ch;

    if (!m_AACDecInfo)
        return ERR_AAC_NULL_POINTER;

    /* reset common state variables which change per-frame
     * don't touch state variables which are (usually) constant for entire clip
     *   (nChans, sampRate, profile, format, sbrEnabled)
     */
    m_AACDecInfo->prevBlockID = AAC_ID_INVALID;
    m_AACDecInfo->currBlockID = AAC_ID_INVALID;
    m_AACDecInfo->currInstTag = -1;
    for (ch = 0; ch < MAX_NCHANS_ELEM; ch++)
        m_AACDecInfo->sbDeinterleaveReqd[ch] = 0;
    m_AACDecInfo->adtsBlocksLeft = 0;
    m_AACDecInfo->tnsUsed = 0;
    m_AACDecInfo->pnsUsed = 0;

    /* reset internal codec state (flush overlap buffers, etc.) */
    memset(m_PSInfoBase->overlap, 0, AAC_MAX_NCHANS * AAC_MAX_NSAMPS * sizeof(int32_t));
    memset(m_PSInfoBase->prevWinShape, 0, AAC_MAX_NCHANS * sizeof(int32_t));

    return ERR_AAC_NONE;
}
/***********************************************************************************************************************
 * Function:    AACDecoder_FreeBuffers
 *
 * Description: allocate all the memory needed for the AAC decoder
 *
 * Inputs:      none
 *
 * Outputs:     none
 *
 * Return:      none

 **********************************************************************************************************************/
void AACDecoder::AACDecoder_FreeBuffers(void) {
    //    uint32_t i = ESP.getFreeHeap();

    if (m_AACDecInfo) {
        free(m_AACDecInfo);
        m_AACDecInfo = NULL;
    }
    if (m_PSInfoBase) {
        free(m_PSInfoBase);
        m_PSInfoBase = NULL;
    }
    if (m_pce[0]) {
        free(m_pce[0]);
        m_pce[0] = NULL;
    }

#ifdef AAC_ENABLE_SBR
    if (m_PSInfoSBR) {
        free(m_PSInfoSBR);
        m_PSInfoSBR = NULL;
    } //Clear AACDecInfo
#endif

    //    log_i("AACDecoder: %lu bytes memory was freed", ESP.getFreeHeap() - i);
}

/***********************************************************************************************************************
 * Function:    AACDecoder_IsInit
 *
 * Description: returns AAC decoder initialization status
 *
 * Inputs:      none
 *
 * Outputs:     none
 *
 * Return:      true if buffers allocated, otherwise false

 **********************************************************************************************************************/
bool AACDecoder::AACDecoder_IsInit(void) {
    if (m_AACDecInfo && m_PSInfoBase && m_pce[0]) {
        return true;
    }
    return false;
}

bool AACDecoder::AllocateBuffers() {
    return AACDecoder_AllocateBuffers();
}

bool AACDecoder::IsInit() {
    return AACDecoder_IsInit();
}

void AACDecoder::FreeBuffers() {
    AACDecoder_FreeBuffers();
}

int32_t AACDecoder::Decode(uint8_t **inbuf, int32_t *bytesLeft, int16_t *outbuf) {
    if (inbuf == nullptr || *inbuf == nullptr || bytesLeft == nullptr || outbuf == nullptr || !AACDecoder_IsInit()) {
        return ERR_AAC_NULL_POINTER;
    }

    const int32_t bytesBefore = *bytesLeft;
    const int32_t result = AACDecode(*inbuf, bytesLeft, outbuf);
    if (result == ERR_AAC_NONE) {
        *inbuf += bytesBefore - *bytesLeft;
    }
    return result;
}

void AACDecoder::GetLastFrameInfo(AACFrameInfo *info) {
    if (info == nullptr) {
        return;
    }

    *info = {};
    if (m_AACDecInfo == nullptr) {
        return;
    }

    info->bitRate = m_AACDecInfo->bitRate;
    info->nChans = m_AACDecInfo->nChans;
    info->sampRateCore = m_AACDecInfo->sampRate;
    info->sampRateOut = AACGetSampRate();
    info->bitsPerSample = AACGetBitsPerSample();
    info->outputSamps = AACGetOutputSamps();
    info->profile = m_AACDecInfo->profile;
    info->tnsUsed = m_AACDecInfo->tnsUsed;
    info->pnsUsed = m_AACDecInfo->pnsUsed;
}

/***********************************************************************************************************************
 * Function:    AACDecoder_FreeBuffers
 *
 * Description: allocate all the memory needed for the AAC decoder
 *
 * Inputs:      none
 *
 * Outputs:     none
 *
 * Return:      none

 **********************************************************************************************************************/

/***********************************************************************************************************************
 * Function:    AACFindSyncWord
 *
 * Description: locate the next byte-alinged sync word in the raw AAC stream
 *
 * Inputs:      buffer to search for sync word
 *              max number of bytes to search in buffer
 *
 * Outputs:     none
 *
 * Return:      offset to first sync word (bytes from start of buf)
 *              -1 if sync not found after searching nBytes
 **********************************************************************************************************************/
int32_t AACDecoder::AACFindSyncWord(uint8_t *buf, int32_t nBytes) {
    int32_t i;

    /* find byte-aligned syncword (12 bits = 0xFFF) */
    for (i = 0; i < nBytes - 1; i++) {
        if ((buf[i + 0] & SYNCWORDH) == SYNCWORDH && (buf[i + 1] & SYNCWORDL) == SYNCWORDL)
            return i;
    }

    return -1;
}
//**************************************************************************************
int32_t AACDecoder::AACGetSampRate() {
    return m_AACDecInfo->sampRate * (m_AACDecInfo->sbrEnabled ? 2 : 1);
}
int32_t AACDecoder::AACGetChannels() {
    return m_AACDecInfo->nChans;
}
int32_t AACDecoder::AACGetBitsPerSample() {
    return 16;
}
int32_t AACDecoder::AACGetID() {
    return m_AACDecInfo->id;
} // 0-MPEG4, 1-MPEG2
uint8_t AACDecoder::AACGetProfile() {
    return (uint8_t)m_AACDecInfo->profile;
} // 0-Main, 1-LC, 2-SSR, 3-reserved
uint8_t AACDecoder::AACGetFormat() {
    return (uint8_t)m_AACDecInfo->format;
} // 0-unknown 1-ADTS 2-ADIF, 3-RAW
int32_t AACDecoder::AACGetOutputSamps() {
    return m_AACDecInfo->nChans * AAC_MAX_NSAMPS * (m_AACDecInfo->sbrEnabled ? 2 : 1);
}
int32_t AACDecoder::AACGetBitrate() {
    uint32_t br = AACGetBitsPerSample() * AACGetChannels() * AACGetSampRate();
    return (br / m_AACDecInfo->compressionRatio);
}
/**************************************************************************************
 * Function:    AACSetRawBlockParams
 *
 * Description: set internal state variables for decoding a stream of raw data blocks
 *
 * Inputs:      flag indicating source of parameters
 *              nChans, sampRate,
 *              and profile  0 = main, 1 = LC, 2 = SSR, 3 = reserved
 *                optionally filled-in
 *
 * Outputs:     updated codec state
 *
 * Return:      0 if successful, error code (< 0) if error
 *
 * Notes:       if copyLast == 1, then the codec sets up its internal state (for
 *                decoding raw blocks) based on previously-decoded ADTS header info
 *              if copyLast == 0, then the codec uses the values passed in
 *                aacFrameInfo to configure its internal state (useful when the
 *                source is MP4 format, for example)
 **************************************************************************************/
int32_t AACDecoder::AACSetRawBlockParams(int32_t copyLast, int32_t nChans, int32_t sampRateCore, int32_t profile) {
    if (!m_AACDecInfo)
        return ERR_AAC_NULL_POINTER;

    m_AACDecInfo->format = AAC_FF_RAW;
    if (copyLast)
        return AACDecoder::SetRawBlockParams(1, 0, 0, 0);
    else
        return AACDecoder::SetRawBlockParams(0, nChans, sampRateCore, profile);
}

/***********************************************************************************************************************
 * Function:    AACDecode
 *
 * Description: decode AAC frame
 *
 * Inputs:      double pointer to buffer of AAC data
 *              pointer to number of valid bytes remaining in inbuf
 *              pointer to outbuf, big enough to hold one frame of decoded PCM samples
 *
 * Outputs:     PCM data in outbuf, interleaved LRLRLR... if stereo
 *                number of output samples = 1024 per channel
 *              updated inbuf pointer
 *              updated bytesLeft
 *
 * Return:      0 if successful, error code (< 0) if error
 *
 * Notes:       inbuf pointer and bytesLeft are not updated until whole frame is
 *                successfully decoded, so if ERR_AAC_INDATA_UNDERFLOW is returned
 *                just call AACDecode again with more data in inbuf
 **********************************************************************************************************************/
int32_t AACDecoder::AACDecode(uint8_t *inbuf, int32_t *bytesLeft, int16_t *outbuf) {
    int32_t err, offset, bitOffset, bitsAvail;
    int32_t ch, baseChan, elementChans;
    uint8_t *inptr;

#ifdef AAC_ENABLE_SBR
    int32_t baseChanSBR, elementChansSBR;
#endif

    /* make local copies (see "Notes" above) */
    inptr = inbuf;
    bitOffset = 0;
    bitsAvail = (*bytesLeft) << 3;

    /* first time through figure out what the file format is */
    if (m_AACDecInfo->format == AAC_FF_Unknown) {
        if (bitsAvail < 32)
            return ERR_AAC_INDATA_UNDERFLOW;

        if ((inptr)[0] == 'A' && (inptr)[1] == 'D' && (inptr)[2] == 'I' && (inptr)[3] == 'F') {
            /* unpack ADIF header */
            m_AACDecInfo->format = AAC_FF_ADIF;
            err = UnpackADIFHeader(&inptr, &bitOffset, &bitsAvail);
            if (err)
                return err;
        } else {
            /* assume ADTS by default */
            m_AACDecInfo->format = AAC_FF_ADTS;
        }
    }
    /* if ADTS, search for start of next frame */
    if (m_AACDecInfo->format == AAC_FF_ADTS) {
        /* can have 1-4 raw data blocks per ADTS frame (header only present for first one) */
        if (m_AACDecInfo->adtsBlocksLeft == 0) {
            offset = AACFindSyncWord(inptr, bitsAvail >> 3);
            if (offset < 0)
                return ERR_AAC_INDATA_UNDERFLOW;
            inptr += offset;
            bitsAvail -= (offset << 3);

            err = UnpackADTSHeader(&inptr, &bitOffset, &bitsAvail);
            if (err)
                return err;

            if (m_AACDecInfo->nChans == -1) {
                /* figure out implicit channel mapping if necessary */
                err = GetADTSChannelMapping(inptr, bitOffset, bitsAvail);
                if (err)
                    return err;
            }
        }
        m_AACDecInfo->adtsBlocksLeft--;
    } else if (m_AACDecInfo->format == AAC_FF_RAW) {
        err = PrepareRawBlock();
        if (err)
            return err;
    }

    /* check for valid number of channels */
    if (m_AACDecInfo->nChans > AAC_MAX_NCHANS || m_AACDecInfo->nChans <= 0)
        return ERR_AAC_NCHANS_TOO_HIGH;

    /* will be set later if active in this frame */
    m_AACDecInfo->tnsUsed = 0;
    m_AACDecInfo->pnsUsed = 0;

    bitOffset = 0;
    baseChan = 0;

#ifdef AAC_ENABLE_SBR
    baseChanSBR = 0;
#endif

    do {
        /* parse next syntactic element */
        if (bitsAvail < 0)
            return ERR_AAC_INDATA_UNDERFLOW;
        err = DecodeNextElement(&inptr, &bitOffset, &bitsAvail);
        if (err)
            return err;

        elementChans = elementNumChans[m_AACDecInfo->currBlockID];
        if (baseChan + elementChans > AAC_MAX_NCHANS)
            return ERR_AAC_NCHANS_TOO_HIGH;

        /* noiseless decoder and dequantizer */
        for (ch = 0; ch < elementChans; ch++) {
            err = DecodeNoiselessData(&inptr, &bitOffset, &bitsAvail, ch);

            if (err)
                return err;

            if (AACDequantize(ch))
                return ERR_AAC_DEQUANT;
        }

        /* mid-side and intensity stereo */
        if (m_AACDecInfo->currBlockID == AAC_ID_CPE) {
            if (StereoProcess())
                return ERR_AAC_STEREO_PROCESS;
        }

        /* PNS, TNS, inverse transform */
        for (ch = 0; ch < elementChans; ch++) {
            if (PNS(ch))
                return ERR_AAC_PNS;

            if (m_AACDecInfo->sbDeinterleaveReqd[ch]) {
                /* deinterleave short blocks, if required */
                if (DeinterleaveShortBlocks(ch))
                    return ERR_AAC_SHORT_BLOCK_DEINT;
                m_AACDecInfo->sbDeinterleaveReqd[ch] = 0;
            }

            if (TNSFilter(ch))
                return ERR_AAC_TNS;

            if (IMDCT(ch, baseChan + ch, outbuf))
                return ERR_AAC_IMDCT;
        }

#ifdef AAC_ENABLE_SBR
        if (m_AACDecInfo->sbrEnabled &&
            (m_AACDecInfo->currBlockID == AAC_ID_FIL || m_AACDecInfo->currBlockID == AAC_ID_LFE)) {
            if (m_AACDecInfo->currBlockID == AAC_ID_LFE)
                elementChansSBR = elementNumChans[AAC_ID_LFE];
            else if (m_AACDecInfo->currBlockID == AAC_ID_FIL &&
                     (m_AACDecInfo->prevBlockID == AAC_ID_SCE || m_AACDecInfo->prevBlockID == AAC_ID_CPE))
                elementChansSBR = elementNumChans[m_AACDecInfo->prevBlockID];
            else
                elementChansSBR = 0;

            if (baseChanSBR + elementChansSBR > AAC_MAX_NCHANS)
                return ERR_AAC_SBR_NCHANS_TOO_HIGH;

            /* parse SBR extension data if present (contained in a fill element) */
            if (DecodeSBRBitstream(baseChanSBR))
                return ERR_AAC_SBR_BITSTREAM;

            /* apply SBR */
            if (DecodeSBRData(baseChanSBR, outbuf))
                return ERR_AAC_SBR_DATA;

            baseChanSBR += elementChansSBR;
        }
#endif

        baseChan += elementChans;
    } while (m_AACDecInfo->currBlockID != AAC_ID_END);

    /* byte align after each raw_data_block */
    if (bitOffset) {
        inptr++;
        bitsAvail -= (8 - bitOffset);
        bitOffset = 0;
        if (bitsAvail < 0)
            return ERR_AAC_INDATA_UNDERFLOW;
    }

    m_AACDecInfo->compressionRatio = (float)(AACGetOutputSamps()) * 2 / (inptr - inbuf);

    /* update pointers */
    m_AACDecInfo->frameCount++;
    *bytesLeft -= (inptr - inbuf);
    inbuf = inptr;

    return ERR_AAC_NONE;
}
/***********************************************************************************************************************
 * Function:    DecodeLPCCoefs
 *
 * Description: decode LPC coefficients for TNS
 *
 * Inputs:      order of TNS filter
 *              resolution of coefficients (3 or 4 bits)
 *              coefficients unpacked from bitstream
 *              scratch buffer (b) of size >= order
 *
 * Outputs:     LPC coefficients in Q(FBITS_LPC_COEFS), in 'a'
 *
 * Return:      none
 *
 * Notes:       assumes no guard bits in input transform coefficients
 *              a[i] = Q(FBITS_LPC_COEFS), don't store a0 = 1.0
 *                (so a[0] = first delay tap, etc.)
 *              max abs(a[i]) < log2(order), so for max order = 20 a[i] < 4.4
 *                (up to 3 bits of gain) so a[i] has at least 31 - FBITS_LPC_COEFS - 3
 *                guard bits
 *              to ensure no intermediate overflow in all-pole filter, set
 *                FBITS_LPC_COEFS such that number of guard bits >= log2(max order)
 **********************************************************************************************************************/
void AACDecoder::DecodeLPCCoefs(int32_t order, int32_t res, int8_t *filtCoef, int32_t *a, int32_t *b) {
    int32_t i, m, t;
    const uint32_t *invQuantTab;

    if (res == 3)
        invQuantTab = invQuant3;
    else if (res == 4)
        invQuantTab = invQuant4;
    else
        return;

    for (m = 0; m < order; m++) {
        t = invQuantTab[filtCoef[m] & 0x0f]; /* t = Q31 */
        for (i = 0; i < m; i++)
            b[i] = a[i] - (MULSHIFT32(t, a[m - i - 1]) << 1);
        for (i = 0; i < m; i++)
            a[i] = b[i];
        a[m] = t >> (31 - FBITS_LPC_COEFS);
    }
}

/***********************************************************************************************************************
 * Function:    FilterRegion
 *
 * Description: apply LPC filter to one region of coefficients
 *
 * Inputs:      number of transform coefficients in this region
 *              direction flag (forward = 1, backward = -1)
 *              order of filter
 *              'size' transform coefficients
 *              'order' LPC coefficients in Q(FBITS_LPC_COEFS)
 *              scratch buffer for history (must be >= order samples long)
 *
 * Outputs:     filtered transform coefficients
 *
 * Return:      guard bit mask (OR of abs value of all filtered transform coefs)
 *
 * Notes:       assumes no guard bits in input transform coefficients
 *              gains 0 int32_t bits
 *              history buffer does not need to be preserved between regions
 **********************************************************************************************************************/
int32_t AACDecoder::FilterRegion(int32_t size, int32_t dir, int32_t order, int32_t *audioCoef, int32_t *a,
                                 int32_t *hist) {
    int32_t i, j, y, hi32, inc, gbMask;
    U64 sum64;

    /* init history to 0 every time */
    for (i = 0; i < order; i++)
        hist[i] = 0;

    sum64.w64 = 0; /* avoid warning */
    gbMask = 0;
    inc = (dir ? -1 : 1);
    do {
        /* sum64 = a0*y[n] = 1.0*y[n] */
        y = *audioCoef;
        sum64.r.hi32 = y >> (32 - FBITS_LPC_COEFS);
        sum64.r.lo32 = y << FBITS_LPC_COEFS;

        /* sum64 += (a1*y[n-1] + a2*y[n-2] + ... + a[order-1]*y[n-(order-1)]) */
        for (j = order - 1; j > 0; j--) {
            sum64.w64 = MADD64(sum64.w64, hist[j], a[j]);
            hist[j] = hist[j - 1];
        }
        sum64.w64 = MADD64(sum64.w64, hist[0], a[0]);
        y = (sum64.r.hi32 << (32 - FBITS_LPC_COEFS)) | (sum64.r.lo32 >> FBITS_LPC_COEFS);

        /* clip output (rare) */
        hi32 = sum64.r.hi32;
        if ((hi32 >> 31) != (hi32 >> (FBITS_LPC_COEFS - 1)))
            y = (hi32 >> 31) ^ 0x7fffffff;

        hist[0] = y;
        *audioCoef = y;
        audioCoef += inc;
        gbMask |= FASTABS(y);
    } while (--size);

    return gbMask;
}

/***********************************************************************************************************************
 * Function:    TNSFilter
 *
 * Description: apply temporal noise shaping, if enabled
 *
 * Inputs:      index of current channel
 *
 * Outputs:     updated transform coefficients
 *              updated minimum guard bit count for this channel
 *
 * Return:      0 if successful, -1 if error
 **********************************************************************************************************************/
int32_t AACDecoder::TNSFilter(int32_t ch) {
    int32_t win, winLen, nWindows, nSFB, filt, bottom, top, order, maxOrder, dir;
    int32_t start, end, size, tnsMaxBand, numFilt, gbMask;
    int32_t *audioCoef;
    uint8_t *filtLength, *filtOrder, *filtRes, *filtDir;
    int8_t *filtCoef;
    const uint16_t *tnsMaxBandTab;
    const uint16_t *sfbTab;
    ICSInfo_t *icsInfo;
    TNSInfo_t *ti;

    icsInfo = (ch == 1 && m_PSInfoBase->commonWin == 1) ? &(m_PSInfoBase->icsInfo[0]) : &(m_PSInfoBase->icsInfo[ch]);
    ti = &m_PSInfoBase->tnsInfo[ch];

    if (!ti->tnsDataPresent)
        return 0;

    if (icsInfo->winSequence == 2) {
        nWindows = NWINDOWS_SHORT;
        winLen = NSAMPS_SHORT;
        nSFB = sfBandTotalShort[m_PSInfoBase->sampRateIdx];
        maxOrder = tnsMaxOrderShort[m_AACDecInfo->profile];
        sfbTab = sfBandTabShort + sfBandTabShortOffset[m_PSInfoBase->sampRateIdx];
        tnsMaxBandTab = tnsMaxBandsShort + tnsMaxBandsShortOffset[m_AACDecInfo->profile];
        tnsMaxBand = tnsMaxBandTab[m_PSInfoBase->sampRateIdx];
    } else {
        nWindows = NWINDOWS_LONG;
        winLen = NSAMPS_LONG;
        nSFB = sfBandTotalLong[m_PSInfoBase->sampRateIdx];
        maxOrder = tnsMaxOrderLong[m_AACDecInfo->profile];
        sfbTab = sfBandTabLong + sfBandTabLongOffset[m_PSInfoBase->sampRateIdx];
        tnsMaxBandTab = tnsMaxBandsLong + tnsMaxBandsLongOffset[m_AACDecInfo->profile];
        tnsMaxBand = tnsMaxBandTab[m_PSInfoBase->sampRateIdx];
    }

    if (tnsMaxBand > icsInfo->maxSFB)
        tnsMaxBand = icsInfo->maxSFB;

    filtRes = ti->coefRes;
    filtLength = ti->length;
    filtOrder = ti->order;
    filtDir = ti->dir;
    filtCoef = ti->coef;

    gbMask = 0;
    audioCoef = m_PSInfoBase->coef[ch];
    for (win = 0; win < nWindows; win++) {
        bottom = nSFB;
        numFilt = ti->numFilt[win];
        for (filt = 0; filt < numFilt; filt++) {
            top = bottom;
            bottom = top - *filtLength++;
            bottom = MAX(bottom, (int32_t)0);
            order = *filtOrder++;
            order = MIN(order, maxOrder);

            if (order) {
                start = sfbTab[MIN(bottom, tnsMaxBand)];
                end = sfbTab[MIN(top, tnsMaxBand)];
                size = end - start;
                if (size > 0) {
                    dir = *filtDir++;
                    if (dir)
                        start = end - 1;

                    DecodeLPCCoefs(order, filtRes[win], filtCoef, m_PSInfoBase->tnsLPCBuf, m_PSInfoBase->tnsWorkBuf);
                    gbMask |= FilterRegion(size, dir, order, audioCoef + start, m_PSInfoBase->tnsLPCBuf,
                                           m_PSInfoBase->tnsWorkBuf);
                }
                filtCoef += order;
            }
        }
        audioCoef += winLen;
    }

    /* update guard bit count if necessary */
    size = CLZ(gbMask) - 1;
    if (m_PSInfoBase->gbCurrent[ch] > size)
        m_PSInfoBase->gbCurrent[ch] = size;

    return 0;
}

/***********************************************************************************************************************
 * Function:    DecodeSingleChannelElement
 *
 * Description: decode one SCE
 *
 * Inputs:      none
 *
 * Outputs:     updated element instance tag
 *
 * Return:      0 if successful, -1 if error
 *
 * Notes:       doesn't decode individual channel stream (part of DecodeNoiselessData)
 **********************************************************************************************************************/
int32_t AACDecoder::DecodeSingleChannelElement() {
    /* read instance tag */
    m_AACDecInfo->currInstTag = GetBits(NUM_INST_TAG_BITS);

    return 0;
}

/***********************************************************************************************************************
 * Function:    DecodeChannelPairElement
 *
 * Description: decode one CPE
 *
 * Inputs:       none
 *
 * Outputs:     updated element instance tag
 *              updated commonWin
 *              updated ICS info, if commonWin == 1
 *              updated mid-side stereo info, if commonWin == 1
 *
 * Return:      0 if successful, -1 if error
 *
 * Notes:       doesn't decode individual channel stream (part of DecodeNoiselessData)
 **********************************************************************************************************************/
int32_t AACDecoder::DecodeChannelPairElement() {
    int32_t sfb, gp, maskOffset;
    uint8_t currBit, *maskPtr;
    ICSInfo_t *icsInfo;

    icsInfo = m_PSInfoBase->icsInfo;

    /* read instance tag */
    m_AACDecInfo->currInstTag = GetBits(NUM_INST_TAG_BITS);

    /* read common window flag and mid-side info (if present)
     * store msMask bits in m_PSInfoBase->msMaskBits[] as follows:
     *  long blocks -  pack bits for each SFB in range [0, maxSFB) starting with lsb of msMaskBits[0]
     *  short blocks - pack bits for each SFB in range [0, maxSFB), for each group [0, 7]
     * msMaskPresent = 0 means no M/S coding
     *               = 1 means m_PSInfoBase->msMaskBits contains 1 bit per SFB to toggle M/S coding
     *               = 2 means all SFB's are M/S coded (so m_PSInfoBase->msMaskBits is not needed)
     */
    m_PSInfoBase->commonWin = GetBits(1);
    if (m_PSInfoBase->commonWin) {
        DecodeICSInfo(icsInfo, m_PSInfoBase->sampRateIdx);
        m_PSInfoBase->msMaskPresent = GetBits(2);
        if (m_PSInfoBase->msMaskPresent == 1) {
            maskPtr = m_PSInfoBase->msMaskBits;
            *maskPtr = 0;
            maskOffset = 0;
            for (gp = 0; gp < icsInfo->numWinGroup; gp++) {
                for (sfb = 0; sfb < icsInfo->maxSFB; sfb++) {
                    currBit = (uint8_t)GetBits(1);
                    *maskPtr |= currBit << maskOffset;
                    if (++maskOffset == 8) {
                        maskPtr++;
                        *maskPtr = 0;
                        maskOffset = 0;
                    }
                }
            }
        }
    }

    return 0;
}

/***********************************************************************************************************************
 * Function:    DecodeLFEChannelElement
 *
 * Description: decode one LFE
 *
 * Inputs:      none
 *
 * Outputs:     updated element instance tag
 *
 * Return:      0 if successful, -1 if error
 *
 * Notes:       doesn't decode individual channel stream (part of DecodeNoiselessData)
 **********************************************************************************************************************/
int32_t AACDecoder::DecodeLFEChannelElement() {
    /* read instance tag */
    m_AACDecInfo->currInstTag = GetBits(NUM_INST_TAG_BITS);

    return 0;
}

/***********************************************************************************************************************
 * Function:    DecodeDataStreamElement
 *
 * Description: decode one DSE
 *
 * Inputs:      none
 *
 * Outputs:     updated element instance tag
 *              filled in data stream buffer
 *
 * Return:      0 if successful, -1 if error
 **********************************************************************************************************************/
int32_t AACDecoder::DecodeDataStreamElement() {
    uint32_t byteAlign, dataCount;
    uint8_t *dataBuf;

    m_AACDecInfo->currInstTag = GetBits(NUM_INST_TAG_BITS);
    byteAlign = GetBits(1);
    dataCount = GetBits(8);
    if (dataCount == 255)
        dataCount += GetBits(8);

    if (byteAlign)
        ByteAlignBitstream();

    m_PSInfoBase->dataCount = dataCount;
    dataBuf = m_PSInfoBase->dataBuf;
    while (dataCount--)
        *dataBuf++ = GetBits(8);

    return 0;
}

/***********************************************************************************************************************
 * Function:    DecodeProgramConfigElement
 *
 * Description: decode one PCE
 *
 * Inputs:      none
 *
 * Outputs:     filled-in ProgConfigElement_t struct
 *              updated aac_BitStreamInfo_t struct
 *
 * Return:      0 if successful, error code (< 0) if error
 *
 * Notes:       #define KEEP_PCE_COMMENTS to save the comment field of the PCE
 *                (otherwise we just skip it in the bitstream, to save memory)
 **********************************************************************************************************************/
int32_t AACDecoder::DecodeProgramConfigElement(uint8_t idx) {
    int32_t i;

    m_pce[idx]->elemInstTag = GetBits(4);
    m_pce[idx]->profile = GetBits(2);
    m_pce[idx]->sampRateIdx = GetBits(4);
    m_pce[idx]->numFCE = GetBits(4);
    m_pce[idx]->numSCE = GetBits(4);
    m_pce[idx]->numBCE = GetBits(4);
    m_pce[idx]->numLCE = GetBits(2);
    m_pce[idx]->numADE = GetBits(3);
    m_pce[idx]->numCCE = GetBits(4);

    m_pce[idx]->monoMixdown = GetBits(1) << 4; /* present flag */
    if (m_pce[idx]->monoMixdown)
        m_pce[idx]->monoMixdown |= GetBits(4); /* element number */

    m_pce[idx]->stereoMixdown = GetBits(1) << 4; /* present flag */
    if (m_pce[idx]->stereoMixdown)
        m_pce[idx]->stereoMixdown |= GetBits(4); /* element number */

    m_pce[idx]->matrixMixdown = GetBits(1) << 4; /* present flag */
    if (m_pce[idx]->matrixMixdown) {
        m_pce[idx]->matrixMixdown |= GetBits(2) << 1; /* index */
        m_pce[idx]->matrixMixdown |= GetBits(1);      /* pseudo-surround enable */
    }

    for (i = 0; i < m_pce[idx]->numFCE; i++) {
        m_pce[idx]->fce[i] = GetBits(1) << 4; /* is_cpe flag */
        m_pce[idx]->fce[i] |= GetBits(4);     /* tag select */
    }

    for (i = 0; i < m_pce[idx]->numSCE; i++) {
        m_pce[idx]->sce[i] = GetBits(1) << 4; /* is_cpe flag */
        m_pce[idx]->sce[i] |= GetBits(4);     /* tag select */
    }

    for (i = 0; i < m_pce[idx]->numBCE; i++) {
        m_pce[idx]->bce[i] = GetBits(1) << 4; /* is_cpe flag */
        m_pce[idx]->bce[i] |= GetBits(4);     /* tag select */
    }

    for (i = 0; i < m_pce[idx]->numLCE; i++)
        m_pce[idx]->lce[i] = GetBits(4); /* tag select */

    for (i = 0; i < m_pce[idx]->numADE; i++)
        m_pce[idx]->ade[i] = GetBits(4); /* tag select */

    for (i = 0; i < m_pce[idx]->numCCE; i++) {
        m_pce[idx]->cce[i] = GetBits(1) << 4; /* independent/dependent flag */
        m_pce[idx]->cce[i] |= GetBits(4);     /* tag select */
    }

    ByteAlignBitstream();
    /* eat comment bytes and throw away */
    i = GetBits(8);
    while (i--)
        GetBits(8);

    return 0;
}

/***********************************************************************************************************************
 * Function:    DecodeFillElement
 *
 * Description: decode one fill element
 *
 * Inputs:      none
 *                (14496-3, table 4.4.11)
 *
 * Outputs:     updated element instance tag
 *              unpacked extension payload
 *
 * Return:      0 if successful, -1 if error
 **********************************************************************************************************************/
int32_t AACDecoder::DecodeFillElement() {
    uint32_t fillCount;
    uint8_t *fillBuf;

    fillCount = GetBits(4);
    if (fillCount == 15)
        fillCount += (GetBits(8) - 1);

    m_PSInfoBase->fillCount = fillCount;
    fillBuf = m_PSInfoBase->fillBuf;
    while (fillCount--)
        *fillBuf++ = GetBits(8);

    m_AACDecInfo->currInstTag = -1; /* fill elements don't have instance tag */
    m_AACDecInfo->fillExtType = 0;

#ifdef AAC_ENABLE_SBR
    /* check for SBR
     * aacDecInfo->sbrEnabled is sticky (reset each raw_data_block), so for multichannel
     *    need to verify that all SCE/CPE/ICCE have valid SBR fill element following, and
     *    must upsample by 2 for LFE
     */
    if (m_PSInfoBase->fillCount > 0) {
        m_AACDecInfo->fillExtType = (int32_t)((m_PSInfoBase->fillBuf[0] >> 4) & 0x0f);
        if (m_AACDecInfo->fillExtType == EXT_SBR_DATA || m_AACDecInfo->fillExtType == EXT_SBR_DATA_CRC)
            m_AACDecInfo->sbrEnabled = 1;
    }
#endif

    m_AACDecInfo->fillBuf = m_PSInfoBase->fillBuf;
    m_AACDecInfo->fillCount = m_PSInfoBase->fillCount;

    return 0;
}

/***********************************************************************************************************************
 * Function:    DecodeNextElement
 *
 * Description: decode next syntactic element in AAC frame
 *
 * Inputs:      double pointer to buffer containing next element
 *              pointer to bit offset
 *              pointer to number of valid bits remaining in buf
 *
 * Outputs:     type of element decoded (aacDecInfo->currBlockID)
 *              type of element decoded last time (aacDecInfo->prevBlockID)
 *              updated aacDecInfo state, depending on which element was decoded
 *              updated buffer pointer
 *              updated bit offset
 *              updated number of available bits
 *
 * Return:      0 if successful, error code (< 0) if error
 **********************************************************************************************************************/
int32_t AACDecoder::DecodeNextElement(uint8_t **buf, int32_t *bitOffset, int32_t *bitsAvail) {
    int32_t err, bitsUsed;

    /* init bitstream reader */
    SetBitstreamPointer((*bitsAvail + 7) >> 3, *buf);
    GetBits(*bitOffset);

    m_AACDecInfo->prevBlockID = m_AACDecInfo->currBlockID;
    m_AACDecInfo->currBlockID = GetBits(NUM_SYN_ID_BITS);

    /* set defaults (could be overwritten by DecodeXXXElement(), depending on currBlockID) */
    m_PSInfoBase->commonWin = 0;

    err = 0;
    switch (m_AACDecInfo->currBlockID) {
    case AAC_ID_SCE:
        err = DecodeSingleChannelElement();
        break;
    case AAC_ID_CPE:
        err = DecodeChannelPairElement();
        break;
    case AAC_ID_CCE:
        break;
    case AAC_ID_LFE:
        err = DecodeLFEChannelElement();
        break;
    case AAC_ID_DSE:
        err = DecodeDataStreamElement();
        break;
    case AAC_ID_PCE:
        err = DecodeProgramConfigElement(0);
        break;
    case AAC_ID_FIL:
        err = DecodeFillElement();
        break;
    case AAC_ID_END:
        break;
    }
    if (err)
        return ERR_AAC_SYNTAX_ELEMENT;

    /* update bitstream reader */
    bitsUsed = CalcBitsUsed(*buf, *bitOffset);
    *buf += (bitsUsed + *bitOffset) >> 3;
    *bitOffset = (bitsUsed + *bitOffset) & 0x07;
    *bitsAvail -= bitsUsed;

    if (*bitsAvail < 0)
        return ERR_AAC_INDATA_UNDERFLOW;

    return ERR_AAC_NONE;
}

/***********************************************************************************************************************
 * Function:    PreMultiply
 *
 * Description: pre-twiddle stage of DCT4
 *
 * Inputs:      table index (for transform size)
 *              buffer of nmdct samples
 *
 * Outputs:     processed samples in same buffer
 *
 * Return:      none
 *
 * Notes:       minimum 1 GB in, 2 GB out, gains 5 (short) or 8 (long) frac bits
 *              i.e. gains 2-7= -5 int32_t bits (short) or 2-10 = -8 int32_t bits (long)
 *              normalization by -1/N is rolled into tables here (see trigtabs.c)
 *              uses 3-mul, 3-add butterflies instead of 4-mul, 2-add
 **********************************************************************************************************************/
void AACDecoder::PreMultiply(int32_t tabidx, int32_t *zbuf1) {
    int32_t i, nmdct, ar1, ai1, ar2, ai2, z1, z2;
    int32_t t, cms2, cps2a, sin2a, cps2b, sin2b;
    int32_t *zbuf2;
    const uint32_t *csptr;

    nmdct = nmdctTab[tabidx];
    zbuf2 = zbuf1 + nmdct - 1;
    csptr = cos4sin4tab + cos4sin4tabOffset[tabidx];

    /* whole thing should fit in registers - verify that compiler does this */
    for (i = nmdct >> 2; i != 0; i--) {
        /* cps2 = (cos+sin), sin2 = sin, cms2 = (cos-sin) */
        cps2a = *csptr++;
        sin2a = *csptr++;
        cps2b = *csptr++;
        sin2b = *csptr++;

        ar1 = *(zbuf1 + 0);
        ai2 = *(zbuf1 + 1);
        ai1 = *(zbuf2 + 0);
        ar2 = *(zbuf2 - 1);

        /* gain 2 ints bit from MULSHIFT32 by Q30, but drop 7 or 10 int32_t bits from table scaling of 1/M
         * max per-sample gain (ignoring implicit scaling) = MAX(sin(angle)+cos(angle)) = 1.414
         * i.e. gain 1 GB since worst case is sin(angle) = cos(angle) = 0.707 (Q30), gain 2 from
         *   extra sign bits, and eat one in adding
         */
        t = MULSHIFT32(sin2a, ar1 + ai1);
        z2 = MULSHIFT32(cps2a, ai1) - t;
        cms2 = cps2a - 2 * sin2a;
        z1 = MULSHIFT32(cms2, ar1) + t;
        *zbuf1++ = z1; /* cos*ar1 + sin*ai1 */
        *zbuf1++ = z2; /* cos*ai1 - sin*ar1 */

        t = MULSHIFT32(sin2b, ar2 + ai2);
        z2 = MULSHIFT32(cps2b, ai2) - t;
        cms2 = cps2b - 2 * sin2b;
        z1 = MULSHIFT32(cms2, ar2) + t;
        *zbuf2-- = z2; /* cos*ai2 - sin*ar2 */
        *zbuf2-- = z1; /* cos*ar2 + sin*ai2 */
    }
}

/***********************************************************************************************************************
 * Function:    PostMultiply
 *
 * Description: post-twiddle stage of DCT4
 *
 * Inputs:      table index (for transform size)
 *              buffer of nmdct samples
 *
 * Outputs:     processed samples in same buffer
 *
 * Return:      none
 *
 * Notes:       minimum 1 GB in, 2 GB out - gains 2 int32_t bits
 *              uses 3-mul, 3-add butterflies instead of 4-mul, 2-add
 **********************************************************************************************************************/
void AACDecoder::PostMultiply(int32_t tabidx, int32_t *fft1) {
    int32_t i, nmdct, ar1, ai1, ar2, ai2, skipFactor;
    int32_t t, cms2, cps2, sin2;
    int32_t *fft2;
    const int32_t *csptr;

    nmdct = nmdctTab[tabidx];
    csptr = cos1sin1tab;
    skipFactor = postSkip[tabidx];
    fft2 = fft1 + nmdct - 1;

    /* load coeffs for first pass
     * cps2 = (cos+sin), sin2 = sin, cms2 = (cos-sin)
     */
    cps2 = *csptr++;
    sin2 = *csptr;
    csptr += skipFactor;
    cms2 = cps2 - 2 * sin2;

    for (i = nmdct >> 2; i != 0; i--) {
        ar1 = *(fft1 + 0);
        ai1 = *(fft1 + 1);
        ar2 = *(fft2 - 1);
        ai2 = *(fft2 + 0);

        /* gain 2 ints bit from MULSHIFT32 by Q30
         * max per-sample gain = MAX(sin(angle)+cos(angle)) = 1.414
         * i.e. gain 1 GB since worst case is sin(angle) = cos(angle) = 0.707 (Q30), gain 2 from
         *   extra sign bits, and eat one in adding
         */
        t = MULSHIFT32(sin2, ar1 + ai1);
        *fft2-- = t - MULSHIFT32(cps2, ai1); /* sin*ar1 - cos*ai1 */
        *fft1++ = t + MULSHIFT32(cms2, ar1); /* cos*ar1 + sin*ai1 */
        cps2 = *csptr++;
        sin2 = *csptr;
        csptr += skipFactor;

        ai2 = -ai2;
        t = MULSHIFT32(sin2, ar2 + ai2);
        *fft2-- = t - MULSHIFT32(cps2, ai2); /* sin*ar1 - cos*ai1 */
        cms2 = cps2 - 2 * sin2;
        *fft1++ = t + MULSHIFT32(cms2, ar2); /* cos*ar1 + sin*ai1 */
    }
}

/***********************************************************************************************************************
 * Function:    PreMultiplyRescale
 *
 * Description: pre-twiddle stage of DCT4, with rescaling for extra guard bits
 *
 * Inputs:      table index (for transform size)
 *              buffer of nmdct samples
 *              number of guard bits to add to input before processing
 *
 * Outputs:     processed samples in same buffer
 *
 * Return:      none
 *
 * Notes:       see notes on PreMultiply(), above
 **********************************************************************************************************************/
void AACDecoder::PreMultiplyRescale(int32_t tabidx, int32_t *zbuf1, int32_t es) {
    int32_t i, nmdct, ar1, ai1, ar2, ai2, z1, z2;
    int32_t t, cms2, cps2a, sin2a, cps2b, sin2b;
    int32_t *zbuf2;
    const uint32_t *csptr;

    nmdct = nmdctTab[tabidx];
    zbuf2 = zbuf1 + nmdct - 1;
    csptr = cos4sin4tab + cos4sin4tabOffset[tabidx];

    /* whole thing should fit in registers - verify that compiler does this */
    for (i = nmdct >> 2; i != 0; i--) {
        /* cps2 = (cos+sin), sin2 = sin, cms2 = (cos-sin) */
        cps2a = *csptr++;
        sin2a = *csptr++;
        cps2b = *csptr++;
        sin2b = *csptr++;

        ar1 = *(zbuf1 + 0) >> es;
        ai1 = *(zbuf2 + 0) >> es;
        ai2 = *(zbuf1 + 1) >> es;

        t = MULSHIFT32(sin2a, ar1 + ai1);
        z2 = MULSHIFT32(cps2a, ai1) - t;
        cms2 = cps2a - 2 * sin2a;
        z1 = MULSHIFT32(cms2, ar1) + t;
        *zbuf1++ = z1;
        *zbuf1++ = z2;

        ar2 = *(zbuf2 - 1) >> es; /* do here to free up register used for es */

        t = MULSHIFT32(sin2b, ar2 + ai2);
        z2 = MULSHIFT32(cps2b, ai2) - t;
        cms2 = cps2b - 2 * sin2b;
        z1 = MULSHIFT32(cms2, ar2) + t;
        *zbuf2-- = z2;
        *zbuf2-- = z1;
    }
}

/***********************************************************************************************************************
 * Function:    PostMultiplyRescale
 *
 * Description: post-twiddle stage of DCT4, with rescaling for extra guard bits
 *
 * Inputs:      table index (for transform size)
 *              buffer of nmdct samples
 *              number of guard bits to remove from output
 *
 * Outputs:     processed samples in same buffer
 *
 * Return:      none
 *
 * Notes:       clips output to [-2^30, 2^30 - 1], guaranteeing at least 1 guard bit
 *              see notes on PostMultiply(), above
 **********************************************************************************************************************/
void AACDecoder::PostMultiplyRescale(int32_t tabidx, int32_t *fft1, int32_t es) {
    int32_t i, nmdct, ar1, ai1, ar2, ai2, skipFactor, z;
    int32_t t, cs2, sin2;
    int32_t *fft2;
    const int32_t *csptr;

    nmdct = nmdctTab[tabidx];
    csptr = cos1sin1tab;
    skipFactor = postSkip[tabidx];
    fft2 = fft1 + nmdct - 1;

    /* load coeffs for first pass
     * cps2 = (cos+sin), sin2 = sin, cms2 = (cos-sin)
     */
    cs2 = *csptr++;
    sin2 = *csptr;
    csptr += skipFactor;

    for (i = nmdct >> 2; i != 0; i--) {
        ar1 = *(fft1 + 0);
        ai1 = *(fft1 + 1);
        ai2 = *(fft2 + 0);

        t = MULSHIFT32(sin2, ar1 + ai1);
        z = t - MULSHIFT32(cs2, ai1);
        {
            int32_t sign = (z) >> 31;
            if (sign != (z) >> (30 - (es))) {
                (z) = sign ^ (0x3fffffff);
            } else {
                (z) = (z) << (es);
            }
        }
        *fft2-- = z;
        cs2 -= 2 * sin2;
        z = t + MULSHIFT32(cs2, ar1);
        {
            int32_t sign = (z) >> 31;
            if (sign != (z) >> (30 - (es))) {
                (z) = sign ^ (0x3fffffff);
            } else {
                (z) = (z) << (es);
            }
        }
        *fft1++ = z;

        cs2 = *csptr++;
        sin2 = *csptr;
        csptr += skipFactor;

        ar2 = *fft2;
        ai2 = -ai2;
        t = MULSHIFT32(sin2, ar2 + ai2);
        z = t - MULSHIFT32(cs2, ai2);
        {
            int32_t sign = (z) >> 31;
            if (sign != (z) >> (30 - (es))) {
                (z) = sign ^ (0x3fffffff);
            } else {
                (z) = (z) << (es);
            }
        }
        *fft2-- = z;
        cs2 -= 2 * sin2;
        z = t + MULSHIFT32(cs2, ar2);
        {
            int32_t sign = (z) >> 31;
            if (sign != (z) >> (30 - (es))) {
                (z) = sign ^ (0x3fffffff);
            } else {
                (z) = (z) << (es);
            }
        }
        *fft1++ = z;
        cs2 += 2 * sin2;
    }
}

/***********************************************************************************************************************
 * Function:    DCT4
 *
 * Description: type-IV DCT
 *
 * Inputs:      table index (for transform size)
 *              buffer of nmdct samples
 *              number of guard bits in the input buffer
 *
 * Outputs:     processed samples in same buffer
 *
 * Return:      none
 *
 * Notes:       operates in-place
 *              if number of guard bits in input is < GBITS_IN_DCT4, the input is
 *                scaled (>>) before the DCT4 and rescaled (<<, with clipping) after
 *                the DCT4 (rare)
 *              the output has FBITS_LOST_DCT4 fewer fraction bits than the input
 *              the output will always have at least 1 guard bit (GBITS_IN_DCT4 >= 4)
 *              int32_t bits gained per stage (PreMul + FFT + PostMul)
 *                 short blocks = (-5 + 4 + 2) = 1 total
 *                 long blocks =  (-8 + 7 + 2) = 1 total
 **********************************************************************************************************************/
void AACDecoder::DCT4(int32_t tabidx, int32_t *coef, int32_t gb) {
    int32_t es;

    /* fast in-place DCT-IV - adds guard bits if necessary */
    if (gb < GBITS_IN_DCT4) {
        es = GBITS_IN_DCT4 - gb;
        PreMultiplyRescale(tabidx, coef, es);
        R4FFT(tabidx, coef);
        PostMultiplyRescale(tabidx, coef, es);
    } else {
        PreMultiply(tabidx, coef);
        R4FFT(tabidx, coef);
        PostMultiply(tabidx, coef);
    }
}

/***********************************************************************************************************************
 * Function:    BitReverse
 *
 * Description: Ken's fast in-place bit reverse, using super-small table
 *
 * Inputs:      buffer of samples
 *              table index (for transform size)
 *
 * Outputs:     bit-reversed samples in same buffer
 *
 * Return:      none
 **********************************************************************************************************************/
void AACDecoder::BitReverse(int32_t *inout, int32_t tabidx) {
    int32_t *part0, *part1;
    int32_t a, b, t;
    const uint8_t *tab = bitrevtab + bitrevtabOffset[tabidx];
    int32_t nbits = nfftlog2Tab[tabidx];

    part0 = inout;
    part1 = inout + (1 << nbits);

    while ((a = pgm_read_byte(tab++)) != 0) {
        b = pgm_read_byte(tab++);

        t = part0[4 * a + 0];
        part0[4 * a + 0] = part0[4 * b + 0];
        part0[4 * b + 0] = t; /* 0xxx0 <-> 0yyy0 */
        t = part0[4 * a + 1];
        part0[4 * a + 1] = part0[4 * b + 1];
        part0[4 * b + 1] = t;

        t = part0[4 * a + 2];
        part0[4 * a + 2] = part1[4 * b + 0];
        part1[4 * b + 0] = t; /* 0xxx0 <-> 0yyy0 */
        t = part0[4 * a + 3];
        part0[4 * a + 3] = part1[4 * b + 1];
        part1[4 * b + 1] = t;

        t = part1[4 * a + 0];
        part1[4 * a + 0] = part0[4 * b + 2];
        part0[4 * b + 2] = t; /* 1xxx0 <-> 0yyy1 */
        t = part1[4 * a + 1];
        part1[4 * a + 1] = part0[4 * b + 3];
        part0[4 * b + 3] = t;

        t = part1[4 * a + 2];
        part1[4 * a + 2] = part1[4 * b + 2];
        part1[4 * b + 2] = t; /* 1xxx1 <-> 1yyy1 */
        t = part1[4 * a + 3];
        part1[4 * a + 3] = part1[4 * b + 3];
        part1[4 * b + 3] = t;
    }

    do {
        t = part0[4 * a + 2];
        part0[4 * a + 2] = part1[4 * a + 0];
        part1[4 * a + 0] = t; /* 0xxx1 <-> 1xxx0 */
        t = part0[4 * a + 3];
        part0[4 * a + 3] = part1[4 * a + 1];
        part1[4 * a + 1] = t;
    } while ((a = pgm_read_byte(tab++)) != 0);
}

/***********************************************************************************************************************
 * Function:    R4FirstPass
 *
 * Description: radix-4 trivial pass for decimation-in-time FFT
 *
 * Inputs:      buffer of (bit-reversed) samples
 *              number of R4 butterflies per group (i.e. nfft / 4)
 *
 * Outputs:     processed samples in same buffer
 *
 * Return:      none
 *
 * Notes:       assumes 2 guard bits, gains no integer bits,
 *                guard bits out = guard bits in - 2
 **********************************************************************************************************************/
void AACDecoder::R4FirstPass(int32_t *x, int32_t bg) {
    int32_t ar, ai, br, bi, cr, ci, dr, di;

    for (; bg != 0; bg--) {
        ar = x[0] + x[2];
        br = x[0] - x[2];
        ai = x[1] + x[3];
        bi = x[1] - x[3];
        cr = x[4] + x[6];
        dr = x[4] - x[6];
        ci = x[5] + x[7];
        di = x[5] - x[7];

        /* max per-sample gain = 4.0 (adding 4 inputs together) */
        x[0] = ar + cr;
        x[4] = ar - cr;
        x[1] = ai + ci;
        x[5] = ai - ci;
        x[2] = br + di;
        x[6] = br - di;
        x[3] = bi - dr;
        x[7] = bi + dr;

        x += 8;
    }
}

/***********************************************************************************************************************
 * Function:    R8FirstPass
 *
 * Description: radix-8 trivial pass for decimation-in-time FFT
 *
 * Inputs:      buffer of (bit-reversed) samples
 *              number of R8 butterflies per group (i.e. nfft / 8)
 *
 * Outputs:     processed samples in same buffer
 *
 * Return:      none
 *
 * Notes:       assumes 3 guard bits, gains 1 integer bit
 *              guard bits out = guard bits in - 3 (if inputs are full scale)
 *                or guard bits in - 2 (if inputs bounded to +/- sqrt(2)/2)
 *              see scaling comments in code
 **********************************************************************************************************************/
void AACDecoder::R8FirstPass(int32_t *x, int32_t bg) {
    int32_t ar, ai, br, bi, cr, ci, dr, di;
    int32_t sr, si, tr, ti, ur, ui, vr, vi;
    int32_t wr, wi, xr, xi, yr, yi, zr, zi;

    for (; bg != 0; bg--) {
        ar = x[0] + x[2];
        br = x[0] - x[2];
        ai = x[1] + x[3];
        bi = x[1] - x[3];
        cr = x[4] + x[6];
        dr = x[4] - x[6];
        ci = x[5] + x[7];
        di = x[5] - x[7];

        sr = ar + cr;
        ur = ar - cr;
        si = ai + ci;
        ui = ai - ci;
        tr = br - di;
        vr = br + di;
        ti = bi + dr;
        vi = bi - dr;

        ar = x[8] + x[10];
        br = x[8] - x[10];
        ai = x[9] + x[11];
        bi = x[9] - x[11];
        cr = x[12] + x[14];
        dr = x[12] - x[14];
        ci = x[13] + x[15];
        di = x[13] - x[15];

        /* max gain of wr/wi/yr/yi vs input = 2
         *  (sum of 4 samples >> 1)
         */
        wr = (ar + cr) >> 1;
        yr = (ar - cr) >> 1;
        wi = (ai + ci) >> 1;
        yi = (ai - ci) >> 1;

        /* max gain of output vs input = 4
         *  (sum of 4 samples >> 1 + sum of 4 samples >> 1)
         */
        x[0] = (sr >> 1) + wr;
        x[8] = (sr >> 1) - wr;
        x[1] = (si >> 1) + wi;
        x[9] = (si >> 1) - wi;
        x[4] = (ur >> 1) + yi;
        x[12] = (ur >> 1) - yi;
        x[5] = (ui >> 1) - yr;
        x[13] = (ui >> 1) + yr;

        ar = br - di;
        cr = br + di;
        ai = bi + dr;
        ci = bi - dr;

        /* max gain of xr/xi/zr/zi vs input = 4*sqrt(2)/2 = 2*sqrt(2)
         *  (sum of 8 samples, multiply by sqrt(2)/2, implicit >> 1 from Q31)
         */
        xr = MULSHIFT32(SQRTHALF, ar - ai);
        xi = MULSHIFT32(SQRTHALF, ar + ai);
        zr = MULSHIFT32(SQRTHALF, cr - ci);
        zi = MULSHIFT32(SQRTHALF, cr + ci);

        /* max gain of output vs input = (2 + 2*sqrt(2) ~= 4.83)
         *  (sum of 4 samples >> 1, plus xr/xi/zr/zi with gain of 2*sqrt(2))
         * in absolute terms, we have max gain of appx 9.656 (4 + 0.707*8)
         *  but we also gain 1 int32_t bit (from MULSHIFT32 or from explicit >> 1)
         */
        x[6] = (tr >> 1) - xr;
        x[14] = (tr >> 1) + xr;
        x[7] = (ti >> 1) - xi;
        x[15] = (ti >> 1) + xi;
        x[2] = (vr >> 1) + zi;
        x[10] = (vr >> 1) - zi;
        x[3] = (vi >> 1) - zr;
        x[11] = (vi >> 1) + zr;

        x += 16;
    }
}

/***********************************************************************************************************************
 * Function:    R4Core
 *
 * Description: radix-4 pass for decimation-in-time FFT
 *
 * Inputs:      buffer of samples
 *              number of R4 butterflies per group
 *              number of R4 groups per pass
 *              pointer to twiddle factors tables
 *
 * Outputs:     processed samples in same buffer
 *
 * Return:      none
 *
 * Notes:       gain 2 integer bits per pass (see scaling comments in code)
 *              min 1 GB in
 *              gbOut = gbIn - 1 (short block) or gbIn - 2 (long block)
 *              uses 3-mul, 3-add butterflies instead of 4-mul, 2-add
 **********************************************************************************************************************/
void AACDecoder::R4Core(int32_t *x, int32_t bg, int32_t gp, int32_t *wtab) {
    int32_t ar, ai, br, bi, cr, ci, dr, di, tr, ti;
    int32_t wd, ws, wi;
    int32_t i, j, step;
    int32_t *xptr, *wptr;

    for (; bg != 0; gp <<= 2, bg >>= 2) {
        step = 2 * gp;
        xptr = x;

        /* max per-sample gain, per group < 1 + 3*sqrt(2) ~= 5.25 if inputs x are full-scale
         * do 3 groups for long block, 2 groups for short block (gain 2 int32_t bits per group)
         *
         * very conservative scaling:
         *   group 1: max gain = 5.25,           int32_t bits gained = 2, gb used = 1 (2^3 = 8)
         *   group 2: max gain = 5.25^2 = 27.6,  int32_t bits gained = 4, gb used = 1 (2^5 = 32)
         *   group 3: max gain = 5.25^3 = 144.7, int32_t bits gained = 6, gb used = 2 (2^8 = 256)
         */
        for (i = bg; i != 0; i--) {
            wptr = wtab;

            for (j = gp; j != 0; j--) {
                ar = xptr[0];
                ai = xptr[1];
                xptr += step;

                /* gain 2 int32_t bits for br/bi, cr/ci, dr/di (MULSHIFT32 by Q30)
                 * gain 1 net GB
                 */
                ws = wptr[0];
                wi = wptr[1];
                br = xptr[0];
                bi = xptr[1];
                wd = ws + 2 * wi;
                tr = MULSHIFT32(wi, br + bi);
                br = MULSHIFT32(wd, br) - tr; /* cos*br + sin*bi */
                bi = MULSHIFT32(ws, bi) + tr; /* cos*bi - sin*br */
                xptr += step;

                ws = wptr[2];
                wi = wptr[3];
                cr = xptr[0];
                ci = xptr[1];
                wd = ws + 2 * wi;
                tr = MULSHIFT32(wi, cr + ci);
                cr = MULSHIFT32(wd, cr) - tr;
                ci = MULSHIFT32(ws, ci) + tr;
                xptr += step;

                ws = wptr[4];
                wi = wptr[5];
                dr = xptr[0];
                di = xptr[1];
                wd = ws + 2 * wi;
                tr = MULSHIFT32(wi, dr + di);
                dr = MULSHIFT32(wd, dr) - tr;
                di = MULSHIFT32(ws, di) + tr;
                wptr += 6;

                tr = ar;
                ti = ai;
                ar = (tr >> 2) - br;
                ai = (ti >> 2) - bi;
                br = (tr >> 2) + br;
                bi = (ti >> 2) + bi;

                tr = cr;
                ti = ci;
                cr = tr + dr;
                ci = di - ti;
                dr = tr - dr;
                di = di + ti;

                xptr[0] = ar + ci;
                xptr[1] = ai + dr;
                xptr -= step;
                xptr[0] = br - cr;
                xptr[1] = bi - di;
                xptr -= step;
                xptr[0] = ar - ci;
                xptr[1] = ai - dr;
                xptr -= step;
                xptr[0] = br + cr;
                xptr[1] = bi + di;
                xptr += 2;
            }
            xptr += 3 * step;
        }
        wtab += 3 * step;
    }
}

/***********************************************************************************************************************
 * Function:    R4FFT
 *
 * Description: Ken's very fast in-place radix-4 decimation-in-time FFT
 *
 * Inputs:      table index (for transform size)
 *              buffer of samples (non bit-reversed)
 *
 * Outputs:     processed samples in same buffer
 *
 * Return:      none
 *
 * Notes:       assumes 5 guard bits in for nfft <= 512
 *              gbOut = gbIn - 4 (assuming input is from PreMultiply)
 *              gains log2(nfft) - 2 int32_t bits total
 *                so gain 7 int32_t bits (LONG), 4 int32_t bits (SHORT)
 **********************************************************************************************************************/
void AACDecoder::R4FFT(int32_t tabidx, int32_t *x) {
    int32_t order = nfftlog2Tab[tabidx];
    int32_t nfft = nfftTab[tabidx];

    /* decimation in time */
    BitReverse(x, tabidx);

    if (order & 0x1) {
        /* long block: order = 9, nfft = 512 */
        R8FirstPass(x, nfft >> 3);                      /* gain 1 int32_t bit,  lose 2 GB */
        R4Core(x, nfft >> 5, 8, (int32_t *)twidTabOdd); /* gain 6 int32_t bits, lose 2 GB */
    } else {
        /* short block: order = 6, nfft = 64 */
        R4FirstPass(x, nfft >> 2);                       /* gain 0 int32_t bits, lose 2 GB */
        R4Core(x, nfft >> 4, 4, (int32_t *)twidTabEven); /* gain 4 int32_t bits, lose 1 GB */
    }
}

/***********************************************************************************************************************
 * Function:    UnpackZeros
 *
 * Description: fill a section of coefficients with zeros
 *
 * Inputs:      number of coefficients
 *
 * Outputs:     nVals zeros, starting at coef
 *
 * Return:      none
 *
 * Notes:       assumes nVals is always a multiple of 4 because all scalefactor bands
 *                are a multiple of 4 coefficients long
 **********************************************************************************************************************/
/*
 void AACDecoder::UnpackZeros(int32_t nVals, int32_t *coef) {
    while (nVals > 0) {
        *coef++ = 0;
        *coef++ = 0;
        *coef++ = 0;
        *coef++ = 0;
        nVals -= 4;
    }
}*/
void AACDecoder::UnpackZeros(int32_t nVals, int32_t *coef) {
    memset(coef, 0, nVals * sizeof(int32_t));
};
/***********************************************************************************************************************
 * Function:    UnpackQuads
 *
 * Description: decode a section of 4-way vector Huffman coded coefficients
 *
 * Inputs       index of Huffman codebook
 *              number of coefficients
 *
 * Outputs:     nVals coefficients, starting at coef
 *
 * Return:      none
 *
 * Notes:       assumes nVals is always a multiple of 4 because all scalefactor bands
 *                are a multiple of 4 coefficients long
 **********************************************************************************************************************/
void AACDecoder::UnpackQuads(int32_t cb, int32_t nVals, int32_t *coef) {
    int32_t w, x, y, z, maxBits, nCodeBits, nSignBits, val;
    uint32_t bitBuf;

    maxBits = huffTabSpecInfo[cb - HUFFTAB_SPEC_OFFSET].maxBits + 4;
    while (nVals > 0) {
        /* decode quad */
        bitBuf = GetBitsNoAdvance(maxBits) << (32 - maxBits);
        nCodeBits = DecodeHuffmanScalar(huffTabSpec, &huffTabSpecInfo[cb - HUFFTAB_SPEC_OFFSET], bitBuf, &val);

        w = (((int32_t)(val) << 20) >> 29); /* bits 11-9, sign-extend */
        x = (((int32_t)(val) << 23) >> 29); /* bits  8-6, sign-extend */
        y = (((int32_t)(val) << 26) >> 29); /* bits  5-3, sign-extend */
        z = (((int32_t)(val) << 29) >> 29); /* bits  2-0, sign-extend */

        bitBuf <<= nCodeBits;
        nSignBits = (int32_t)(((uint32_t)(val) << 17) >> 29); /* bits 14-12, unsigned */

        AdvanceBitstream(nCodeBits + nSignBits);
        if (nSignBits) {
            if (w) {
                w ^= ((int32_t)bitBuf >> 31);
                w -= ((int32_t)bitBuf >> 31);
                bitBuf <<= 1;
            }
            if (x) {
                x ^= ((int32_t)bitBuf >> 31);
                x -= ((int32_t)bitBuf >> 31);
                bitBuf <<= 1;
            }
            if (y) {
                y ^= ((int32_t)bitBuf >> 31);
                y -= ((int32_t)bitBuf >> 31);
                bitBuf <<= 1;
            }
            if (z) {
                z ^= ((int32_t)bitBuf >> 31);
                z -= ((int32_t)bitBuf >> 31);
                bitBuf <<= 1;
            }
        }
        *coef++ = w;
        *coef++ = x;
        *coef++ = y;
        *coef++ = z;
        nVals -= 4;
    }
}

/***********************************************************************************************************************
 * Function:    UnpackPairsNoEsc
 *
 * Description: decode a section of 2-way vector Huffman coded coefficients,
 *                using non-esc tables (5 through 10)
 *
 * Inputs       index of Huffman codebook (must not be the escape codebook)
 *              number of coefficients
 *
 * Outputs:     nVals coefficients, starting at coef
 *
 * Return:      none
 *
 * Notes:       assumes nVals is always a multiple of 2 because all scalefactor bands
 *                are a multiple of 4 coefficients long
 **********************************************************************************************************************/
void AACDecoder::UnpackPairsNoEsc(int32_t cb, int32_t nVals, int32_t *coef) {
    int32_t y, z, maxBits, nCodeBits, nSignBits, val;
    uint32_t bitBuf;

    maxBits = huffTabSpecInfo[cb - HUFFTAB_SPEC_OFFSET].maxBits + 2;
    while (nVals > 0) {
        /* decode pair */
        bitBuf = GetBitsNoAdvance(maxBits) << (32 - maxBits);
        nCodeBits = DecodeHuffmanScalar(huffTabSpec, &huffTabSpecInfo[cb - HUFFTAB_SPEC_OFFSET], bitBuf, &val);

        y = (((int32_t)(val) << 22) >> 27); /* bits  9-5, sign-extend */
        z = (((int32_t)(val) << 27) >> 27); /* bits  4-0, sign-extend */

        bitBuf <<= nCodeBits;
        nSignBits = (((uint32_t)(val) << 20) >> 30); /* bits 11-10, unsigned */
        AdvanceBitstream(nCodeBits + nSignBits);
        if (nSignBits) {
            if (y) {
                y ^= ((int32_t)bitBuf >> 31);
                y -= ((int32_t)bitBuf >> 31);
                bitBuf <<= 1;
            }
            if (z) {
                z ^= ((int32_t)bitBuf >> 31);
                z -= ((int32_t)bitBuf >> 31);
                bitBuf <<= 1;
            }
        }
        *coef++ = y;
        *coef++ = z;
        nVals -= 2;
    }
}

/***********************************************************************************************************************
 * Function:    UnpackPairsEsc
 *
 * Description: decode a section of 2-way vector Huffman coded coefficients,
 *                using esc table (11)
 *
 * Inputs       index of Huffman codebook (must be the escape codebook)
 *              number of coefficients
 *
 * Outputs:     nVals coefficients, starting at coef
 *
 * Return:      none
 *
 * Notes:       assumes nVals is always a multiple of 2 because all scalefactor bands
 *                are a multiple of 4 coefficients long
 **********************************************************************************************************************/
void AACDecoder::UnpackPairsEsc(int32_t cb, int32_t nVals, int32_t *coef) {
    int32_t y, z, maxBits, nCodeBits, nSignBits, n, val;
    uint32_t bitBuf;

    maxBits = huffTabSpecInfo[cb - HUFFTAB_SPEC_OFFSET].maxBits + 2;
    while (nVals > 0) {
        /* decode pair with escape value */
        bitBuf = GetBitsNoAdvance(maxBits) << (32 - maxBits);
        nCodeBits = DecodeHuffmanScalar(huffTabSpec, &huffTabSpecInfo[cb - HUFFTAB_SPEC_OFFSET], bitBuf, &val);

        y = (((int32_t)(val) << 20) >> 26); /* bits 11-6, sign-extend */
        z = (((int32_t)(val) << 26) >> 26); /* bits  5-0, sign-extend */

        bitBuf <<= nCodeBits;
        nSignBits = (((uint32_t)(val) << 18) >> 30); /* bits 13-12, unsigned */
        AdvanceBitstream(nCodeBits + nSignBits);

        if (y == 16) {
            n = 4;
            while (GetBits(1) == 1)
                n++;
            y = (1 << n) + GetBits(n);
        }
        if (z == 16) {
            n = 4;
            while (GetBits(1) == 1)
                n++;
            z = (1 << n) + GetBits(n);
        }

        if (nSignBits) {
            if (y) {
                y ^= ((int32_t)bitBuf >> 31);
                y -= ((int32_t)bitBuf >> 31);
                bitBuf <<= 1;
            }
            if (z) {
                z ^= ((int32_t)bitBuf >> 31);
                z -= ((int32_t)bitBuf >> 31);
                bitBuf <<= 1;
            }
        }

        *coef++ = y;
        *coef++ = z;
        nVals -= 2;
    }
}

/***********************************************************************************************************************
 * Function:    DecodeSpectrumLong
 *
 * Description: decode transform coefficients for frame with one long block
 *
 * Inputs:      index of current channel
 *
 * Outputs:     decoded, quantized coefficients for this channel
 *
 * Return:      none
 *
 * Notes:       adds in pulse data if present
 *              fills coefficient buffer with zeros in any region not coded with
 *                codebook in range [1, 11] (including sfb's above sfbMax)
 **********************************************************************************************************************/
void AACDecoder::DecodeSpectrumLong(int32_t ch) {
    int32_t i, sfb, cb, nVals, offset;
    const uint16_t *sfbTab;
    uint8_t *sfbCodeBook;
    int32_t *coef;
    ICSInfo_t *icsInfo;

    coef = m_PSInfoBase->coef[ch];
    icsInfo = (ch == 1 && m_PSInfoBase->commonWin == 1) ? &(m_PSInfoBase->icsInfo[0]) : &(m_PSInfoBase->icsInfo[ch]);

    /* decode long block */
    sfbTab = sfBandTabLong + sfBandTabLongOffset[m_PSInfoBase->sampRateIdx];
    sfbCodeBook = m_PSInfoBase->sfbCodeBook[ch];
    for (sfb = 0; sfb < icsInfo->maxSFB; sfb++) {
        cb = *sfbCodeBook++;
        nVals = sfbTab[sfb + 1] - sfbTab[sfb];

        if (cb == 0)
            UnpackZeros(nVals, coef);
        else if (cb <= 4)
            UnpackQuads(cb, nVals, coef);
        else if (cb <= 10)
            UnpackPairsNoEsc(cb, nVals, coef);
        else if (cb == 11)
            UnpackPairsEsc(cb, nVals, coef);
        else
            UnpackZeros(nVals, coef);

        coef += nVals;
    }

    /* fill with zeros above maxSFB */
    nVals = NSAMPS_LONG - sfbTab[sfb];
    UnpackZeros(nVals, coef);

    /* add pulse data, if present */
    if (m_pulseInfo[ch].pulseDataPresent) {
        coef = m_PSInfoBase->coef[ch];
        offset = sfbTab[m_pulseInfo[ch].startSFB];
        for (i = 0; i < m_pulseInfo[ch].numPulse; i++) {
            offset += m_pulseInfo[ch].offset[i];
            if (coef[offset] > 0)
                coef[offset] += m_pulseInfo[ch].amp[i];
            else
                coef[offset] -= m_pulseInfo[ch].amp[i];
        }
        ASSERT(offset < NSAMPS_LONG);
    }
}

/***********************************************************************************************************************
 * Function:    DecodeSpectrumShort
 *
 * Description: decode transform coefficients for frame with eight short blocks
 *
 * Inputs:      index of current channel
 *
 * Outputs:     decoded, quantized coefficients for this channel
 *
 * Return:      none
 *
 * Notes:       fills coefficient buffer with zeros in any region not coded with
 *                codebook in range [1, 11] (including sfb's above sfbMax)
 *              deinterleaves window groups into 8 windows
 **********************************************************************************************************************/
void AACDecoder::DecodeSpectrumShort(int32_t ch) {
    int32_t gp, cb, nVals = 0, win, offset, sfb;
    const uint16_t *sfbTab;
    uint8_t *sfbCodeBook;
    int32_t *coef;
    ICSInfo_t *icsInfo;

    coef = m_PSInfoBase->coef[ch];
    icsInfo = (ch == 1 && m_PSInfoBase->commonWin == 1) ? &(m_PSInfoBase->icsInfo[0]) : &(m_PSInfoBase->icsInfo[ch]);

    /* decode short blocks, deinterleaving in-place */
    sfbTab = sfBandTabShort + sfBandTabShortOffset[m_PSInfoBase->sampRateIdx];
    sfbCodeBook = m_PSInfoBase->sfbCodeBook[ch];
    for (gp = 0; gp < icsInfo->numWinGroup; gp++) {
        for (sfb = 0; sfb < icsInfo->maxSFB; sfb++) {
            nVals = sfbTab[sfb + 1] - sfbTab[sfb];
            cb = *sfbCodeBook++;

            for (win = 0; win < icsInfo->winGroupLen[gp]; win++) {
                offset = win * NSAMPS_SHORT;
                if (cb == 0)
                    UnpackZeros(nVals, coef + offset);
                else if (cb <= 4)
                    UnpackQuads(cb, nVals, coef + offset);
                else if (cb <= 10)
                    UnpackPairsNoEsc(cb, nVals, coef + offset);
                else if (cb == 11)
                    UnpackPairsEsc(cb, nVals, coef + offset);
                else
                    UnpackZeros(nVals, coef + offset);
            }
            coef += nVals;
        }

        /* fill with zeros above maxSFB */
        for (win = 0; win < icsInfo->winGroupLen[gp]; win++) {
            offset = win * NSAMPS_SHORT;
            nVals = NSAMPS_SHORT - sfbTab[sfb];
            UnpackZeros(nVals, coef + offset);
        }
        coef += nVals;
        coef += (icsInfo->winGroupLen[gp] - 1) * NSAMPS_SHORT;
    }

    ASSERT(coef == m_PSInfoBase->coef[ch] + NSAMPS_LONG);
}

#ifndef AAC_ENABLE_SBR
/***********************************************************************************************************************
 * Function:    DecWindowOverlap
 *
 * Description: apply synthesis window, do overlap-add, clip to 16-bit PCM,
 *                for winSequence LONG-LONG
 *
 * Inputs:      input buffer (output of type-IV DCT)
 *              overlap buffer (saved from last time)
 *              number of channels
 *              window type (sin or KBD) for input buffer
 *              window type (sin or KBD) for overlap buffer
 *
 * Outputs:     one channel, one frame of 16-bit PCM, interleaved by nChans
 *
 * Return:      none
 *
 * Notes:       this processes one channel at a time, but skips every other sample in
 *                the output buffer (pcm) for stereo interleaving
 *              this should fit in registers on ARM
 *
 **********************************************************************************************************************/
void AACDecoder::DecWindowOverlap(int32_t *buf0, int32_t *over0, int16_t *pcm0, int32_t nChans, int32_t winTypeCurr,
                                  int32_t winTypePrev) {
    int32_t in, w0, w1, f0, f1;
    int32_t *buf1, *over1;
    int16_t *pcm1;
    const int32_t *wndCurr, *wndPrev;

    buf0 += (1024 >> 1);
    buf1 = buf0 - 1;
    pcm1 = pcm0 + (1024 - 1) * nChans;
    over1 = over0 + 1024 - 1;

    wndPrev = (winTypePrev == 1 ? kbdWindow + kbdWindowOffset[1] : sinWindow + sinWindowOffset[1]);
    if (winTypeCurr == winTypePrev) {
        /* cut window loads in half since current and overlap sections use same symmetric window */
        do {
            w0 = *wndPrev++;
            w1 = *wndPrev++;
            in = *buf0++;

            f0 = MULSHIFT32(w0, in);
            f1 = MULSHIFT32(w1, in);

            in = *over0;
            *pcm0 = CLIPTOSHORT((in - f0 + (1 << (FBITS_OUT_IMDCT - 1))) >> FBITS_OUT_IMDCT);
            pcm0 += nChans;

            in = *over1;
            *pcm1 = CLIPTOSHORT((in + f1 + (1 << (FBITS_OUT_IMDCT - 1))) >> FBITS_OUT_IMDCT);
            pcm1 -= nChans;

            in = *buf1--;
            *over1-- = MULSHIFT32(w0, in);
            *over0++ = MULSHIFT32(w1, in);
        } while (over0 < over1);
    } else {
        /* different windows for current and overlap parts - should still fit in registers on ARM w/o stack spill */
        wndCurr = (winTypeCurr == 1 ? kbdWindow + kbdWindowOffset[1] : sinWindow + sinWindowOffset[1]);
        do {
            w0 = *wndPrev++;
            w1 = *wndPrev++;
            in = *buf0++;

            f0 = MULSHIFT32(w0, in);
            f1 = MULSHIFT32(w1, in);

            in = *over0;
            *pcm0 = CLIPTOSHORT((in - f0 + (1 << (FBITS_OUT_IMDCT - 1))) >> FBITS_OUT_IMDCT);
            pcm0 += nChans;

            in = *over1;
            *pcm1 = CLIPTOSHORT((in + f1 + (1 << (FBITS_OUT_IMDCT - 1))) >> FBITS_OUT_IMDCT);
            pcm1 -= nChans;

            w0 = *wndCurr++;
            w1 = *wndCurr++;
            in = *buf1--;

            *over1-- = MULSHIFT32(w0, in);
            *over0++ = MULSHIFT32(w1, in);
        } while (over0 < over1);
    }
}

/***********************************************************************************************************************
 * Function:    DecWindowOverlapLongStart
 *
 * Description: apply synthesis window, do overlap-add, clip to 16-bit PCM,
 *                for winSequence LONG-START
 *
 * Inputs:      input buffer (output of type-IV DCT)
 *              overlap buffer (saved from last time)
 *              number of channels
 *              window type (sin or KBD) for input buffer
 *              window type (sin or KBD) for overlap buffer
 *
 * Outputs:     one channel, one frame of 16-bit PCM, interleaved by nChans
 *
 * Return:      none
 *
 * Notes:       this processes one channel at a time, but skips every other sample in
 *                the output buffer (pcm) for stereo interleaving
 *              this should fit in registers on ARM
 **********************************************************************************************************************/
void AACDecoder::DecWindowOverlapLongStart(int32_t *buf0, int32_t *over0, int16_t *pcm0, int32_t nChans,
                                           int32_t winTypeCurr, int32_t winTypePrev) {
    int32_t i, in, w0, w1, f0, f1;
    int32_t *buf1, *over1;
    int16_t *pcm1;
    const int32_t *wndPrev, *wndCurr;

    buf0 += (1024 >> 1);
    buf1 = buf0 - 1;
    pcm1 = pcm0 + (1024 - 1) * nChans;
    over1 = over0 + 1024 - 1;

    wndPrev = (winTypePrev == 1 ? kbdWindow + kbdWindowOffset[1] : sinWindow + sinWindowOffset[1]);
    i = 448; /* 2 outputs, 2 overlaps per loop */
    do {
        w0 = *wndPrev++;
        w1 = *wndPrev++;
        in = *buf0++;

        f0 = MULSHIFT32(w0, in);
        f1 = MULSHIFT32(w1, in);

        in = *over0;
        *pcm0 = CLIPTOSHORT((in - f0 + (1 << (FBITS_OUT_IMDCT - 1))) >> FBITS_OUT_IMDCT);
        pcm0 += nChans;

        in = *over1;
        *pcm1 = CLIPTOSHORT((in + f1 + (1 << (FBITS_OUT_IMDCT - 1))) >> FBITS_OUT_IMDCT);
        pcm1 -= nChans;

        in = *buf1--;

        *over1-- = 0;       /* Wn = 0 for n = (2047, 2046, ... 1600) */
        *over0++ = in >> 1; /* Wn = 1 for n = (1024, 1025, ... 1471) */
    } while (--i);

    wndCurr = (winTypeCurr == 1 ? kbdWindow + kbdWindowOffset[0] : sinWindow + sinWindowOffset[0]);

    /* do 64 more loops - 2 outputs, 2 overlaps per loop */
    do {
        w0 = *wndPrev++;
        w1 = *wndPrev++;
        in = *buf0++;

        f0 = MULSHIFT32(w0, in);
        f1 = MULSHIFT32(w1, in);

        in = *over0;
        *pcm0 = CLIPTOSHORT((in - f0 + (1 << (FBITS_OUT_IMDCT - 1))) >> FBITS_OUT_IMDCT);
        pcm0 += nChans;

        in = *over1;
        *pcm1 = CLIPTOSHORT((in + f1 + (1 << (FBITS_OUT_IMDCT - 1))) >> FBITS_OUT_IMDCT);
        pcm1 -= nChans;

        w0 = *wndCurr++; /* W[0], W[1], ... --> W[255], W[254], ... */
        w1 = *wndCurr++; /* W[127], W[126], ... --> W[128], W[129], ... */
        in = *buf1--;

        *over1-- = MULSHIFT32(w0, in); /* Wn = short window for n = (1599, 1598, ... , 1536) */
        *over0++ = MULSHIFT32(w1, in); /* Wn = short window for n = (1472, 1473, ... , 1535) */
    } while (over0 < over1);
}

/***********************************************************************************************************************
 * Function:    DecWindowOverlapLongStop
 *
 * Description: apply synthesis window, do overlap-add, clip to 16-bit PCM,
 *                for winSequence LONG-STOP
 *
 * Inputs:      input buffer (output of type-IV DCT)
 *              overlap buffer (saved from last time)
 *              number of channels
 *              window type (sin or KBD) for input buffer
 *              window type (sin or KBD) for overlap buffer
 *
 * Outputs:     one channel, one frame of 16-bit PCM, interleaved by nChans
 *
 * Return:      none
 *
 * Notes:       this processes one channel at a time, but skips every other sample in
 *                the output buffer (pcm) for stereo interleaving
 *              this should fit in registers on ARM
 **********************************************************************************************************************/
void AACDecoder::DecWindowOverlapLongStop(int32_t *buf0, int32_t *over0, int16_t *pcm0, int32_t nChans,
                                          int32_t winTypeCurr, int32_t winTypePrev) {
    int32_t i, in, w0, w1, f0, f1;
    int32_t *buf1, *over1;
    int16_t *pcm1;
    const int32_t *wndPrev, *wndCurr;

    buf0 += (1024 >> 1);
    buf1 = buf0 - 1;
    pcm1 = pcm0 + (1024 - 1) * nChans;
    over1 = over0 + 1024 - 1;

    wndPrev = (winTypePrev == 1 ? kbdWindow + kbdWindowOffset[0] : sinWindow + sinWindowOffset[0]);
    wndCurr = (winTypeCurr == 1 ? kbdWindow + kbdWindowOffset[1] : sinWindow + sinWindowOffset[1]);

    i = 448; /* 2 outputs, 2 overlaps per loop */
    do {
        /* Wn = 0 for n = (0, 1, ... 447) */
        /* Wn = 1 for n = (576, 577, ... 1023) */
        in = *buf0++;
        f1 = in >> 1; /* scale since skipping multiply by Q31 */

        in = *over0;
        *pcm0 = CLIPTOSHORT((in + (1 << (FBITS_OUT_IMDCT - 1))) >> FBITS_OUT_IMDCT);
        pcm0 += nChans;

        in = *over1;
        *pcm1 = CLIPTOSHORT((in + f1 + (1 << (FBITS_OUT_IMDCT - 1))) >> FBITS_OUT_IMDCT);
        pcm1 -= nChans;

        w0 = *wndCurr++;
        w1 = *wndCurr++;
        in = *buf1--;

        *over1-- = MULSHIFT32(w0, in);
        *over0++ = MULSHIFT32(w1, in);
    } while (--i);

    /* do 64 more loops - 2 outputs, 2 overlaps per loop */
    do {
        w0 = *wndPrev++; /* W[0], W[1], ...W[63] */
        w1 = *wndPrev++; /* W[127], W[126], ... W[64] */
        in = *buf0++;

        f0 = MULSHIFT32(w0, in);
        f1 = MULSHIFT32(w1, in);

        in = *over0;
        *pcm0 = CLIPTOSHORT((in - f0 + (1 << (FBITS_OUT_IMDCT - 1))) >> FBITS_OUT_IMDCT);
        pcm0 += nChans;

        in = *over1;
        *pcm1 = CLIPTOSHORT((in + f1 + (1 << (FBITS_OUT_IMDCT - 1))) >> FBITS_OUT_IMDCT);
        pcm1 -= nChans;

        w0 = *wndCurr++;
        w1 = *wndCurr++;
        in = *buf1--;

        *over1-- = MULSHIFT32(w0, in);
        *over0++ = MULSHIFT32(w1, in);
    } while (over0 < over1);
}

/***********************************************************************************************************************
 * Function:    DecWindowOverlapShort
 *
 * Description: apply synthesis window, do overlap-add, clip to 16-bit PCM,
 *                for winSequence EIGHT-SHORT (does all 8 short blocks)
 *
 * Inputs:      input buffer (output of type-IV DCT)
 *              overlap buffer (saved from last time)
 *              number of channels
 *              window type (sin or KBD) for input buffer
 *              window type (sin or KBD) for overlap buffer
 *
 * Outputs:     one channel, one frame of 16-bit PCM, interleaved by nChans
 *
 * Return:      none
 *
 * Notes:       this processes one channel at a time, but skips every other sample in
 *                the output buffer (pcm) for stereo interleaving
 *              this should fit in registers on ARM
 **********************************************************************************************************************/
void AACDecoder::DecWindowOverlapShort(int32_t *buf0, int32_t *over0, int16_t *pcm0, int32_t nChans,
                                       int32_t winTypeCurr, int32_t winTypePrev) {
    int32_t i, in, w0, w1, f0, f1;
    int32_t *buf1, *over1;
    int16_t *pcm1;
    const int32_t *wndPrev, *wndCurr;

    wndPrev = (winTypePrev == 1 ? kbdWindow + kbdWindowOffset[0] : sinWindow + sinWindowOffset[0]);
    wndCurr = (winTypeCurr == 1 ? kbdWindow + kbdWindowOffset[0] : sinWindow + sinWindowOffset[0]);

    /* pcm[0-447] = 0 + overlap[0-447] */
    i = 448;
    do {
        f0 = *over0++;
        f1 = *over0++;
        *pcm0 = CLIPTOSHORT((f0 + (1 << (FBITS_OUT_IMDCT - 1))) >> FBITS_OUT_IMDCT);
        pcm0 += nChans;
        *pcm0 = CLIPTOSHORT((f1 + (1 << (FBITS_OUT_IMDCT - 1))) >> FBITS_OUT_IMDCT);
        pcm0 += nChans;
        i -= 2;
    } while (i);

    /* pcm[448-575] = Wp[0-127] * block0[0-127] + overlap[448-575] */
    pcm1 = pcm0 + (128 - 1) * nChans;
    over1 = over0 + 128 - 1;
    buf0 += 64;
    buf1 = buf0 - 1;
    do {
        w0 = *wndPrev++; /* W[0], W[1], ...W[63] */
        w1 = *wndPrev++; /* W[127], W[126], ... W[64] */
        in = *buf0++;

        f0 = MULSHIFT32(w0, in);
        f1 = MULSHIFT32(w1, in);

        in = *over0;
        *pcm0 = CLIPTOSHORT((in - f0 + (1 << (FBITS_OUT_IMDCT - 1))) >> FBITS_OUT_IMDCT);
        pcm0 += nChans;

        in = *over1;
        *pcm1 = CLIPTOSHORT((in + f1 + (1 << (FBITS_OUT_IMDCT - 1))) >> FBITS_OUT_IMDCT);
        pcm1 -= nChans;

        w0 = *wndCurr++;
        w1 = *wndCurr++;
        in = *buf1--;

        /* save over0/over1 for next short block, in the slots just vacated */
        *over1-- = MULSHIFT32(w0, in);
        *over0++ = MULSHIFT32(w1, in);
    } while (over0 < over1);

    /* pcm[576-703] = Wc[128-255] * block0[128-255] + Wc[0-127] * block1[0-127] + overlap[576-703]
     * pcm[704-831] = Wc[128-255] * block1[128-255] + Wc[0-127] * block2[0-127] + overlap[704-831]
     * pcm[832-959] = Wc[128-255] * block2[128-255] + Wc[0-127] * block3[0-127] + overlap[832-959]
     */
    for (i = 0; i < 3; i++) {
        pcm0 += 64 * nChans;
        pcm1 = pcm0 + (128 - 1) * nChans;
        over0 += 64;
        over1 = over0 + 128 - 1;
        buf0 += 64;
        buf1 = buf0 - 1;
        wndCurr -= 128;

        do {
            w0 = *wndCurr++; /* W[0], W[1], ...W[63] */
            w1 = *wndCurr++; /* W[127], W[126], ... W[64] */
            in = *buf0++;

            f0 = MULSHIFT32(w0, in);
            f1 = MULSHIFT32(w1, in);

            in = *(over0 - 128); /* from last short block */
            in += *(over0 + 0);  /* from last full frame */
            *pcm0 = CLIPTOSHORT((in - f0 + (1 << (FBITS_OUT_IMDCT - 1))) >> FBITS_OUT_IMDCT);
            pcm0 += nChans;

            in = *(over1 - 128); /* from last short block */
            in += *(over1 + 0);  /* from last full frame */
            *pcm1 = CLIPTOSHORT((in + f1 + (1 << (FBITS_OUT_IMDCT - 1))) >> FBITS_OUT_IMDCT);
            pcm1 -= nChans;

            /* save over0/over1 for next short block, in the slots just vacated */
            in = *buf1--;
            *over1-- = MULSHIFT32(w0, in);
            *over0++ = MULSHIFT32(w1, in);
        } while (over0 < over1);
    }

    /* pcm[960-1023] = Wc[128-191] * block3[128-191] + Wc[0-63]   * block4[0-63] + overlap[960-1023]
     * over[0-63]    = Wc[192-255] * block3[192-255] + Wc[64-127] * block4[64-127]
     */
    pcm0 += 64 * nChans;
    over0 -= 832;            /* points at overlap[64] */
    over1 = over0 + 128 - 1; /* points at overlap[191] */
    buf0 += 64;
    buf1 = buf0 - 1;
    wndCurr -= 128;
    do {
        w0 = *wndCurr++; /* W[0], W[1], ...W[63] */
        w1 = *wndCurr++; /* W[127], W[126], ... W[64] */
        in = *buf0++;

        f0 = MULSHIFT32(w0, in);
        f1 = MULSHIFT32(w1, in);

        in = *(over0 + 768);  /* from last short block */
        in += *(over0 + 896); /* from last full frame */
        *pcm0 = CLIPTOSHORT((in - f0 + (1 << (FBITS_OUT_IMDCT - 1))) >> FBITS_OUT_IMDCT);
        pcm0 += nChans;

        in = *(over1 + 768); /* from last short block */
        *(over1 - 128) = in + f1;

        in = *buf1--;
        *over1-- = MULSHIFT32(w0, in); /* save in overlap[128-191] */
        *over0++ = MULSHIFT32(w1, in); /* save in overlap[64-127] */
    } while (over0 < over1);

    /* over0 now points at overlap[128] */

    /* over[64-191]   = Wc[128-255] * block4[128-255] + Wc[0-127] * block5[0-127]
     * over[192-319]  = Wc[128-255] * block5[128-255] + Wc[0-127] * block6[0-127]
     * over[320-447]  = Wc[128-255] * block6[128-255] + Wc[0-127] * block7[0-127]
     * over[448-576]  = Wc[128-255] * block7[128-255]
     */
    for (i = 0; i < 3; i++) {
        over0 += 64;
        over1 = over0 + 128 - 1;
        buf0 += 64;
        buf1 = buf0 - 1;
        wndCurr -= 128;
        do {
            w0 = *wndCurr++; /* W[0], W[1], ...W[63] */
            w1 = *wndCurr++; /* W[127], W[126], ... W[64] */
            in = *buf0++;

            f0 = MULSHIFT32(w0, in);
            f1 = MULSHIFT32(w1, in);

            /* from last short block */
            *(over0 - 128) -= f0;
            *(over1 - 128) += f1;

            in = *buf1--;
            *over1-- = MULSHIFT32(w0, in);
            *over0++ = MULSHIFT32(w1, in);
        } while (over0 < over1);
    }

    /* over[576-1024] = 0 */
    i = 448;
    over0 += 64;
    do {
        *over0++ = 0;
        *over0++ = 0;
        *over0++ = 0;
        *over0++ = 0;
        i -= 4;
    } while (i);
}

#endif /* !AAC_ENABLE_SBR */

/***********************************************************************************************************************
 * Function:    IMDCT
 *
 * Description: inverse transform and convert to 16-bit PCM
 *
 * Inputs:      index of current channel (0 for SCE/LFE, 0 or 1 for CPE)
 *              output channel (range = [0, nChans-1])
 *
 * Outputs:     complete frame of decoded PCM, after inverse transform
 *
 * Return:      0 if successful, -1 if error
 *
 * Notes:       If AAC_ENABLE_SBR is defined at compile time then window + overlap
 *                does NOT clip to 16-bit PCM and does NOT interleave channels
 *              If AAC_ENABLE_SBR is NOT defined at compile time, then window + overlap
 *                does clip to 16-bit PCM and interleaves channels
 *              If SBR is enabled at compile time, but we don't know whether it is
 *                actually used for this frame (e.g. the first frame of a stream),
 *                we need to produce both clipped 16-bit PCM in outbuf AND
 *                unclipped 32-bit PCM in the SBR input buffer. In this case we make
 *                a separate pass over the 32-bit PCM to produce 16-bit PCM output.
 *                This inflicts a slight performance hit when decoding non-SBR files.
 **********************************************************************************************************************/
int32_t AACDecoder::IMDCT(int32_t ch, int32_t chOut, int16_t *outbuf) {
    int32_t i;
    ICSInfo_t *icsInfo;

    icsInfo = (ch == 1 && m_PSInfoBase->commonWin == 1) ? &(m_PSInfoBase->icsInfo[0]) : &(m_PSInfoBase->icsInfo[ch]);
    outbuf += chOut;

    /* optimized type-IV DCT (operates inplace) */
    if (icsInfo->winSequence == 2) {
        /* 8 short blocks */
        for (i = 0; i < 8; i++)
            DCT4(0, m_PSInfoBase->coef[ch] + i * 128, m_PSInfoBase->gbCurrent[ch]);
    } else {
        /* 1 long block */
        DCT4(1, m_PSInfoBase->coef[ch], m_PSInfoBase->gbCurrent[ch]);
    }

#ifdef AAC_ENABLE_SBR
    /* window, overlap-add, don't clip to short (send to SBR decoder)
     * store the decoded 32-bit samples in top half (second AAC_MAX_NSAMPS samples) of coef buffer
     */
    if (icsInfo->winSequence == 0)
        DecWindowOverlapNoClip(m_PSInfoBase->coef[ch], m_PSInfoBase->overlap[chOut], m_PSInfoBase->sbrWorkBuf[ch],
                               icsInfo->winShape, m_PSInfoBase->prevWinShape[chOut]);
    else if (icsInfo->winSequence == 1)
        DecWindowOverlapLongStartNoClip(m_PSInfoBase->coef[ch], m_PSInfoBase->overlap[chOut],
                                        m_PSInfoBase->sbrWorkBuf[ch], icsInfo->winShape,
                                        m_PSInfoBase->prevWinShape[chOut]);
    else if (icsInfo->winSequence == 2)
        DecWindowOverlapShortNoClip(m_PSInfoBase->coef[ch], m_PSInfoBase->overlap[chOut], m_PSInfoBase->sbrWorkBuf[ch],
                                    icsInfo->winShape, m_PSInfoBase->prevWinShape[chOut]);
    else if (icsInfo->winSequence == 3)
        DecWindowOverlapLongStopNoClip(m_PSInfoBase->coef[ch], m_PSInfoBase->overlap[chOut],
                                       m_PSInfoBase->sbrWorkBuf[ch], icsInfo->winShape,
                                       m_PSInfoBase->prevWinShape[chOut]);

    if (!m_AACDecInfo->sbrEnabled) {
        for (i = 0; i < AAC_MAX_NSAMPS; i++) {
            *outbuf = CLIPTOSHORT((m_PSInfoBase->sbrWorkBuf[ch][i] + RND_VAL) >> FBITS_OUT_IMDCT);
            outbuf += m_AACDecInfo->nChans;
        }
    }

    m_AACDecInfo->rawSampleBuf[ch] = m_PSInfoBase->sbrWorkBuf[ch];
    m_AACDecInfo->rawSampleBytes = sizeof(int32_t);
    m_AACDecInfo->rawSampleFBits = FBITS_OUT_IMDCT;
#else
    /* window, overlap-add, round to PCM - optimized for each window sequence */
    if (icsInfo->winSequence == 0)
        DecWindowOverlap(m_PSInfoBase->coef[ch], m_PSInfoBase->overlap[chOut], outbuf, m_AACDecInfo->nChans,
                         icsInfo->winShape, m_PSInfoBase->prevWinShape[chOut]);
    else if (icsInfo->winSequence == 1)
        DecWindowOverlapLongStart(m_PSInfoBase->coef[ch], m_PSInfoBase->overlap[chOut], outbuf, m_AACDecInfo->nChans,
                                  icsInfo->winShape, m_PSInfoBase->prevWinShape[chOut]);
    else if (icsInfo->winSequence == 2)
        DecWindowOverlapShort(m_PSInfoBase->coef[ch], m_PSInfoBase->overlap[chOut], outbuf, m_AACDecInfo->nChans,
                              icsInfo->winShape, m_PSInfoBase->prevWinShape[chOut]);
    else if (icsInfo->winSequence == 3)
        DecWindowOverlapLongStop(m_PSInfoBase->coef[ch], m_PSInfoBase->overlap[chOut], outbuf, m_AACDecInfo->nChans,
                                 icsInfo->winShape, m_PSInfoBase->prevWinShape[chOut]);

    m_AACDecInfo->rawSampleBuf[ch] = 0;
    m_AACDecInfo->rawSampleBytes = 0;
    m_AACDecInfo->rawSampleFBits = 0;
#endif

    m_PSInfoBase->prevWinShape[chOut] = icsInfo->winShape;

    return 0;
}

/***********************************************************************************************************************
 * Function:    DecodeICSInfo
 *
 * Description: decode individual channel stream info
 *
 * Inputs:      sample rate index
 *
 * Outputs:     updated icsInfo struct
 *
 * Return:      none
 **********************************************************************************************************************/
void AACDecoder::DecodeICSInfo(ICSInfo_t *icsInfo, int32_t sampRateIdx) {
    int32_t sfb, g, mask;

    icsInfo->icsResBit = GetBits(1);
    icsInfo->winSequence = GetBits(2);
    icsInfo->winShape = GetBits(1);
    if (icsInfo->winSequence == 2) {
        /* short block */
        icsInfo->maxSFB = GetBits(4);
        icsInfo->sfGroup = GetBits(7);
        icsInfo->numWinGroup = 1;
        icsInfo->winGroupLen[0] = 1;
        mask = 0x40; /* start with bit 6 */
        for (g = 0; g < 7; g++) {
            if (icsInfo->sfGroup & mask) {
                icsInfo->winGroupLen[icsInfo->numWinGroup - 1]++;
            } else {
                icsInfo->numWinGroup++;
                icsInfo->winGroupLen[icsInfo->numWinGroup - 1] = 1;
            }
            mask >>= 1;
        }
    } else {
        /* long block */
        icsInfo->maxSFB = GetBits(6);
        icsInfo->predictorDataPresent = GetBits(1);
        if (icsInfo->predictorDataPresent) {
            icsInfo->predictorReset = GetBits(1);
            if (icsInfo->predictorReset)
                icsInfo->predictorResetGroupNum = GetBits(5);
            for (sfb = 0; sfb < MIN(icsInfo->maxSFB, predSFBMax[sampRateIdx]); sfb++)
                icsInfo->predictionUsed[sfb] = GetBits(1);
        }
        icsInfo->numWinGroup = 1;
        icsInfo->winGroupLen[0] = 1;
    }
}

/***********************************************************************************************************************
 * Function:    DecodeSectionData
 *
 * Description: decode section data (scale factor band groupings and
 *                associated Huffman codebooks)
 *
 * Inputs:      window sequence (short or long blocks)
 *              number of window groups (1 for long blocks, 1-8 for short blocks)
 *              max coded scalefactor band
 *
 * Outputs:     index of Huffman codebook for each scalefactor band in each section
 *
 * Return:      none
 *
 * Notes:       sectCB, sectEnd, sfbCodeBook, ordered by window groups for short blocks
 **********************************************************************************************************************/
void AACDecoder::DecodeSectionData(int32_t winSequence, int32_t numWinGrp, int32_t maxSFB, uint8_t *sfbCodeBook) {
    int32_t g, cb, sfb;
    int32_t sectLen, sectLenBits, sectLenIncr, sectEscapeVal;

    sectLenBits = (winSequence == 2 ? 3 : 5);
    sectEscapeVal = (1 << sectLenBits) - 1;

    for (g = 0; g < numWinGrp; g++) {
        sfb = 0;
        while (sfb < maxSFB) {
            cb = GetBits(4); /* next section codebook */
            sectLen = 0;
            do {
                sectLenIncr = GetBits(sectLenBits);
                sectLen += sectLenIncr;
            } while (sectLenIncr == sectEscapeVal);

            sfb += sectLen;
            while (sectLen--)
                *sfbCodeBook++ = (uint8_t)cb;
        }
        ASSERT(sfb == maxSFB);
    }
}

/***********************************************************************************************************************
 * Function:    DecodeOneScaleFactor
 *
 * Description: decode one scalefactor using scalefactor Huffman codebook
 *
 * Inputs:      none
 *
 * Outputs:     none
 *
 * Return:      one decoded scalefactor, including index_offset of -60
 **********************************************************************************************************************/
int32_t AACDecoder::DecodeOneScaleFactor() {
    int32_t nBits, val;
    uint32_t bitBuf;

    /* decode next scalefactor from bitstream */
    bitBuf = GetBitsNoAdvance(huffTabScaleFactInfo.maxBits) << (32 - huffTabScaleFactInfo.maxBits);
    nBits = DecodeHuffmanScalar(huffTabScaleFact, &huffTabScaleFactInfo, bitBuf, &val);
    AdvanceBitstream(nBits);
    return val;
}

/***********************************************************************************************************************
 * Function:    DecodeScaleFactors
 *
 * Description: decode scalefactors, PNS energy, and intensity stereo weights
 *
 * Inputs:      number of window groups (1 for long blocks, 1-8 for short blocks)
 *              max coded scalefactor band
 *              global gain (starting value for differential scalefactor coding)
 *              index of Huffman codebook for each scalefactor band in each section
 *
 * Outputs:     decoded scalefactor for each section
 *
 * Return:      none
 *
 * Notes:       sfbCodeBook, scaleFactors ordered by window groups for short blocks
 *              for section with codebook 13, scaleFactors buffer has decoded PNS
 *                energy instead of regular scalefactor
 *              for section with codebook 14 or 15, scaleFactors buffer has intensity
 *                stereo weight instead of regular scalefactor
 **********************************************************************************************************************/
void AACDecoder::DecodeScaleFactors(int32_t numWinGrp, int32_t maxSFB, int32_t globalGain, uint8_t *sfbCodeBook,
                                    int16_t *scaleFactors) {
    int32_t g, sfbCB, nrg, npf, val, sf, is;

    /* starting values for differential coding */
    sf = globalGain;
    is = 0;
    nrg = globalGain - 90 - 256;
    npf = 1;

    for (g = 0; g < numWinGrp * maxSFB; g++) {
        sfbCB = *sfbCodeBook++;

        if (sfbCB == 14 || sfbCB == 15) {
            /* intensity stereo - differential coding */
            val = DecodeOneScaleFactor();
            is += val;
            *scaleFactors++ = (int16_t)is;
        } else if (sfbCB == 13) {
            /* PNS - first energy is directly coded, rest are Huffman coded (npf = noise_pcm_flag) */
            if (npf) {
                val = GetBits(9);
                npf = 0;
            } else {
                val = DecodeOneScaleFactor();
            }
            nrg += val;
            *scaleFactors++ = (int16_t)nrg;
        } else if (sfbCB >= 1 && sfbCB <= 11) {
            /* regular (non-zero) region - differential coding */
            val = DecodeOneScaleFactor();
            sf += val;
            *scaleFactors++ = (int16_t)sf;
        } else {
            /* inactive scalefactor band if codebook 0 */
            *scaleFactors++ = 0;
        }
    }
}

/***********************************************************************************************************************
 * Function:    DecodePulseInfo
 *
 * Description: decode pulse information
 *
 * Inputs:      none
 *
 * Outputs:     updated PulseInfo_t struct
 *
 * Return:      none
 **********************************************************************************************************************/
void AACDecoder::DecodePulseInfo(uint8_t ch) {
    int32_t i;

    m_pulseInfo[ch].numPulse = GetBits(2) + 1; /* add 1 here */
    m_pulseInfo[ch].startSFB = GetBits(6);
    for (i = 0; i < m_pulseInfo[ch].numPulse; i++) {
        m_pulseInfo[ch].offset[i] = GetBits(5);
        m_pulseInfo[ch].amp[i] = GetBits(4);
    }
}

/***********************************************************************************************************************
 * Function:    DecodeTNSInfo
 *
 * Description: decode TNS filter information
 *
 * Inputs:      window sequence (short or long blocks)
 *
 * Outputs:     updated TNSInfo_t struct
 *              buffer of decoded (signed) TNS filter coefficients
 *
 * Return:      none
 **********************************************************************************************************************/
void AACDecoder::DecodeTNSInfo(int32_t winSequence, TNSInfo_t *ti, int8_t *tnsCoef) {
    int32_t i, w, f, coefBits, compress;
    int8_t c, s, n;
    uint8_t *filtLength, *filtOrder, *filtDir;

    filtLength = ti->length;
    filtOrder = ti->order;
    filtDir = ti->dir;

    if (winSequence == 2) {
        /* short blocks */
        for (w = 0; w < NWINDOWS_SHORT; w++) {
            ti->numFilt[w] = GetBits(1);
            if (ti->numFilt[w]) {
                ti->coefRes[w] = GetBits(1) + 3;
                *filtLength = GetBits(4);
                *filtOrder = GetBits(3);
                if (*filtOrder) {
                    *filtDir++ = GetBits(1);
                    compress = GetBits(1);
                    coefBits = (int32_t)ti->coefRes[w] - compress; /* 2, 3, or 4 */
                    s = sgnMask[coefBits - 2];
                    n = negMask[coefBits - 2];
                    for (i = 0; i < *filtOrder; i++) {
                        c = GetBits(coefBits);
                        if (c & s)
                            c |= n;
                        *tnsCoef++ = c;
                    }
                }
                filtLength++;
                filtOrder++;
            }
        }
    } else {
        /* long blocks */
        ti->numFilt[0] = GetBits(2);
        if (ti->numFilt[0])
            ti->coefRes[0] = GetBits(1) + 3;
        for (f = 0; f < ti->numFilt[0]; f++) {
            *filtLength = GetBits(6);
            *filtOrder = GetBits(5);
            if (*filtOrder) {
                *filtDir++ = GetBits(1);
                compress = GetBits(1);
                coefBits = (int32_t)ti->coefRes[0] - compress; /* 2, 3, or 4 */
                s = sgnMask[coefBits - 2];
                n = negMask[coefBits - 2];
                for (i = 0; i < *filtOrder; i++) {
                    c = GetBits(coefBits);
                    if (c & s)
                        c |= n;
                    *tnsCoef++ = c;
                }
            }
            filtLength++;
            filtOrder++;
        }
    }
}

/* bitstream field lengths for gain control data:
 *   gainBits[winSequence][0] = maxWindow (how many gain windows there are)
 *   gainBits[winSequence][1] = locBitsZero (bits for alocCode if window == 0)
 *   gainBits[winSequence][2] = locBits (bits for alocCode if window != 0)
 */
static const uint8_t gainBits[4][3] = {
    {1, 5, 5}, /* long */
    {2, 4, 2}, /* start */
    {8, 2, 2}, /* short */
    {2, 4, 5}, /* stop */
};

/***********************************************************************************************************************
 * Function:    DecodeGainControlInfo
 *
 * Description: decode gain control information (SSR profile only)
 *
 * Inputs:      window sequence (short or long blocks)
 *
 * Outputs:     updated GainControlInfo_t struct
 *
 * Return:      none
 **********************************************************************************************************************/
void AACDecoder::DecodeGainControlInfo(int32_t winSequence, GainControlInfo_t *gi) {
    int32_t bd, wd, ad;
    int32_t locBits, locBitsZero, maxWin;

    gi->maxBand = GetBits(2);
    maxWin = (int32_t)gainBits[winSequence][0];
    locBitsZero = (int32_t)gainBits[winSequence][1];
    locBits = (int32_t)gainBits[winSequence][2];

    for (bd = 1; bd <= gi->maxBand; bd++) {
        for (wd = 0; wd < maxWin; wd++) {
            gi->adjNum[bd][wd] = GetBits(3);
            for (ad = 0; ad < gi->adjNum[bd][wd]; ad++) {
                gi->alevCode[bd][wd][ad] = GetBits(4);
                gi->alocCode[bd][wd][ad] = GetBits(wd == 0 ? locBitsZero : locBits);
            }
        }
    }
}

/***********************************************************************************************************************
 * Function:    DecodeICS
 *
 * Description: decode individual channel stream
 *
 * Inputs:      index of current channel
 *
 * Outputs:     updated section data, scale factor data, pulse data, TNS data,
 *                and gain control data
 *
 * Return:      none
 **********************************************************************************************************************/
void AACDecoder::DecodeICS(int32_t ch) {
    int32_t globalGain;
    ICSInfo_t *icsInfo;
    TNSInfo_t *ti;
    GainControlInfo_t *gi;

    icsInfo = (ch == 1 && m_PSInfoBase->commonWin == 1) ? &(m_PSInfoBase->icsInfo[0]) : &(m_PSInfoBase->icsInfo[ch]);

    globalGain = GetBits(8);
    if (!m_PSInfoBase->commonWin)
        DecodeICSInfo(icsInfo, m_PSInfoBase->sampRateIdx);

    DecodeSectionData(icsInfo->winSequence, icsInfo->numWinGroup, icsInfo->maxSFB, m_PSInfoBase->sfbCodeBook[ch]);

    DecodeScaleFactors(icsInfo->numWinGroup, icsInfo->maxSFB, globalGain, m_PSInfoBase->sfbCodeBook[ch],
                       m_PSInfoBase->scaleFactors[ch]);

    m_pulseInfo[ch].pulseDataPresent = GetBits(1);
    if (m_pulseInfo[ch].pulseDataPresent)
        DecodePulseInfo(ch);

    ti = &m_PSInfoBase->tnsInfo[ch];
    ti->tnsDataPresent = GetBits(1);
    if (ti->tnsDataPresent)
        DecodeTNSInfo(icsInfo->winSequence, ti, ti->coef);

    gi = &m_PSInfoBase->gainControlInfo[ch];
    gi->gainControlDataPresent = GetBits(1);
    if (gi->gainControlDataPresent)
        DecodeGainControlInfo(icsInfo->winSequence, gi);
}

/***********************************************************************************************************************
 * Function:    DecodeNoiselessData
 *
 * Description: decode noiseless data (side info and transform coefficients)
 *
 * Inputs:      double pointer to buffer pointing to start of individual channel stream
 *                (14496-3, table 4.4.24)
 *              pointer to bit offset
 *              pointer to number of valid bits remaining in buf
 *              index of current channel
 *
 * Outputs:     updated global gain, section data, scale factor data, pulse data,
 *                TNS data, gain control data, and spectral data
 *
 * Return:      0 if successful, error code (< 0) if error
 **********************************************************************************************************************/
int32_t AACDecoder::DecodeNoiselessData(uint8_t **buf, int32_t *bitOffset, int32_t *bitsAvail, int32_t ch) {
    int32_t bitsUsed;
    ICSInfo_t *icsInfo;

    icsInfo = (ch == 1 && m_PSInfoBase->commonWin == 1) ? &(m_PSInfoBase->icsInfo[0]) : &(m_PSInfoBase->icsInfo[ch]);

    SetBitstreamPointer((*bitsAvail + 7) >> 3, *buf);
    GetBits(*bitOffset);

    DecodeICS(ch);

    if (icsInfo->winSequence == 2)
        DecodeSpectrumShort(ch);
    else
        DecodeSpectrumLong(ch);

    bitsUsed = CalcBitsUsed(*buf, *bitOffset);
    *buf += ((bitsUsed + *bitOffset) >> 3);
    *bitOffset = ((bitsUsed + *bitOffset) & 0x07);
    *bitsAvail -= bitsUsed;

    m_AACDecInfo->sbDeinterleaveReqd[ch] = 0;
    m_AACDecInfo->tnsUsed |= m_PSInfoBase->tnsInfo[ch].tnsDataPresent; /* set flag if TNS used for any channel */

    return ERR_AAC_NONE;
}
/***********************************************************************************************************************
 * Function:    DecodeHuffmanScalar
 *
 * Description: decode one Huffman symbol from bitstream
 *
 * Inputs:      pointers to Huffman table and info struct
 *              left-aligned bit buffer with >= huffTabInfo->maxBits bits
 *
 * Outputs:     decoded symbol in *val
 *
 * Return:      number of bits in symbol
 *
 * Notes:       assumes canonical Huffman codes:
 *                first CW always 0, we have "count" CW's of length "nBits" bits
 *                starting CW for codes of length nBits+1 =
 *                  (startCW[nBits] + count[nBits]) << 1
 *                if there are no codes at nBits, then we just keep << 1 each time
 *                  (since count[nBits] = 0)
 **********************************************************************************************************************/
int32_t AACDecoder::DecodeHuffmanScalar(const uint16_t *huffTab, const HuffInfo_t *huffTabInfo, uint32_t bitBuf,
                                        int32_t *val) {
    uint32_t count, start, shift, t;
    const uint8_t *countPtr;
    const uint16_t *map;

    map = huffTab + huffTabInfo->offset;
    countPtr = huffTabInfo->count;

    start = 0;
    count = 0;
    shift = 32;
    do {
        start += count;
        start <<= 1;
        map += count;
        count = *countPtr++;
        shift--;
        t = (bitBuf >> shift) - start;
    } while (t >= count);

    *val = (int32_t)map[t];
    return (countPtr - huffTabInfo->count);
}

/***********************************************************************************************************************
* Function:    UnpackADTSHeader
*
* Description: parse the ADTS frame header and initialize decoder state, Audio Data Transport Stream
*
* Inputs:      double pointer to buffer with complete ADTS frame header (byte aligned)
*                header size = 7 bytes, plus 2 if CRC
*
* Outputs:     filled in ADTS struct
*              updated buffer pointer
*              updated bit offset
*              updated number of available bits
*
* Return:      0 if successful, error code (< 0) if error
*              verify that fixed fields don't change between frames
***********************************************************************************************************************/
int32_t AACDecoder::UnpackADTSHeader(uint8_t **buf, int32_t *bitOffset, int32_t *bitsAvail) {
    int32_t bitsUsed;

    /* init bitstream reader */
    SetBitstreamPointer((*bitsAvail + 7) >> 3, *buf);
    GetBits(*bitOffset);

    /* verify that first 12 bits of header are syncword */
    if (GetBits(12) != 0x0fff) {
        return ERR_AAC_INVALID_ADTS_HEADER;
    }

    /* fixed fields - should not change from frame to frame */
    m_fhADTS.id = GetBits(1);
    m_fhADTS.layer = GetBits(2);
    m_fhADTS.protectBit = GetBits(1);
    m_fhADTS.profile = GetBits(2);
    m_fhADTS.sampRateIdx = GetBits(4);
    m_fhADTS.privateBit = GetBits(1);
    m_fhADTS.channelConfig = GetBits(3);
    m_fhADTS.origCopy = GetBits(1);
    m_fhADTS.home = GetBits(1);

    /* variable fields - can change from frame to frame */
    m_fhADTS.copyBit = GetBits(1);
    m_fhADTS.copyStart = GetBits(1);
    m_fhADTS.frameLength = GetBits(13);
    m_fhADTS.bufferFull = GetBits(11);
    m_fhADTS.numRawDataBlocks = GetBits(2) + 1;

    /* note - MPEG4 spec, correction 1 changes how CRC is handled when protectBit == 0 and numRawDataBlocks > 1 */
    if (m_fhADTS.protectBit == 0)
        m_fhADTS.crcCheckWord = GetBits(16);

    /* byte align */
    ByteAlignBitstream(); /* should always be aligned anyway */

    /* check validity of header */
    if (m_fhADTS.layer != 0 || m_fhADTS.profile != AAC_PROFILE_LC || m_fhADTS.sampRateIdx >= NUM_SAMPLE_RATES ||
        m_fhADTS.channelConfig >= NUM_DEF_CHAN_MAPS)
        return ERR_AAC_INVALID_ADTS_HEADER;

#ifndef AAC_ENABLE_MPEG4
    if (m_fhADTS.id != 1)
        return ERR_AAC_MPEG4_UNSUPPORTED;
#endif

    /* update codec info */
    m_PSInfoBase->sampRateIdx = m_fhADTS.sampRateIdx;
    if (!m_PSInfoBase->useImpChanMap)
        m_PSInfoBase->nChans = channelMapTab[m_fhADTS.channelConfig];

    /* syntactic element fields will be read from bitstream for each element */
    m_AACDecInfo->prevBlockID = AAC_ID_INVALID;
    m_AACDecInfo->currBlockID = AAC_ID_INVALID;
    m_AACDecInfo->currInstTag = -1;

    /* fill in user-accessible data */
    m_AACDecInfo->bitRate = 0;
    m_AACDecInfo->nChans = m_PSInfoBase->nChans;
    m_AACDecInfo->sampRate = sampRateTab[m_PSInfoBase->sampRateIdx];
    m_AACDecInfo->id = m_fhADTS.id;
    m_AACDecInfo->profile = m_fhADTS.profile;
    m_AACDecInfo->sbrEnabled = 0;
    m_AACDecInfo->adtsBlocksLeft = m_fhADTS.numRawDataBlocks;

    /* update bitstream reader */
    bitsUsed = CalcBitsUsed(*buf, *bitOffset);
    *buf += (bitsUsed + *bitOffset) >> 3;
    *bitOffset = (bitsUsed + *bitOffset) & 0x07;
    *bitsAvail -= bitsUsed;
    if (*bitsAvail < 0)
        return ERR_AAC_INDATA_UNDERFLOW;

    return ERR_AAC_NONE;
}

/***********************************************************************************************************************
* Function:    GetADTSChannelMapping
*
* Description: determine the number of channels from implicit mapping rules
*
* Inputs:      pointer to start of raw_data_block
*              bit offset
*              bits available
*
* Outputs:     updated number of channels
*
* Return:      0 if successful, error code (< 0) if error
*
* Notes:       calculates total number of channels using rules in 14496-3, 4.5.1.2.1
*              does not attempt to deduce speaker geometry
***********************************************************************************************************************/
int32_t AACDecoder::GetADTSChannelMapping(uint8_t *buf, int32_t bitOffset, int32_t bitsAvail) {
    int32_t ch, nChans, elementChans, err;

    nChans = 0;
    do {
        /* parse next syntactic element */
        err = DecodeNextElement(&buf, &bitOffset, &bitsAvail);
        if (err)
            return err;

        elementChans = elementNumChans[m_AACDecInfo->currBlockID];
        nChans += elementChans;

        for (ch = 0; ch < elementChans; ch++) {
            err = DecodeNoiselessData(&buf, &bitOffset, &bitsAvail, ch);
            if (err)
                return err;
        }
    } while (m_AACDecInfo->currBlockID != AAC_ID_END);

    if (nChans <= 0)
        return ERR_AAC_CHANNEL_MAP;

    /* update number of channels in codec state and user-accessible info structs */
    m_PSInfoBase->nChans = nChans;
    m_AACDecInfo->nChans = m_PSInfoBase->nChans;
    m_PSInfoBase->useImpChanMap = 1;

    return ERR_AAC_NONE;
}

/***********************************************************************************************************************
* Function:    GetNumChannelsADIF
*
* Description: get number of channels from program config elements in an ADIF file
*
* Inputs:      array of filled-in program config element structures
*              number of PCE's
*
* Outputs:     none
*
* Return:      total number of channels in file
*              -1 if error (invalid number of PCE's or unsupported mode)
***********************************************************************************************************************/
int32_t AACDecoder::GetNumChannelsADIF(int32_t nPCE) {
    int32_t i, j, nChans;

    if (nPCE < 1 || nPCE > MAX_NUM_PCE_ADIF)
        return -1;

    nChans = 0;
    for (i = 0; i < nPCE; i++) {
        /* for now: only support LC, no channel coupling */
        if (m_pce[i]->profile != AAC_PROFILE_LC || m_pce[i]->numCCE > 0)
            return -1;

        /* add up number of channels in all channel elements (assume all single-channel) */
        nChans += m_pce[i]->numFCE;
        nChans += m_pce[i]->numSCE;
        nChans += m_pce[i]->numBCE;
        nChans += m_pce[i]->numLCE;

        /* add one more for every element which is a channel pair */
        for (j = 0; j < m_pce[i]->numFCE; j++) {
            if ((m_pce[i]->fce[j] & 0x10) >> 4) /* bit 4 = SCE/CPE flag */
                nChans++;
        }
        for (j = 0; j < m_pce[i]->numSCE; j++) {
            if ((m_pce[i]->sce[j] & 0x10) >> 4) /* bit 4 = SCE/CPE flag */
                nChans++;
        }
        for (j = 0; j < m_pce[i]->numBCE; j++) {
            if ((m_pce[i]->bce[j] & 0x10) >> 4) /* bit 4 = SCE/CPE flag */
                nChans++;
        }
    }

    return nChans;
}

/***********************************************************************************************************************
* Function:    GetSampleRateIdxADIF
*
* Description: get sampling rate index from program config elements in an ADIF file
*
* Inputs:      array of filled-in program config element structures
*              number of PCE's
*
* Outputs:     none
*
* Return:      sample rate of file
*              -1 if error (invalid number of PCE's or sample rate mismatch)
***********************************************************************************************************************/
int32_t AACDecoder::GetSampleRateIdxADIF(int32_t nPCE) {
    int32_t i, idx;

    if (nPCE < 1 || nPCE > MAX_NUM_PCE_ADIF)
        return -1;

    /* make sure all PCE's have the same sample rate */
    idx = m_pce[0]->sampRateIdx;
    for (i = 1; i < nPCE; i++) {
        if (m_pce[i]->sampRateIdx != idx)
            return -1;
    }

    return idx;
}

/***********************************************************************************************************************
* Function:    UnpackADIFHeader
*
* Description: parse the ADIF file header and initialize decoder state
*
* Inputs:      double pointer to buffer with complete ADIF header
*                (starting at 'A' in 'ADIF' tag)
*              pointer to bit offset
*              pointer to number of valid bits remaining in inbuf
*
* Outputs:     filled-in ADIF struct
*              updated buffer pointer
*              updated bit offset
*              updated number of available bits
*
* Return:      0 if successful, error code (< 0) if error
***********************************************************************************************************************/
int32_t AACDecoder::UnpackADIFHeader(uint8_t **buf, int32_t *bitOffset, int32_t *bitsAvail) {
    uint8_t i;
    int32_t bitsUsed;

    /* init bitstream reader */
    SetBitstreamPointer((*bitsAvail + 7) >> 3, *buf);
    GetBits(*bitOffset);

    /* verify that first 32 bits of header are "ADIF" */
    if (GetBits(8) != 'A' || GetBits(8) != 'D' || GetBits(8) != 'I' || GetBits(8) != 'F')
        return ERR_AAC_INVALID_ADIF_HEADER;

    /* read ADIF header fields */
    m_fhADIF.copyBit = GetBits(1);
    if (m_fhADIF.copyBit) {
        for (i = 0; i < ADIF_COPYID_SIZE; i++)
            m_fhADIF.copyID[i] = GetBits(8);
    }
    m_fhADIF.origCopy = GetBits(1);
    m_fhADIF.home = GetBits(1);
    m_fhADIF.bsType = GetBits(1);
    m_fhADIF.bitRate = GetBits(23);
    m_fhADIF.numPCE = GetBits(4) + 1; /* add 1 (so range = [1, 16]) */
    if (m_fhADIF.bsType == 0)
        m_fhADIF.bufferFull = GetBits(20);

    /* parse all program config elements */
    for (i = 0; i < m_fhADIF.numPCE; i++)
        DecodeProgramConfigElement(i);

    /* byte align */
    ByteAlignBitstream();

    /* update codec info */
    m_PSInfoBase->nChans = GetNumChannelsADIF(m_fhADIF.numPCE);
    m_PSInfoBase->sampRateIdx = GetSampleRateIdxADIF(m_fhADIF.numPCE);

    /* check validity of header */
    if (m_PSInfoBase->nChans < 0 || m_PSInfoBase->sampRateIdx < 0 || m_PSInfoBase->sampRateIdx >= NUM_SAMPLE_RATES)
        return ERR_AAC_INVALID_ADIF_HEADER;

    /* syntactic element fields will be read from bitstream for each element */
    m_AACDecInfo->prevBlockID = AAC_ID_INVALID;
    m_AACDecInfo->currBlockID = AAC_ID_INVALID;
    m_AACDecInfo->currInstTag = -1;

    /* fill in user-accessible data */
    m_AACDecInfo->bitRate = 0;
    m_AACDecInfo->nChans = m_PSInfoBase->nChans;
    m_AACDecInfo->sampRate = sampRateTab[m_PSInfoBase->sampRateIdx];
    m_AACDecInfo->profile = m_pce[0]->profile;
    m_AACDecInfo->sbrEnabled = 0;

    /* update bitstream reader */
    bitsUsed = CalcBitsUsed(*buf, *bitOffset);
    *buf += (bitsUsed + *bitOffset) >> 3;
    *bitOffset = (bitsUsed + *bitOffset) & 0x07;
    *bitsAvail -= bitsUsed;
    if (*bitsAvail < 0)
        return ERR_AAC_INDATA_UNDERFLOW;

    return ERR_AAC_NONE;
}

/***********************************************************************************************************************
* Function:    SetRawBlockParams
*
* Description: set internal state variables for decoding a stream of raw data blocks
*
* Inputs:      flag indicating source of parameters (from previous headers or passed
*                explicitly by caller)
*              number of channels
*              sample rate
*              profile ID
*
* Outputs:     updated state variables in aacDecInfo
*
* Return:      0 if successful, error code (< 0) if error
*
* Notes:       if copyLast == 1, then m_PSInfoBase->nChans, m_PSInfoBase->sampRateIdx, and
*                aacDecInfo->profile are not changed (it's assumed that we already
*                set them, such as by a previous call to UnpackADTSHeader())
*              if copyLast == 0, then the parameters we passed in are used instead
***********************************************************************************************************************/
int32_t AACDecoder::SetRawBlockParams(int32_t copyLast, int32_t nChans, int32_t sampRate, int32_t profile) {
    int32_t idx;

    if (!copyLast) {
        m_AACDecInfo->profile = profile;
        m_PSInfoBase->nChans = nChans;
        for (idx = 0; idx < NUM_SAMPLE_RATES; idx++) {
            if (sampRate == sampRateTab[idx]) {
                m_PSInfoBase->sampRateIdx = idx;
                break;
            }
        }
        if (idx == NUM_SAMPLE_RATES)
            return ERR_AAC_INVALID_FRAME;
    }
    m_AACDecInfo->nChans = m_PSInfoBase->nChans;
    m_AACDecInfo->sampRate = sampRateTab[m_PSInfoBase->sampRateIdx];

    /* check validity of header */
    if (m_PSInfoBase->sampRateIdx >= NUM_SAMPLE_RATES || m_PSInfoBase->sampRateIdx < 0 ||
        m_AACDecInfo->profile != AAC_PROFILE_LC)
        return ERR_AAC_RAWBLOCK_PARAMS;

    return ERR_AAC_NONE;
}

/***********************************************************************************************************************
* Function:    PrepareRawBlock
*
* Description: reset per-block state variables for raw blocks (no ADTS/ADIF headers)
*
* Inputs:      none
*
* Outputs:     updated state variables in aacDecInfo
*
* Return:      0 if successful, error code (< 0) if error
***********************************************************************************************************************/
int32_t AACDecoder::PrepareRawBlock() {
    /* syntactic element fields will be read from bitstream for each element */
    m_AACDecInfo->prevBlockID = AAC_ID_INVALID;
    m_AACDecInfo->currBlockID = AAC_ID_INVALID;
    m_AACDecInfo->currInstTag = -1;

    /* fill in user-accessible data */
    m_AACDecInfo->bitRate = 0;
    m_AACDecInfo->sbrEnabled = 0;

    return ERR_AAC_NONE;
}

/***********************************************************************************************************************
 * Function:    DequantBlock
 *
 * Description: dequantize one block of transform coefficients (in-place)
 *
 * Inputs:      quantized transform coefficients, range = [0, 8191]
 *              number of samples to dequantize
 *              scalefactor for this block of data, range = [0, 256]
 *
 * Outputs:     dequantized transform coefficients in Q(FBITS_OUT_DQ_OFF)
 *
 * Return:      guard bit mask (OR of abs value of all dequantized coefs)
 *
 * Notes:       applies dequant formula y = pow(x, 4.0/3.0) * pow(2, (scale - 100)/4.0)
 *                * pow(2, FBITS_OUT_DQ_OFF)
 *              clips outputs to Q(FBITS_OUT_DQ_OFF)
 *              output has no minimum number of guard bits
 **********************************************************************************************************************/
int32_t AACDecoder::DequantBlock(int32_t *inbuf, int32_t nSamps, int32_t scale) {
    int32_t iSamp, scalef, scalei, x, y, gbMask, shift, tab4[4];
    const uint32_t *tab16, *coef;

    if (nSamps <= 0)
        return 0;

    scale -= SF_OFFSET; /* new range = [-100, 156] */

    /* with two's complement numbers, scalei/scalef factorization works for pos and neg values of scale:
     *  [+4...+7] >> 2 = +1, [ 0...+3] >> 2 = 0, [-4...-1] >> 2 = -1, [-8...-5] >> 2 = -2 ...
     *  (-1 & 0x3) = 3, (-2 & 0x3) = 2, (-3 & 0x3) = 1, (0 & 0x3) = 0
     *
     * Example: 2^(-5/4) = 2^(-1) * 2^(-1/4) = 2^-2 * 2^(3/4)
     */
    tab16 = pow43_14[scale & 0x3];
    scalef = pow14[scale & 0x3];
    scalei = (scale >> 2) + FBITS_OUT_DQ_OFF;

    /* cache first 4 values:
     * tab16[j] = Q28 for j = [0,3]
     * tab4[x] = x^(4.0/3.0) * 2^(0.25*scale), Q(FBITS_OUT_DQ_OFF)
     */
    shift = 28 - scalei;
    if (shift > 31) {
        tab4[0] = tab4[1] = tab4[2] = tab4[3] = 0;
    } else if (shift <= 0) {
        shift = -shift;
        if (shift > 31)
            shift = 31;
        for (x = 0; x < 4; x++) {
            y = tab16[x];
            if (y > (0x7fffffff >> shift))
                y = 0x7fffffff; /* clip (rare) */
            else
                y <<= shift;
            tab4[x] = y;
        }
    } else {
        tab4[0] = 0;
        tab4[1] = tab16[1] >> shift;
        tab4[2] = tab16[2] >> shift;
        tab4[3] = tab16[3] >> shift;
    }

    gbMask = 0;
    do {
        iSamp = *inbuf;
        x = FASTABS(iSamp);

        if (x < 4) {
            y = tab4[x];
        } else {
            if (x < 16) {
                /* result: y = Q25 (tab16 = Q25) */
                y = tab16[x];
                shift = 25 - scalei;
            } else if (x < 64) {
                /* result: y = Q21 (pow43tab[j] = Q23, scalef = Q30) */
                y = pow43[x - 16];
                shift = 21 - scalei;
                y = MULSHIFT32(y, scalef);
            } else {
                /* normalize to [0x40000000, 0x7fffffff]
                 * input x = [64, 8191] = [64, 2^13-1]
                 * ranges:
                 *  shift = 7:   64 -  127
                 *  shift = 6:  128 -  255
                 *  shift = 5:  256 -  511
                 *  shift = 4:  512 - 1023
                 *  shift = 3: 1024 - 2047
                 *  shift = 2: 2048 - 4095
                 *  shift = 1: 4096 - 8191
                 */
                x <<= 17;
                shift = 0;
                if (x < 0x08000000)
                    x <<= 4, shift += 4;
                if (x < 0x20000000)
                    x <<= 2, shift += 2;
                if (x < 0x40000000)
                    x <<= 1, shift += 1;

                coef = (x < SQRTHALF) ? poly43lo : poly43hi;

                /* polynomial */
                y = coef[0];
                y = MULSHIFT32(y, x) + coef[1];
                y = MULSHIFT32(y, x) + coef[2];
                y = MULSHIFT32(y, x) + coef[3];
                y = MULSHIFT32(y, x) + coef[4];
                y = MULSHIFT32(y, pow2frac[shift]) << 3;

                /* fractional scale
                 * result: y = Q21 (pow43tab[j] = Q23, scalef = Q30)
                 */
                y = MULSHIFT32(y, scalef); /* now y is Q24 */
                shift = 24 - scalei - pow2exp[shift];
            }

            /* integer scale */
            if (shift <= 0) {
                shift = -shift;
                if (shift > 31)
                    shift = 31;

                if (y > (0x7fffffff >> shift))
                    y = 0x7fffffff; /* clip (rare) */
                else
                    y <<= shift;
            } else {
                if (shift > 31)
                    shift = 31;
                y >>= shift;
            }
        }

        /* sign and store (gbMask used to count GB's) */
        gbMask |= y;

        /* apply sign */
        iSamp >>= 31;
        y ^= iSamp;
        y -= iSamp;

        *inbuf++ = y;
    } while (--nSamps);

    return gbMask;
}

/***********************************************************************************************************************
 * Function:    AACDequantize
 *
 * Description: dequantize all transform coefficients for one channel
 *
 * Inputs:      index of current channel
 *
 * Outputs:     dequantized coefficients, including short-block deinterleaving
 *              flags indicating if intensity and/or PNS is active
 *              minimum guard bit count for dequantized coefficients
 *
 * Return:      0 if successful, error code (< 0) if error
 **********************************************************************************************************************/
int32_t AACDecoder::AACDequantize(int32_t ch) {
    int32_t gp, cb, sfb, win, width, nSamps, gbMask;
    int32_t *coef;
    const uint16_t *sfbTab;
    uint8_t *sfbCodeBook;
    int16_t *scaleFactors;
    ICSInfo_t *icsInfo;

    icsInfo = (ch == 1 && m_PSInfoBase->commonWin == 1) ? &(m_PSInfoBase->icsInfo[0]) : &(m_PSInfoBase->icsInfo[ch]);

    if (icsInfo->winSequence == 2) {
        sfbTab = sfBandTabShort + sfBandTabShortOffset[m_PSInfoBase->sampRateIdx];
        nSamps = NSAMPS_SHORT;
    } else {
        sfbTab = sfBandTabLong + sfBandTabLongOffset[m_PSInfoBase->sampRateIdx];
        nSamps = NSAMPS_LONG;
    }
    coef = m_PSInfoBase->coef[ch];
    sfbCodeBook = m_PSInfoBase->sfbCodeBook[ch];
    scaleFactors = m_PSInfoBase->scaleFactors[ch];

    m_PSInfoBase->intensityUsed[ch] = 0;
    m_PSInfoBase->pnsUsed[ch] = 0;
    gbMask = 0;
    for (gp = 0; gp < icsInfo->numWinGroup; gp++) {
        for (win = 0; win < icsInfo->winGroupLen[gp]; win++) {
            for (sfb = 0; sfb < icsInfo->maxSFB; sfb++) {
                /* dequantize one scalefactor band (not necessary if codebook is intensity or PNS)
                 * for zero codebook, still run dequantizer in case non-zero pulse data was added
                 */
                cb = (int32_t)(sfbCodeBook[sfb]);
                width = sfbTab[sfb + 1] - sfbTab[sfb];
                if (cb >= 0 && cb <= 11)
                    gbMask |= DequantBlock(coef, width, scaleFactors[sfb]);
                else if (cb == 13)
                    m_PSInfoBase->pnsUsed[ch] = 1;
                else if (cb == 14 || cb == 15)
                    m_PSInfoBase->intensityUsed[ch] = 1; /* should only happen if ch == 1 */
                coef += width;
            }
            coef += (nSamps - sfbTab[icsInfo->maxSFB]);
        }
        sfbCodeBook += icsInfo->maxSFB;
        scaleFactors += icsInfo->maxSFB;
    }
    m_AACDecInfo->pnsUsed |= m_PSInfoBase->pnsUsed[ch]; /* set flag if PNS used for any channel */

    /* calculate number of guard bits in dequantized data */
    m_PSInfoBase->gbCurrent[ch] = CLZ(gbMask) - 1;

    return ERR_AAC_NONE;
}

/***********************************************************************************************************************
 * Function:    DeinterleaveShortBlocks
 *
 * Description: deinterleave transform coefficients in short blocks for one channel
 *
 * Inputs:      index of current channel
 *
 * Outputs:     deinterleaved coefficients (window groups into 8 separate windows)
 *
 * Return:      0 if successful, error code (< 0) if error
 *
 * Notes:       only necessary if deinterleaving not part of Huffman decoding
 **********************************************************************************************************************/
int32_t AACDecoder::DeinterleaveShortBlocks(int32_t ch) {
    //    (void)aacDecInfo;
    //    (void)ch;
    /* not used for this implementation - short block deinterleaving performed during Huffman decoding */
    return ERR_AAC_NONE;
}

/***********************************************************************************************************************
 * Function:    Get32BitVal
 *
 * Description: generate 32-bit unsigned random number
 *
 * Inputs:      last number calculated (seed, first time through)
 *
 * Outputs:     new number, saved in *last
 *
 * Return:      32-bit number, uniformly distributed between [0, 2^32)
 *
 * Notes:       uses simple linear congruential generator
 **********************************************************************************************************************/
uint32_t AACDecoder::Get32BitVal(uint32_t *last) {
    uint32_t r = *last;

    /* use same coefs as MPEG reference code (classic LCG)
     * use unsigned multiply to force reliable wraparound behavior in C (mod 2^32)
     */
    r = (1664525U * r) + 1013904223U;
    *last = r;

    return r;
}

/***********************************************************************************************************************
 * Function:    InvRootR
 *
 * Description: use Newton's method to solve for x = 1/sqrt(r)
 *
 * Inputs:      r in Q30 format, range = [0.25, 1] (normalize inputs to this range)
 *
 * Outputs:     none
 *
 * Return:      x = Q29, range = (1, 2)
 *
 * Notes:       guaranteed to converge and not overflow for any r in this range
 *
 *              xn+1  = xn - f(xn)/f'(xn)
 *              f(x)  = 1/sqrt(r) - x = 0 (find root)
 *                    = 1/x^2 - r
 *              f'(x) = -2/x^3
 *
 *              so xn+1 = xn/2 * (3 - r*xn^2)
 *
 *              NUM_ITER_INVSQRT = 3, maxDiff = 1.3747e-02
 *              NUM_ITER_INVSQRT = 4, maxDiff = 3.9832e-04
 **********************************************************************************************************************/
int32_t AACDecoder::InvRootR(int32_t r) {
    int32_t i, xn, t;

    /* use linear equation for initial guess
     * x0 = -2*r + 3 (so x0 always >= correct answer in range [0.25, 1))
     * xn = Q29 (at every step)
     */
    xn = (MULSHIFT32(r, X0_COEF_2) << 2) + X0_OFF_2;

    for (i = 0; i < NUM_ITER_INVSQRT; i++) {
        t = MULSHIFT32(xn, xn);              /* Q26 = Q29*Q29 */
        t = Q26_3 - (MULSHIFT32(r, t) << 2); /* Q26 = Q26 - (Q31*Q26 << 1) */
        xn = MULSHIFT32(xn, t) << (6 - 1);   /* Q29 = (Q29*Q26 << 6), and -1 for division by 2 */
    }

    /* clip to range (1.0, 2.0)
     * (because of rounding, this can converge to xn slightly > 2.0 when r is near 0.25)
     */
    if (xn >> 30)
        xn = (1 << 30) - 1;

    return xn;
}

/***********************************************************************************************************************
 * Function:    ScaleNoiseVector
 *
 * Description: apply scaling to vector of noise coefficients for one scalefactor band
 *
 * Inputs:      unscaled coefficients
 *              number of coefficients in vector (one scalefactor band of coefs)
 *              scalefactor for this band (i.e. noise energy)
 *
 * Outputs:     nVals coefficients in Q(FBITS_OUT_DQ_OFF)
 *
 * Return:      guard bit mask (OR of abs value of all noise coefs)
 **********************************************************************************************************************/
int32_t AACDecoder::ScaleNoiseVector(int32_t *coef, int32_t nVals, int32_t sf) {
    /* pow(2, i/4.0) for i = [0,1,2,3], format = Q30 */
    static const int32_t pow14[4] PROGMEM = {0x40000000, 0x4c1bf829, 0x5a82799a, 0x6ba27e65};

    int32_t i, c, spec, energy, sq, scalef, scalei, invSqrtEnergy, z, gbMask;

    energy = 0;
    for (i = 0; i < nVals; i++) {
        spec = coef[i];

        /* max nVals = max SFB width = 96, so energy can gain < 2^7 bits in accumulation */
        sq = (spec * spec) >> 8; /* spec*spec range = (-2^30, 2^30) */
        energy += sq;
    }

    /* unless nVals == 1 (or the number generator is broken...), this should not happen */
    if (energy == 0)
        return 0; /* coef[i] must = 0 for i = [0, nVals-1], so gbMask = 0 */

    /* pow(2, sf/4) * pow(2, FBITS_OUT_DQ_OFF) */
    scalef = pow14[sf & 0x3];
    scalei = (sf >> 2) + FBITS_OUT_DQ_OFF;

    /* energy has implied factor of 2^-8 since we shifted the accumulator
     * normalize energy to range [0.25, 1.0), calculate 1/sqrt(1), and denormalize
     *   i.e. divide input by 2^(30-z) and convert to Q30
     *        output of 1/sqrt(i) now has extra factor of 2^((30-z)/2)
     *        for energy > 0, z is an even number between 0 and 28
     * final scaling of invSqrtEnergy:
     *  2^(15 - z/2) to compensate for implicit 2^(30-z) factor in input
     *  +4 to compensate for implicit 2^-8 factor in input
     */
    z = CLZ(energy) - 2;                   /* energy has at least 2 leading zeros (see acc loop) */
    z &= 0xfffffffe;                       /* force even */
    invSqrtEnergy = InvRootR(energy << z); /* energy << z must be in range [0x10000000, 0x40000000] */
    scalei -= (15 - z / 2 + 4);            /* nInt = 1/sqrt(energy) in Q29 */

    /* normalize for final scaling */
    z = CLZ(invSqrtEnergy) - 1;
    invSqrtEnergy <<= z;
    scalei -= (z - 3 - 2);                      /* -2 for scalef, z-3 for invSqrtEnergy */
    scalef = MULSHIFT32(scalef, invSqrtEnergy); /* scalef (input) = Q30, invSqrtEnergy = Q29 * 2^z */
    gbMask = 0;

    if (scalei < 0) {
        scalei = -scalei;
        if (scalei > 31)
            scalei = 31;
        for (i = 0; i < nVals; i++) {
            c = MULSHIFT32(coef[i], scalef) >> scalei;
            gbMask |= FASTABS(c);
            coef[i] = c;
        }
    } else {
        /* for scalei <= 16, no clipping possible (coef[i] is < 2^15 before scaling)
         * for scalei > 16, just saturate exponent (rare)
         *   scalef is close to full-scale (since we normalized invSqrtEnergy)
         * remember, we are just producing noise here
         */
        if (scalei > 16)
            scalei = 16;
        for (i = 0; i < nVals; i++) {
            c = MULSHIFT32(coef[i] << scalei, scalef);
            coef[i] = c;
            gbMask |= FASTABS(c);
        }
    }

    return gbMask;
}

/***********************************************************************************************************************
 * Function:    GenerateNoiseVector
 *
 * Description: create vector of noise coefficients for one scalefactor band
 *
 * Inputs:      seed for number generator
 *              number of coefficients to generate
 *
 * Outputs:     buffer of nVals coefficients, range = [-2^15, 2^15)
 *              updated seed for number generator
 *
 * Return:      none
 **********************************************************************************************************************/
void AACDecoder::GenerateNoiseVector(int32_t *coef, int32_t *last, int32_t nVals) {
    int32_t i;

    for (i = 0; i < nVals; i++)
        coef[i] = ((int32_t)Get32BitVal((uint32_t *)last)) >> 16;
}

/***********************************************************************************************************************
 * Function:    CopyNoiseVector
 *
 * Description: copy vector of noise coefficients for one scalefactor band from L to R
 *
 * Inputs:      buffer of left coefficients
 *              number of coefficients to copy
 *
 * Outputs:     buffer of right coefficients
 *
 * Return:      none
 **********************************************************************************************************************/
void AACDecoder::CopyNoiseVector(int32_t *coefL, int32_t *coefR, int32_t nVals) {
    int32_t i;

    for (i = 0; i < nVals; i++)
        coefR[i] = coefL[i];
}

/***********************************************************************************************************************
 * Function:    PNS
 *
 * Description: apply perceptual noise substitution, if enabled (MPEG-4 only)
 *
 * Inputs:      index of current channel
 *
 * Outputs:     shaped noise in scalefactor bands where PNS is active
 *              updated minimum guard bit count for this channel
 *
 * Return:      0 if successful, -1 if error
 **********************************************************************************************************************/
int32_t AACDecoder::PNS(int32_t ch) {
    int32_t gp, sfb, win, width, nSamps, gb, gbMask;
    int32_t *coef;
    const uint16_t *sfbTab;
    uint8_t *sfbCodeBook;
    int16_t *scaleFactors;
    int32_t msMaskOffset, checkCorr, genNew;
    uint8_t msMask;
    uint8_t *msMaskPtr;
    ICSInfo_t *icsInfo;

    icsInfo = (ch == 1 && m_PSInfoBase->commonWin == 1) ? &(m_PSInfoBase->icsInfo[0]) : &(m_PSInfoBase->icsInfo[ch]);

    if (!m_PSInfoBase->pnsUsed[ch])
        return 0;

    if (icsInfo->winSequence == 2) {
        sfbTab = sfBandTabShort + sfBandTabShortOffset[m_PSInfoBase->sampRateIdx];
        nSamps = NSAMPS_SHORT;
    } else {
        sfbTab = sfBandTabLong + sfBandTabLongOffset[m_PSInfoBase->sampRateIdx];
        nSamps = NSAMPS_LONG;
    }
    coef = m_PSInfoBase->coef[ch];
    sfbCodeBook = m_PSInfoBase->sfbCodeBook[ch];
    scaleFactors = m_PSInfoBase->scaleFactors[ch];
    checkCorr = (m_AACDecInfo->currBlockID == AAC_ID_CPE && m_PSInfoBase->commonWin == 1 ? 1 : 0);

    gbMask = 0;
    for (gp = 0; gp < icsInfo->numWinGroup; gp++) {
        for (win = 0; win < icsInfo->winGroupLen[gp]; win++) {
            msMaskPtr = m_PSInfoBase->msMaskBits + ((gp * icsInfo->maxSFB) >> 3);
            msMaskOffset = ((gp * icsInfo->maxSFB) & 0x07);
            msMask = (*msMaskPtr++) >> msMaskOffset;

            for (sfb = 0; sfb < icsInfo->maxSFB; sfb++) {
                width = sfbTab[sfb + 1] - sfbTab[sfb];
                if (sfbCodeBook[sfb] == 13) {
                    if (ch == 0) {
                        /* generate new vector, copy into ch 1 if it's possible that the channels will be correlated
                         * if ch 1 has PNS enabled for this SFB but it's uncorrelated (i.e. ms_used == 0),
                         *    the copied values will be overwritten when we process ch 1
                         */
                        GenerateNoiseVector(coef, &m_PSInfoBase->pnsLastVal, width);
                        if (checkCorr && m_PSInfoBase->sfbCodeBook[1][gp * icsInfo->maxSFB + sfb] == 13)
                            CopyNoiseVector(coef, m_PSInfoBase->coef[1] + (coef - m_PSInfoBase->coef[0]), width);
                    } else {
                        /* generate new vector if no correlation between channels */
                        genNew = 1;
                        if (checkCorr && m_PSInfoBase->sfbCodeBook[0][gp * icsInfo->maxSFB + sfb] == 13) {
                            if ((m_PSInfoBase->msMaskPresent == 1 && (msMask & 0x01)) ||
                                m_PSInfoBase->msMaskPresent == 2)
                                genNew = 0;
                        }
                        if (genNew)
                            GenerateNoiseVector(coef, &m_PSInfoBase->pnsLastVal, width);
                    }
                    gbMask |= ScaleNoiseVector(coef, width, m_PSInfoBase->scaleFactors[ch][gp * icsInfo->maxSFB + sfb]);
                }
                coef += width;

                /* get next mask bit (should be branchless on ARM) */
                msMask >>= 1;
                if (++msMaskOffset == 8) {
                    msMask = *msMaskPtr++;
                    msMaskOffset = 0;
                }
            }
            coef += (nSamps - sfbTab[icsInfo->maxSFB]);
        }
        sfbCodeBook += icsInfo->maxSFB;
        scaleFactors += icsInfo->maxSFB;
    }

    /* update guard bit count if necessary */
    gb = CLZ(gbMask) - 1;
    if (m_PSInfoBase->gbCurrent[ch] > gb)
        m_PSInfoBase->gbCurrent[ch] = gb;

    return 0;
}

/***********************************************************************************************************************
 * Function:    GetSampRateIdx
 *
 * Description: get index of given sample rate
 *
 * Inputs:      sample rate (in Hz)
 *
 * Outputs:     none
 *
 * Return:      index of sample rate (table 1.15 in 14496-3:2001(E))
 *              -1 if sample rate not found in table
 **********************************************************************************************************************/
int32_t AACDecoder::GetSampRateIdx(int32_t sampRate) {
    int32_t idx;

    for (idx = 0; idx < NUM_SAMPLE_RATES; idx++) {
        if (sampRate == sampRateTab[idx])
            return idx;
    }

    return -1;
}

/***********************************************************************************************************************
 * Function:    StereoProcessGroup
 *
 * Description: apply mid-side and intensity stereo to group of transform coefficients
 *
 * Inputs:      dequantized transform coefficients for both channels
 *              pointer to appropriate scalefactor band table
 *              mid-side mask enabled flag
 *              buffer with mid-side mask (one bit for each scalefactor band)
 *              bit offset into mid-side mask buffer
 *              max coded scalefactor band
 *              buffer of codebook indices for right channel
 *              buffer of scalefactors for right channel, range = [0, 256]
 *
 * Outputs:     updated transform coefficients in Q(FBITS_OUT_DQ_OFF)
 *              updated minimum guard bit count for both channels
 *
 * Return:      none
 *
 * Notes:       assume no guard bits in input
 *              gains 0 int32_t bits
 **********************************************************************************************************************/
void AACDecoder::StereoProcessGroup(int32_t *coefL, int32_t *coefR, const uint16_t *sfbTab, int32_t msMaskPres,
                                    uint8_t *msMaskPtr, int32_t msMaskOffset, int32_t maxSFB, uint8_t *cbRight,
                                    int16_t *sfRight, int32_t *gbCurrent) {
    //fb
    static const uint32_t pow14[2][4] PROGMEM = {{0xc0000000, 0xb3e407d7, 0xa57d8666, 0x945d819b},
                                                 {0x40000000, 0x4c1bf829, 0x5a82799a, 0x6ba27e65}};

    int32_t sfb, width, cbIdx, sf, cl, cr, scalef, scalei;
    int32_t gbMaskL, gbMaskR;
    uint8_t msMask;

    msMask = (*msMaskPtr++) >> msMaskOffset;
    gbMaskL = 0;
    gbMaskR = 0;

    for (sfb = 0; sfb < maxSFB; sfb++) {
        width = sfbTab[sfb + 1] - sfbTab[sfb]; /* assume >= 0 (see sfBandTabLong/sfBandTabShort) */
        cbIdx = cbRight[sfb];

        if (cbIdx == 14 || cbIdx == 15) {
            /* intensity stereo */
            if (msMaskPres == 1 && (msMask & 0x01))
                cbIdx ^= 0x01;  /* invert_intensity(): 14 becomes 15, or 15 becomes 14 */
            sf = -sfRight[sfb]; /* negative since we use identity 0.5^(x) = 2^(-x) (see spec) */
            cbIdx &= 0x01;      /* choose - or + scale factor */
            scalef = pow14[cbIdx][sf & 0x03];
            scalei = (sf >> 2) + 2; /* +2 to compensate for scalef = Q30 */

            if (scalei > 0) {
                if (scalei > 30)
                    scalei = 30;
                do {
                    cr = MULSHIFT32(*coefL++, scalef);
                    {
                        int32_t sign = (cr) >> 31;
                        if (sign != (cr) >> (31 - scalei)) {
                            (cr) = sign ^ ((1 << (31 - scalei)) - 1);
                        }
                    }
                    cr <<= scalei;
                    gbMaskR |= FASTABS(cr);
                    *coefR++ = cr;
                } while (--width);
            } else {
                scalei = -scalei;
                if (scalei > 31)
                    scalei = 31;
                do {
                    cr = MULSHIFT32(*coefL++, scalef) >> scalei;
                    gbMaskR |= FASTABS(cr);
                    *coefR++ = cr;
                } while (--width);
            }
        } else if (cbIdx != 13 && ((msMaskPres == 1 && (msMask & 0x01)) || msMaskPres == 2)) {
            /* mid-side stereo (assumes no GB in inputs) */
            do {
                cl = *coefL;
                cr = *coefR;

                if ((FASTABS(cl) | FASTABS(cr)) >> 30) {
                    /* avoid overflow (rare) */
                    cl >>= 1;
                    sf = cl + (cr >> 1);
                    {
                        int32_t sign = (sf) >> 31;
                        if (sign != (sf) >> (30)) {
                            (sf) = sign ^ ((1 << (30)) - 1);
                        }
                    }
                    sf <<= 1;
                    cl = cl - (cr >> 1);
                    {
                        int32_t sign = (cl) >> 31;
                        if (sign != (cl) >> (30)) {
                            (cl) = sign ^ ((1 << (30)) - 1);
                        }
                    }
                    cl <<= 1;
                } else {
                    /* usual case */
                    sf = cl + cr;
                    cl -= cr;
                }

                *coefL++ = sf;
                gbMaskL |= FASTABS(sf);
                *coefR++ = cl;
                gbMaskR |= FASTABS(cl);
            } while (--width);

        } else {
            /* nothing to do */
            coefL += width;
            coefR += width;
        }

        /* get next mask bit (should be branchless on ARM) */
        msMask >>= 1;
        if (++msMaskOffset == 8) {
            msMask = *msMaskPtr++;
            msMaskOffset = 0;
        }
    }

    cl = CLZ(gbMaskL) - 1;
    if (gbCurrent[0] > cl)
        gbCurrent[0] = cl;

    cr = CLZ(gbMaskR) - 1;
    if (gbCurrent[1] > cr)
        gbCurrent[1] = cr;

    return;
}

/***********************************************************************************************************************
 * Function:    StereoProcess
 *
 * Description: apply mid-side and intensity stereo, if enabled
 *
 * Inputs:      none
 *
 * Outputs:     updated transform coefficients in Q(FBITS_OUT_DQ_OFF)
 *              updated minimum guard bit count for both channels
 *
 * Return:      0 if successful, -1 if error
 **********************************************************************************************************************/
int32_t AACDecoder::StereoProcess() {
    ICSInfo_t *icsInfo;
    int32_t gp, win, nSamps, msMaskOffset;
    int32_t *coefL, *coefR;
    uint8_t *msMaskPtr;
    const uint16_t *sfbTab;

    /* mid-side and intensity stereo require common_window == 1 (see MPEG4 spec, Correction 2, 2004) */
    if (m_PSInfoBase->commonWin != 1 || m_AACDecInfo->currBlockID != AAC_ID_CPE)
        return 0;

    /* nothing to do */
    if (!m_PSInfoBase->msMaskPresent && !m_PSInfoBase->intensityUsed[1])
        return 0;

    icsInfo = &(m_PSInfoBase->icsInfo[0]);
    if (icsInfo->winSequence == 2) {
        sfbTab = sfBandTabShort + sfBandTabShortOffset[m_PSInfoBase->sampRateIdx];
        nSamps = NSAMPS_SHORT;
    } else {
        sfbTab = sfBandTabLong + sfBandTabLongOffset[m_PSInfoBase->sampRateIdx];
        nSamps = NSAMPS_LONG;
    }
    coefL = m_PSInfoBase->coef[0];
    coefR = m_PSInfoBase->coef[1];

    /* do fused mid-side/intensity processing for each block (one long or eight short) */
    msMaskOffset = 0;
    msMaskPtr = m_PSInfoBase->msMaskBits;
    for (gp = 0; gp < icsInfo->numWinGroup; gp++) {
        for (win = 0; win < icsInfo->winGroupLen[gp]; win++) {
            StereoProcessGroup(coefL, coefR, sfbTab, m_PSInfoBase->msMaskPresent, msMaskPtr, msMaskOffset,
                               icsInfo->maxSFB, m_PSInfoBase->sfbCodeBook[1] + gp * icsInfo->maxSFB,
                               m_PSInfoBase->scaleFactors[1] + gp * icsInfo->maxSFB, m_PSInfoBase->gbCurrent);
            coefL += nSamps;
            coefR += nSamps;
        }
        /* we use one bit per sfb, so there are maxSFB bits for each window group */
        msMaskPtr += (msMaskOffset + icsInfo->maxSFB) >> 3;
        msMaskOffset = (msMaskOffset + icsInfo->maxSFB) & 0x07;
    }

    ASSERT(coefL == m_PSInfoBase->coef[0] + 1024);
    ASSERT(coefR == m_PSInfoBase->coef[1] + 1024);

    return 0;
}

/***********************************************************************************************************************
 * Function:    RatioPowInv
 *
 * Description: use Taylor (MacLaurin) series expansion to calculate (a/b) ^ (1/c)
 *
 * Inputs:      a = [1, 64], b = [1, 64], c = [1, 64], a >= b
 *
 * Outputs:     none
 *
 * Return:      y = Q24, range ~= [0.015625, 64]
 **********************************************************************************************************************/
int32_t AACDecoder::RatioPowInv(int32_t a, int32_t b, int32_t c) {
    int32_t lna, lnb, i, p, t, y;

    if (a < 1 || b < 1 || c < 1 || a > 64 || b > 64 || c > 64 || a < b)
        return 0;

    lna = MULSHIFT32(log2Tab[a], LOG2_EXP_INV) << 1; /* ln(a), Q28 */
    lnb = MULSHIFT32(log2Tab[b], LOG2_EXP_INV) << 1; /* ln(b), Q28 */
    p = (lna - lnb) / c;                             /* Q28 */

    /* sum in Q24 */
    y = (1 << 24);
    t = p >> 4; /* t = p^1 * 1/1! (Q24)*/
    y += t;

    for (i = 2; i <= NUM_TERMS_RPI; i++) {
        t = MULSHIFT32(invTab[i - 1], t) << 2;
        t = MULSHIFT32(p, t) << 4; /* t = p^i * 1/i! (Q24) */
        y += t;
    }

    return y;
}

/***********************************************************************************************************************
 * Function:    SqrtFix
 *
 * Description: use binary search to calculate sqrt(q)
 *
 * Inputs:      q = Q30
 *              number of fraction bits in input
 *
 * Outputs:     number of fraction bits in output
 *
 * Return:      lo = Q(fBitsOut)
 *
 * Notes:       absolute precision varies depending on fBitsIn
 *              normalizes input to range [0x200000000, 0x7fffffff] and takes
 *                floor(sqrt(input)), and sets fBitsOut appropriately
 **********************************************************************************************************************/
int32_t AACDecoder::SqrtFix(int32_t q, int32_t fBitsIn, int32_t *fBitsOut) {
    int32_t z, lo, hi, mid;

    if (q <= 0) {
        *fBitsOut = fBitsIn;
        return 0;
    }

    /* force even fBitsIn */
    z = fBitsIn & 0x01;
    q >>= z;
    fBitsIn -= z;

    /* for max precision, normalize to [0x20000000, 0x7fffffff] */
    z = (CLZ(q) - 1);
    z >>= 1;
    q <<= (2 * z);

    /* choose initial bounds */
    lo = 1;
    if (q >= 0x10000000)
        lo = 16384; /* (int32_t)sqrt(0x10000000) */
    hi = 46340;     /* (int32_t)sqrt(0x7fffffff) */

    /* do binary search with 32x32->32 multiply test */
    do {
        mid = (lo + hi) >> 1;
        if (mid * mid > q)
            hi = mid - 1;
        else
            lo = mid + 1;
    } while (hi >= lo);
    lo--;

    *fBitsOut = ((fBitsIn + 2 * z) >> 1);
    return lo;
}

/***********************************************************************************************************************
 * Function:    InvRNormalized
 *
 * Description: use Newton's method to solve for x = 1/r
 *
 * Inputs:      r = Q31, range = [0.5, 1) (normalize your inputs to this range)
 *
 * Outputs:     none
 *
 * Return:      x = Q29, range ~= [1.0, 2.0]
 *
 * Notes:       guaranteed to converge and not overflow for any r in [0.5, 1)
 *
 *              xn+1  = xn - f(xn)/f'(xn)
 *              f(x)  = 1/r - x = 0 (find root)
 *                    = 1/x - r
 *              f'(x) = -1/x^2
 *
 *              so xn+1 = xn - (1/xn - r) / (-1/xn^2)
 *                      = xn * (2 - r*xn)
 *
 *              NUM_ITER_IRN = 2, maxDiff = 6.2500e-02 (precision of about 4 bits)
 *              NUM_ITER_IRN = 3, maxDiff = 3.9063e-03 (precision of about 8 bits)
 *              NUM_ITER_IRN = 4, maxDiff = 1.5288e-05 (precision of about 16 bits)
 *              NUM_ITER_IRN = 5, maxDiff = 3.0034e-08 (precision of about 24 bits)
 **********************************************************************************************************************/
int32_t AACDecoder::InvRNormalized(int32_t r) {
    int32_t i, xn, t;

    /* r =   [0.5, 1.0)
     * 1/r = (1.0, 2.0]
     *   so use 1.5 as initial guess
     */
    xn = Q28_15;

    /* xn = xn*(2.0 - r*xn) */
    for (i = NUM_ITER_IRN; i != 0; i--) {
        t = MULSHIFT32(r, xn);       /* Q31*Q29 = Q28 */
        t = Q28_2 - t;               /* Q28 */
        xn = MULSHIFT32(xn, t) << 4; /* Q29*Q28 << 4 = Q29 */
    }

    return xn;
}

/***********************************************************************************************************************
 * Function:    BitReverse32
 *
 * Description: Ken's fast in-place bit reverse
 *
 * Inputs:      buffer of 32 complex samples
 *
 * Outputs:     bit-reversed samples in same buffer
 *
 * Return:      none
***********************************************************************************************************************/
void AACDecoder::BitReverse32(int32_t *inout) {
    int32_t t;
    t = inout[2];
    inout[2] = inout[32];
    inout[32] = t;
    t = inout[3];
    inout[3] = inout[33];
    inout[33] = t;

    t = inout[4];
    inout[4] = inout[16];
    inout[16] = t;
    t = inout[5];
    inout[5] = inout[17];
    inout[17] = t;

    t = inout[6];
    inout[6] = inout[48];
    inout[48] = t;
    t = inout[7];
    inout[7] = inout[49];
    inout[49] = t;

    t = inout[10];
    inout[10] = inout[40];
    inout[40] = t;
    t = inout[11];
    inout[11] = inout[41];
    inout[41] = t;

    t = inout[12];
    inout[12] = inout[24];
    inout[24] = t;
    t = inout[13];
    inout[13] = inout[25];
    inout[25] = t;

    t = inout[14];
    inout[14] = inout[56];
    inout[56] = t;
    t = inout[15];
    inout[15] = inout[57];
    inout[57] = t;

    t = inout[18];
    inout[18] = inout[36];
    inout[36] = t;
    t = inout[19];
    inout[19] = inout[37];
    inout[37] = t;

    t = inout[22];
    inout[22] = inout[52];
    inout[52] = t;
    t = inout[23];
    inout[23] = inout[53];
    inout[53] = t;

    t = inout[26];
    inout[26] = inout[44];
    inout[44] = t;
    t = inout[27];
    inout[27] = inout[45];
    inout[45] = t;

    t = inout[30];
    inout[30] = inout[60];
    inout[60] = t;
    t = inout[31];
    inout[31] = inout[61];
    inout[61] = t;

    t = inout[38];
    inout[38] = inout[50];
    inout[50] = t;
    t = inout[39];
    inout[39] = inout[51];
    inout[51] = t;

    t = inout[46];
    inout[46] = inout[58];
    inout[58] = t;
    t = inout[47];
    inout[47] = inout[59];
    inout[59] = t;
}

/***********************************************************************************************************************
 * Function:    R8FirstPass32
 *
 * Description: radix-8 trivial pass for decimation-in-time FFT (log2(N) = 5)
 *
 * Inputs:      buffer of (bit-reversed) samples
 *
 * Outputs:     processed samples in same buffer
 *
 * Return:      none
 *
 * Notes:       assumes 3 guard bits, gains 1 integer bit
 *              guard bits out = guard bits in - 3 (if inputs are full scale)
 *                or guard bits in - 2 (if inputs bounded to +/- sqrt(2)/2)
 *              see scaling comments in fft.c for base AAC
 *              should compile with no stack spills on ARM (verify compiled output)
 *              current instruction count (per pass): 16 LDR, 16 STR, 4 SMULL, 61 ALU
 **********************************************************************************************************************/
void AACDecoder::R8FirstPass32(int32_t *r0) {
    int32_t r1, r2, r3, r4, r5, r6, r7;
    int32_t r8, r9, r10, r11, r12, r14;

    /* number of passes = fft size / 8 = 32 / 8 = 4 */
    r1 = (32 >> 3);
    do {
        r2 = r0[8];
        r3 = r0[9];
        r4 = r0[10];
        r5 = r0[11];
        r6 = r0[12];
        r7 = r0[13];
        r8 = r0[14];
        r9 = r0[15];

        r10 = r2 + r4;
        r11 = r3 + r5;
        r12 = r6 + r8;
        r14 = r7 + r9;

        r2 -= r4;
        r3 -= r5;
        r6 -= r8;
        r7 -= r9;

        r4 = r2 - r7;
        r5 = r2 + r7;
        r8 = r3 - r6;
        r9 = r3 + r6;

        r2 = r4 - r9;
        r3 = r4 + r9;
        r6 = r5 - r8;
        r7 = r5 + r8;

        r2 = MULSHIFT32(SQRTHALF, r2); /* can use r4, r5, r8, or r9 for constant and lo32 scratch reg */
        r3 = MULSHIFT32(SQRTHALF, r3);
        r6 = MULSHIFT32(SQRTHALF, r6);
        r7 = MULSHIFT32(SQRTHALF, r7);

        r4 = r10 + r12;
        r5 = r10 - r12;
        r8 = r11 + r14;
        r9 = r11 - r14;

        r10 = r0[0];
        r11 = r0[2];
        r12 = r0[4];
        r14 = r0[6];

        r10 += r11;
        r12 += r14;

        r4 >>= 1;
        r10 += r12;
        r4 += (r10 >> 1);
        r0[0] = r4;
        r4 -= (r10 >> 1);
        r4 = (r10 >> 1) - r4;
        r0[8] = r4;

        r9 >>= 1;
        r10 -= 2 * r12;
        r4 = (r10 >> 1) + r9;
        r0[4] = r4;
        r4 = (r10 >> 1) - r9;
        r0[12] = r4;
        r10 += r12;

        r10 -= 2 * r11;
        r12 -= 2 * r14;

        r4 = r0[1];
        r9 = r0[3];
        r11 = r0[5];
        r14 = r0[7];

        r4 += r9;
        r11 += r14;

        r8 >>= 1;
        r4 += r11;
        r8 += (r4 >> 1);
        r0[1] = r8;
        r8 -= (r4 >> 1);
        r8 = (r4 >> 1) - r8;
        r0[9] = r8;

        r5 >>= 1;
        r4 -= 2 * r11;
        r8 = (r4 >> 1) - r5;
        r0[5] = r8;
        r8 = (r4 >> 1) + r5;
        r0[13] = r8;
        r4 += r11;

        r4 -= 2 * r9;
        r11 -= 2 * r14;

        r9 = r10 - r11;
        r10 += r11;
        r14 = r4 + r12;
        r4 -= r12;

        r5 = (r10 >> 1) + r7;
        r8 = (r4 >> 1) - r6;
        r0[2] = r5;
        r0[3] = r8;

        r5 = (r9 >> 1) - r2;
        r8 = (r14 >> 1) - r3;
        r0[6] = r5;
        r0[7] = r8;

        r5 = (r10 >> 1) - r7;
        r8 = (r4 >> 1) + r6;
        r0[10] = r5;
        r0[11] = r8;

        r5 = (r9 >> 1) + r2;
        r8 = (r14 >> 1) + r3;
        r0[14] = r5;
        r0[15] = r8;

        r0 += 16;
        r1--;
    } while (r1 != 0);
}

/***********************************************************************************************************************
 * Function:    R4Core32
 *
 * Description: radix-4 pass for 32-point decimation-in-time FFT
 *
 * Inputs:      buffer of samples
 *
 * Outputs:     processed samples in same buffer
 *
 * Return:      none
 *
 * Notes:       gain 2 integer bits
 *              guard bits out = guard bits in - 1 (if inputs are full scale)
 *              see scaling comments in fft.c for base AAC
 *              uses 3-mul, 3-add butterflies instead of 4-mul, 2-add
 *              should compile with no stack spills on ARM (verify compiled output)
 *              current instruction count (per pass): 16 LDR, 16 STR, 4 SMULL, 61 ALU
 **********************************************************************************************************************/
void AACDecoder::R4Core32(int32_t *r0) {
    int32_t r2, r3, r4, r5, r6, r7;
    int32_t r8, r9, r10, r12, r14;
    int32_t *r1;

    r1 = (int32_t *)twidTabOdd32;
    r10 = 8;
    do {
        /* can use r14 for lo32 scratch register in all MULSHIFT32 */
        r2 = r1[0];
        r3 = r1[1];
        r4 = r0[16];
        r5 = r0[17];
        r12 = r4 + r5;
        r12 = MULSHIFT32(r3, r12);
        r5 = MULSHIFT32(r2, r5) + r12;
        r2 += 2 * r3;
        r4 = MULSHIFT32(r2, r4) - r12;

        r2 = r1[2];
        r3 = r1[3];
        r6 = r0[32];
        r7 = r0[33];
        r12 = r6 + r7;
        r12 = MULSHIFT32(r3, r12);
        r7 = MULSHIFT32(r2, r7) + r12;
        r2 += 2 * r3;
        r6 = MULSHIFT32(r2, r6) - r12;

        r2 = r1[4];
        r3 = r1[5];
        r8 = r0[48];
        r9 = r0[49];
        r12 = r8 + r9;
        r12 = MULSHIFT32(r3, r12);
        r9 = MULSHIFT32(r2, r9) + r12;
        r2 += 2 * r3;
        r8 = MULSHIFT32(r2, r8) - r12;

        r2 = r0[0];
        r3 = r0[1];

        r12 = r6 + r8;
        r8 = r6 - r8;
        r14 = r9 - r7;
        r9 = r9 + r7;

        r6 = (r2 >> 2) - r4;
        r7 = (r3 >> 2) - r5;
        r4 += (r2 >> 2);
        r5 += (r3 >> 2);

        r2 = r4 + r12;
        r3 = r5 + r9;
        r0[0] = r2;
        r0[1] = r3;
        r2 = r6 - r14;
        r3 = r7 - r8;
        r0[16] = r2;
        r0[17] = r3;
        r2 = r4 - r12;
        r3 = r5 - r9;
        r0[32] = r2;
        r0[33] = r3;
        r2 = r6 + r14;
        r3 = r7 + r8;
        r0[48] = r2;
        r0[49] = r3;

        r0 += 2;
        r1 += 6;
        r10--;
    } while (r10 != 0);
}

/***********************************************************************************************************************
 * Function:    FFT32C
 *
 * Description: Ken's very fast in-place radix-4 decimation-in-time FFT
 *
 * Inputs:      buffer of 32 complex samples (before bit-reversal)
 *
 * Outputs:     processed samples in same buffer
 *
 * Return:      none
 *
 * Notes:       assumes 3 guard bits in, gains 3 integer bits
 *              guard bits out = guard bits in - 2
 *              (guard bit analysis includes assumptions about steps immediately
 *               before and after, i.e. PreMul and PostMul for DCT)
 **********************************************************************************************************************/
void AACDecoder::FFT32C(int32_t *x) {
    /* decimation in time */
    BitReverse32(x);

    /* 32-point complex FFT */
    R8FirstPass32(x); /* gain 1 int32_t bit,  lose 2 GB (making assumptions about input) */
    R4Core32(x);      /* gain 2 int32_t bits, lose 0 GB (making assumptions about input) */
}

/***********************************************************************************************************************
 * Function:    CVKernel1
 *
 * Description: kernel of covariance matrix calculation for p01, p11, p12, p22
 *
 * Inputs:      buffer of low-freq samples, starting at time index = 0,
 *                freq index = patch subband
 *
 * Outputs:     64-bit accumulators for p01re, p01im, p12re, p12im, p11re, p22re
 *                stored in accBuf
 *
 * Return:      none
 *
 * Notes:       this is carefully written to be efficient on ARM
 *              use the assembly code version in sbrcov.s when building for ARM!
 **********************************************************************************************************************/
void AACDecoder::CVKernel1(int32_t *XBuf, int32_t *accBuf) {
    U64 p01re, p01im, p12re, p12im, p11re, p22re;
    int32_t n, x0re, x0im, x1re, x1im;

    x0re = XBuf[0];
    x0im = XBuf[1];
    XBuf += (2 * 64);
    x1re = XBuf[0];
    x1im = XBuf[1];
    XBuf += (2 * 64);

    p01re.w64 = p01im.w64 = 0;
    p12re.w64 = p12im.w64 = 0;
    p11re.w64 = 0;
    p22re.w64 = 0;

    p12re.w64 = MADD64(p12re.w64, x1re, x0re);
    p12re.w64 = MADD64(p12re.w64, x1im, x0im);
    p12im.w64 = MADD64(p12im.w64, x0re, x1im);
    p12im.w64 = MADD64(p12im.w64, -x0im, x1re);
    p22re.w64 = MADD64(p22re.w64, x0re, x0re);
    p22re.w64 = MADD64(p22re.w64, x0im, x0im);
    for (n = (NUM_TIME_SLOTS * SAMPLES_PER_SLOT + 6); n != 0; n--) {
        /* 4 input, 3*2 acc, 1 ptr, 1 loop counter = 12 registers (use same for x0im, -x0im) */
        x0re = x1re;
        x0im = x1im;
        x1re = XBuf[0];
        x1im = XBuf[1];

        p01re.w64 = MADD64(p01re.w64, x1re, x0re);
        p01re.w64 = MADD64(p01re.w64, x1im, x0im);
        p01im.w64 = MADD64(p01im.w64, x0re, x1im);
        p01im.w64 = MADD64(p01im.w64, -x0im, x1re);
        p11re.w64 = MADD64(p11re.w64, x0re, x0re);
        p11re.w64 = MADD64(p11re.w64, x0im, x0im);

        XBuf += (2 * 64);
    }
    /* these can be derived by slight changes to account for boundary conditions */
    p12re.w64 += p01re.w64;
    p12re.w64 = MADD64(p12re.w64, x1re, -x0re);
    p12re.w64 = MADD64(p12re.w64, x1im, -x0im);
    p12im.w64 += p01im.w64;
    p12im.w64 = MADD64(p12im.w64, x0re, -x1im);
    p12im.w64 = MADD64(p12im.w64, x0im, x1re);
    p22re.w64 += p11re.w64;
    p22re.w64 = MADD64(p22re.w64, x0re, -x0re);
    p22re.w64 = MADD64(p22re.w64, x0im, -x0im);

    accBuf[0] = p01re.r.lo32;
    accBuf[1] = p01re.r.hi32;
    accBuf[2] = p01im.r.lo32;
    accBuf[3] = p01im.r.hi32;
    accBuf[4] = p11re.r.lo32;
    accBuf[5] = p11re.r.hi32;
    accBuf[6] = p12re.r.lo32;
    accBuf[7] = p12re.r.hi32;
    accBuf[8] = p12im.r.lo32;
    accBuf[9] = p12im.r.hi32;
    accBuf[10] = p22re.r.lo32;
    accBuf[11] = p22re.r.hi32;
}

/***********************************************************************************************************************
 * Function:    CVKernel2
 *
 * Description: kernel of covariance matrix calculation for p02
 *
 * Inputs:      buffer of low-freq samples, starting at time index = 0,
 *                freq index = patch subband
 *
 * Outputs:     64-bit accumulators for p02re, p02im stored in accBuf
 *
 * Return:      none
 *
 * Notes:       this is carefully written to be efficient on ARM
 *              use the assembly code version in sbrcov.s when building for ARM!
 **********************************************************************************************************************/
void AACDecoder::CVKernel2(int32_t *XBuf, int32_t *accBuf) {
    U64 p02re, p02im;
    int32_t n, x0re, x0im, x1re, x1im, x2re, x2im;

    p02re.w64 = p02im.w64 = 0;

    x0re = XBuf[0];
    x0im = XBuf[1];
    XBuf += (2 * 64);
    x1re = XBuf[0];
    x1im = XBuf[1];
    XBuf += (2 * 64);

    for (n = (NUM_TIME_SLOTS * SAMPLES_PER_SLOT + 6); n != 0; n--) {
        /* 6 input, 2*2 acc, 1 ptr, 1 loop counter = 12 registers (use same for x0im, -x0im) */
        x2re = XBuf[0];
        x2im = XBuf[1];

        p02re.w64 = MADD64(p02re.w64, x2re, x0re);
        p02re.w64 = MADD64(p02re.w64, x2im, x0im);
        p02im.w64 = MADD64(p02im.w64, x0re, x2im);
        p02im.w64 = MADD64(p02im.w64, -x0im, x2re);

        x0re = x1re;
        x0im = x1im;
        x1re = x2re;
        x1im = x2im;
        XBuf += (2 * 64);
    }

    accBuf[0] = p02re.r.lo32;
    accBuf[1] = p02re.r.hi32;
    accBuf[2] = p02im.r.lo32;
    accBuf[3] = p02im.r.hi32;
}

/***********************************************************************************************************************
 * Function:    SetBitstreamPointer
 *
 * Description: initialize bitstream reader
 *
 * Inputs:      number of bytes in bitstream
 *              pointer to byte-aligned buffer of data to read from
 *
 * Outputs:     initialized bitstream info struct
 *
 * Return:      none
 **********************************************************************************************************************/
void AACDecoder::SetBitstreamPointer(int32_t nBytes, uint8_t *buf) {
    /* init bitstream */
    m_aac_BitStreamInfo.bytePtr = buf;
    m_aac_BitStreamInfo.iCache = 0;     /* 4-byte uint32_t */
    m_aac_BitStreamInfo.cachedBits = 0; /* i.e. zero bits in cache */
    m_aac_BitStreamInfo.nBytes = nBytes;
}

/***********************************************************************************************************************
 * Function:    RefillBitstreamCache
 *
 * Description: read new data from bitstream buffer into 32-bit cache
 *
 * Inputs:      none
 *
 * Outputs:     updated bitstream info struct
 *
 * Return:      none
 *
 * Notes:       only call when iCache is completely drained (resets bitOffset to 0)
 *              always loads 4 new bytes except when bsi->nBytes < 4 (end of buffer)
 *              stores data as big-endian in cache, regardless of machine endian-ness
 **********************************************************************************************************************/

/*
inline void AACDecoder::RefillBitstreamCache() {
    int32_t nBytes = m_aac_BitStreamInfo.nBytes;
    if (nBytes >= 4) {
        // optimize for common case, independent of machine endian-ness
        m_aac_BitStreamInfo.iCache = (*m_aac_BitStreamInfo.bytePtr++) << 24;
        m_aac_BitStreamInfo.iCache |= (*m_aac_BitStreamInfo.bytePtr++) << 16;
        m_aac_BitStreamInfo.iCache |= (*m_aac_BitStreamInfo.bytePtr++) << 8;
        m_aac_BitStreamInfo.iCache |= (*m_aac_BitStreamInfo.bytePtr++);

        m_aac_BitStreamInfo.cachedBits = 32;
        m_aac_BitStreamInfo.nBytes -= 4;
    } else {
        m_aac_BitStreamInfo.iCache = 0;
        while (nBytes--) {
            m_aac_BitStreamInfo.iCache |= (*m_aac_BitStreamInfo.bytePtr++);
            m_aac_BitStreamInfo.iCache <<= 8;
        }
        m_aac_BitStreamInfo.iCache <<= ((3 - m_aac_BitStreamInfo.nBytes) * 8);
        m_aac_BitStreamInfo.cachedBits = 8 * m_aac_BitStreamInfo.nBytes;
        m_aac_BitStreamInfo.nBytes = 0;
    }
}
*/
//Optimized for REV16, REV32 (FB)
inline void AACDecoder::RefillBitstreamCache() {
    const int32_t nBytes = m_aac_BitStreamInfo.nBytes;
    uint8_t *ptr = m_aac_BitStreamInfo.bytePtr;

    if (nBytes >= 4) {
        // 32-bit aligned load + byte swap:
        // buffer:  12 34 56 78
        // iCache:  0x12345678
        m_aac_BitStreamInfo.iCache = REV32(*(const uint32_t *)ptr);
        m_aac_BitStreamInfo.bytePtr = ptr + 4;
        m_aac_BitStreamInfo.cachedBits = 32;
        m_aac_BitStreamInfo.nBytes = nBytes - 4;
        return;
    }

    uint32_t iCache = 0;

    switch (nBytes) {
    case 3:
        iCache = *ptr++;
        iCache <<= 8;
        [[fallthrough]];

    case 2:
        iCache |= *ptr++;
        iCache <<= 8;
        [[fallthrough]];

    case 1:
        iCache |= *ptr++;
        break;

    case 0:
        m_aac_BitStreamInfo.iCache = 0;
        m_aac_BitStreamInfo.cachedBits = 0;
        m_aac_BitStreamInfo.nBytes = 0;
        return;
    }

    m_aac_BitStreamInfo.iCache = iCache << ((3 - nBytes) * 8);
    m_aac_BitStreamInfo.bytePtr = ptr;
    m_aac_BitStreamInfo.cachedBits = nBytes * 8;
    m_aac_BitStreamInfo.nBytes = 0;
}

/***********************************************************************************************************************
 * Function:    GetBits
 *
 * Description: get bits from bitstream, advance bitstream pointer
 *
 * Inputs:      pointer to initialized aac_BitStreamInfo_t struct
 *              number of bits to get from bitstream
 *
 * Outputs:     updated bitstream info struct
 *
 * Return:      the next nBits bits of data from bitstream buffer
 *
 * Notes:       nBits must be in range [0, 31], nBits outside this range masked by 0x1f
 *              for speed, does not indicate error if you overrun bit buffer
 *              if nBits == 0, returns 0
 **********************************************************************************************************************/
uint32_t AACDecoder::GetBits(int32_t nBits) {
    uint32_t data, lowBits;

    nBits &= 0x1f; /* nBits mod 32 to avoid unpredictable results like >> by negative amount */
    data = m_aac_BitStreamInfo.iCache >> (31 - nBits); /* unsigned >> so zero-extend */
    data >>= 1;                                        /* do as >> 31, >> 1 so that nBits = 0 works okay (returns 0) */
    m_aac_BitStreamInfo.iCache <<= nBits;              /* left-justify cache */
    m_aac_BitStreamInfo.cachedBits -= nBits;           /* how many bits have we drawn from the cache so far */

    /* if we cross an int32_t boundary, refill the cache */
    if (m_aac_BitStreamInfo.cachedBits < 0) {
        lowBits = -m_aac_BitStreamInfo.cachedBits;
        RefillBitstreamCache();
        data |= m_aac_BitStreamInfo.iCache >> (32 - lowBits); /* get the low-order bits */

        m_aac_BitStreamInfo.cachedBits -= lowBits; /* how many bits have we drawn from the cache so far */
        m_aac_BitStreamInfo.iCache <<= lowBits;    /* left-justify cache */
    }

    return data;
}

/***********************************************************************************************************************
 * Function:    GetBitsNoAdvance
 *
 * Description: get bits from bitstream, do not advance bitstream pointer
 *
 * Inputs:      pointer to initialized aac_BitStreamInfo_t struct
 *              number of bits to get from bitstream
 *
 * Outputs:     none (state of aac_BitStreamInfo_t struct left unchanged)
 *
 * Return:      the next nBits bits of data from bitstream buffer
 *
 * Notes:       nBits must be in range [0, 31], nBits outside this range masked by 0x1f
 *              for speed, does not indicate error if you overrun bit buffer
 *              if nBits == 0, returns 0
 **********************************************************************************************************************/
uint32_t AACDecoder::GetBitsNoAdvance(int32_t nBits) {
    uint8_t *buf;
    uint32_t data, iCache;
    int32_t lowBits;

    nBits &= 0x1f; /* nBits mod 32 to avoid unpredictable results like >> by negative amount */
    data = m_aac_BitStreamInfo.iCache >> (31 - nBits); /* unsigned >> so zero-extend */
    data >>= 1;                                        /* do as >> 31, >> 1 so that nBits = 0 works okay (returns 0) */
    lowBits = nBits - m_aac_BitStreamInfo.cachedBits;  /* how many bits do we have left to read */

    /* if we cross an int32_t boundary, read next bytes in buffer */
    if (lowBits > 0) {
        iCache = 0;
        buf = m_aac_BitStreamInfo.bytePtr;
        while (lowBits > 0) {
            iCache <<= 8;
            if (buf < m_aac_BitStreamInfo.bytePtr + m_aac_BitStreamInfo.nBytes)
                iCache |= (uint32_t)*buf++;
            lowBits -= 8;
        }
        lowBits = -lowBits;
        data |= iCache >> lowBits;
    }

    return data;
}

/***********************************************************************************************************************
 * Function:    AdvanceBitstream
 *
 * Description: move bitstream pointer ahead
 *
 * Inputs:      number of bits to advance bitstream
 *
 * Outputs:     updated bitstream info struct
 *
 * Return:      none
 *
 * Notes:       generally used following GetBitsNoAdvance(bsi, maxBits)
 **********************************************************************************************************************/
void AACDecoder::AdvanceBitstream(int32_t nBits) {
    nBits &= 0x1f;
    if (nBits > m_aac_BitStreamInfo.cachedBits) {
        nBits -= m_aac_BitStreamInfo.cachedBits;
        RefillBitstreamCache();
    }
    m_aac_BitStreamInfo.iCache <<= nBits;
    m_aac_BitStreamInfo.cachedBits -= nBits;
}

/***********************************************************************************************************************
 * Function:    CalcBitsUsed
 *
 * Description: calculate how many bits have been read from bitstream
 *
 * Inputs:      pointer to start of bitstream buffer
 *              bit offset into first byte of startBuf (0-7)
 *
 * Outputs:     none
 *
 * Return:      number of bits read from bitstream, as offset from startBuf:startOffset
 **********************************************************************************************************************/
int32_t AACDecoder::CalcBitsUsed(uint8_t *startBuf, int32_t startOffset) {
    int32_t bitsUsed;

    bitsUsed = (m_aac_BitStreamInfo.bytePtr - startBuf) * 8;
    bitsUsed -= m_aac_BitStreamInfo.cachedBits;
    bitsUsed -= startOffset;

    return bitsUsed;
}
/***********************************************************************************************************************
 * Function:    ByteAlignBitstream
 *
 * Description: bump bitstream pointer to start of next byte
 *
 * Inputs:      none
 *
 * Outputs:     byte-aligned bitstream aac_BitStreamInfo_t struct
 *
 * Return:      none
 *
 * Notes:       if bitstream is already byte-aligned, do nothing
 **********************************************************************************************************************/
void AACDecoder::ByteAlignBitstream() {
    int32_t offset;

    offset = m_aac_BitStreamInfo.cachedBits & 0x07;
    AdvanceBitstream(offset);
}

#ifdef AAC_ENABLE_SBR

/**************************************************************************************
 * Function:    InitSBRState
 *
 * Description: initialize PSInfoSBR struct at start of stream or after flush
 *
 * Inputs:      valid AACDecInfo struct
 *
 * Outputs:     PSInfoSBR struct with proper initial state
 *
 * Return:      none
 **************************************************************************************/
void AACDecoder::InitSBRState() {
    int32_t i, ch;
    uint8_t *c;

    if (!m_PSInfoSBR)
        return;

    /* clear SBR state structure */
    c = (uint8_t *)m_PSInfoSBR;
    /* for (i = 0; i < (int32_t)sizeof(m_PSInfoSBR); i++)
        *c++ = 0; */ //bug
    memset(m_PSInfoSBR, 0, sizeof(*m_PSInfoSBR)); //better, fb
    /* initialize non-zero state variables */
    for (ch = 0; ch < AAC_MAX_NCHANS; ch++) {
        m_PSInfoSBR->sbrChan[ch].reset = 1;
        m_PSInfoSBR->sbrChan[ch].laPrev = -1;
    }
}
#endif

/***********************************************************************************************************************
 * Function:    DecodeSBRBitstream
 *
 * Description: decode sideband information for SBR
 *
 * Inputs:      base output channel (range = [0, nChans-1])
 *
 * Outputs:     initialized state structs (SBRHdr, SBRGrid, SBRFreq, SBRChan)
 *
 * Return:      0 if successful, error code (< 0) if error
 *
 * Notes:       SBR payload should be in aacDecInfo->fillBuf
 *              returns with no error if fill buffer is not an SBR extension block,
 *                or if current block is not a fill block (e.g. for LFE upsampling)
 **********************************************************************************************************************/
int32_t AACDecoder::DecodeSBRBitstream(int32_t chBase) {
    int32_t headerFlag;

    if (m_AACDecInfo->currBlockID != AAC_ID_FIL ||
        (m_AACDecInfo->fillExtType != EXT_SBR_DATA && m_AACDecInfo->fillExtType != EXT_SBR_DATA_CRC))
        return ERR_AAC_NONE;

    SetBitstreamPointer(m_AACDecInfo->fillCount, m_AACDecInfo->fillBuf);
    if (GetBits(4) != (uint32_t)m_AACDecInfo->fillExtType)
        return ERR_AAC_SBR_BITSTREAM;

    if (m_AACDecInfo->fillExtType == EXT_SBR_DATA_CRC)
        m_PSInfoSBR->crcCheckWord = GetBits(10);

    headerFlag = GetBits(1);
    if (headerFlag) {
        /* get sample rate index for output sample rate (2x base rate) */
        m_PSInfoSBR->sampRateIdx = GetSampRateIdx(2 * m_AACDecInfo->sampRate);
        if (m_PSInfoSBR->sampRateIdx < 0 || m_PSInfoSBR->sampRateIdx >= NUM_SAMPLE_RATES)
            return ERR_AAC_SBR_BITSTREAM;
        else if (m_PSInfoSBR->sampRateIdx >= NUM_SAMPLE_RATES_SBR)
            return ERR_AAC_SBR_SINGLERATE_UNSUPPORTED;

        /* reset flag = 1 if header values changed */
        if (UnpackSBRHeader(&(m_PSInfoSBR->sbrHdr[chBase])))
            m_PSInfoSBR->sbrChan[chBase].reset = 1;

        /* first valid SBR header should always trigger CalcFreqTables(), since psi->reset was set in InitSBR() */
        if (m_PSInfoSBR->sbrChan[chBase].reset)
            CalcFreqTables(&(m_PSInfoSBR->sbrHdr[chBase + 0]), &(m_PSInfoSBR->sbrFreq[chBase]),
                           m_PSInfoSBR->sampRateIdx);

        /* copy and reset state to right channel for CPE */
        if (m_AACDecInfo->prevBlockID == AAC_ID_CPE)
            m_PSInfoSBR->sbrChan[chBase + 1].reset = m_PSInfoSBR->sbrChan[chBase + 0].reset;
    }

    /* if no header has been received, upsample only */
    if (m_PSInfoSBR->sbrHdr[chBase].count == 0)
        return ERR_AAC_NONE;

    if (m_AACDecInfo->prevBlockID == AAC_ID_SCE) {
        UnpackSBRSingleChannel(chBase);
    } else if (m_AACDecInfo->prevBlockID == AAC_ID_CPE) {
        UnpackSBRChannelPair(chBase);
    } else {
        return ERR_AAC_SBR_BITSTREAM;
    }

    ByteAlignBitstream();

    return ERR_AAC_NONE;
}

#ifdef AAC_ENABLE_SBR

/***********************************************************************************************************************
 * Function:    DecodeSBRData
 *
 * Description: apply SBR to one frame of PCM data
 *
 * Inputs:      1024 samples of decoded 32-bit PCM, before SBR
 *              size of input PCM samples (must be 4 bytes)
 *              number of fraction bits in input PCM samples
 *              base output channel (range = [0, nChans-1])
 *              initialized state structs (SBRHdr, SBRGrid, SBRFreq, SBRChan)
 *
 * Outputs:     2048 samples of decoded 16-bit PCM, after SBR
 *
 * Return:      0 if successful, error code (< 0) if error
 **********************************************************************************************************************/
int32_t AACDecoder::DecodeSBRData(int32_t chBase, int16_t *outbuf) {
    int32_t k, l, ch, chBlock, qmfaBands, qmfsBands;
    int32_t upsampleOnly, gbIdx, gbMask;
    int32_t *inbuf;
    int16_t *outptr;

    SBRHeader *sbrHdr;
    SBRGrid *sbrGrid;
    SBRFreq *sbrFreq;
    SBRChan *sbrChan;

    /* same header and freq tables for both channels in CPE */
    sbrHdr = &(m_PSInfoSBR->sbrHdr[chBase]);
    sbrFreq = &(m_PSInfoSBR->sbrFreq[chBase]);

    /* upsample only if we haven't received an SBR header yet or if we have an LFE block */
    if (m_AACDecInfo->currBlockID == AAC_ID_LFE) {
        chBlock = 1;
        upsampleOnly = 1;
    } else if (m_AACDecInfo->currBlockID == AAC_ID_FIL) {
        if (m_AACDecInfo->prevBlockID == AAC_ID_SCE)
            chBlock = 1;
        else if (m_AACDecInfo->prevBlockID == AAC_ID_CPE)
            chBlock = 2;
        else
            return ERR_AAC_NONE;

        upsampleOnly = (sbrHdr->count == 0 ? 1 : 0);
        if (m_AACDecInfo->fillExtType != EXT_SBR_DATA && m_AACDecInfo->fillExtType != EXT_SBR_DATA_CRC)
            return ERR_AAC_NONE;
    } else {
        /* ignore non-SBR blocks */
        return ERR_AAC_NONE;
    }

    if (upsampleOnly) {
        sbrFreq->kStart = 32;
        sbrFreq->numQMFBands = 0;
    }

    for (ch = 0; ch < chBlock; ch++) {
        sbrGrid = &(m_PSInfoSBR->sbrGrid[chBase + ch]);
        sbrChan = &(m_PSInfoSBR->sbrChan[chBase + ch]);

        if (m_AACDecInfo->rawSampleBuf[ch] == 0 || m_AACDecInfo->rawSampleBytes != 4)
            return ERR_AAC_SBR_PCM_FORMAT;
        inbuf = (int32_t *)m_AACDecInfo->rawSampleBuf[ch];
        outptr = outbuf + chBase + ch;

        /* restore delay buffers (could use ring buffer or keep in temp buffer for nChans == 1) */
        for (l = 0; l < HF_GEN; l++) {
            for (k = 0; k < 64; k++) {
                m_PSInfoSBR->XBuf[l][k][0] = m_PSInfoSBR->XBufDelay[chBase + ch][l][k][0];
                m_PSInfoSBR->XBuf[l][k][1] = m_PSInfoSBR->XBufDelay[chBase + ch][l][k][1];
            }
        }

        /* step 1 - analysis QMF */
        qmfaBands = sbrFreq->kStart;
        for (l = 0; l < 32; l++) {
            gbMask = QMFAnalysis(inbuf + l * 32, m_PSInfoSBR->delayQMFA[chBase + ch], m_PSInfoSBR->XBuf[l + HF_GEN][0],
                                 m_AACDecInfo->rawSampleFBits, &(m_PSInfoSBR->delayIdxQMFA[chBase + ch]), qmfaBands);

            gbIdx = ((l + HF_GEN) >> 5) & 0x01;
            sbrChan->gbMask[gbIdx] |= gbMask; /* gbIdx = (0 if i < 32), (1 if i >= 32) */
        }

        if (upsampleOnly) {
            /* no SBR - just run synthesis QMF to upsample by 2x */
            qmfsBands = 32;
            for (l = 0; l < 32; l++) {
                /* step 4 - synthesis QMF */
                QMFSynthesis(m_PSInfoSBR->XBuf[l + HF_ADJ][0], m_PSInfoSBR->delayQMFS[chBase + ch],
                             &(m_PSInfoSBR->delayIdxQMFS[chBase + ch]), qmfsBands, outptr, m_AACDecInfo->nChans);
                outptr += 64 * m_AACDecInfo->nChans;
            }
        } else {
            /* if previous frame had lower SBR starting freq than current, zero out the synthesized QMF
             *   bands so they aren't used as sources for patching
             * after patch generation, restore from delay buffer
             * can only happen after header reset
             */
            /*
            for (k = sbrFreq->kStartPrev; k < sbrFreq->kStart; k++) {
                for (l = 0; l < sbrGrid->envTimeBorder[0] + HF_ADJ; l++) {
                    m_PSInfoSBR->XBuf[l][k][0] = 0;
                    m_PSInfoSBR->XBuf[l][k][1] = 0;
                }
            }*/
            for (l = 0; l < endL; l++) {
                memset(&XBuf[l][kStartPrev][0], 0, (kStart - kStartPrev) * 2 * sizeof(int32_t));
            } //fb
        }
        /* step 2 - HF generation */
        GenerateHighFreq(sbrGrid, sbrFreq, sbrChan, ch);

        /* restore SBR bands that were cleared before patch generation (time slots 0, 1 no longer needed) */
        /**/
        for (k = sbrFreq->kStartPrev; k < sbrFreq->kStart; k++) {
            for (l = HF_ADJ; l < sbrGrid->envTimeBorder[0] + HF_ADJ; l++) {
                m_PSInfoSBR->XBuf[l][k][0] = m_PSInfoSBR->XBufDelay[chBase + ch][l][k][0];
                m_PSInfoSBR->XBuf[l][k][1] = m_PSInfoSBR->XBufDelay[chBase + ch][l][k][1];
            }
        }
        */ for (l = HF_ADJ; l < endL; l++) { //fb
            memcpy(&XBuf[l][kStartPrev][0], &XBufDelay[chBase + ch][l][kStartPrev][0],
                   (kStart - kStartPrev) * 2 * sizeof(int32_t));
        }

        /* step 3 - HF adjustment */
        AdjustHighFreq(sbrHdr, sbrGrid, sbrFreq, sbrChan, ch);

        /* step 4 - synthesis QMF */
        qmfsBands = sbrFreq->kStartPrev + sbrFreq->numQMFBandsPrev;
        for (l = 0; l < sbrGrid->envTimeBorder[0]; l++) {
            /* if new envelope starts mid-frame, use old settings until start of first envelope in this frame */
            QMFSynthesis(m_PSInfoSBR->XBuf[l + HF_ADJ][0], m_PSInfoSBR->delayQMFS[chBase + ch],
                         &(m_PSInfoSBR->delayIdxQMFS[chBase + ch]), qmfsBands, outptr, m_AACDecInfo->nChans);
            outptr += 64 * m_AACDecInfo->nChans;
        }

        qmfsBands = sbrFreq->kStart + sbrFreq->numQMFBands;
        for (; l < 32; l++) {
            /* use new settings for rest of frame (usually the entire frame, unless the first envelope starts mid-frame) */
            QMFSynthesis(m_PSInfoSBR->XBuf[l + HF_ADJ][0], m_PSInfoSBR->delayQMFS[chBase + ch],
                         &(m_PSInfoSBR->delayIdxQMFS[chBase + ch]), qmfsBands, outptr, m_AACDecInfo->nChans);
            outptr += 64 * m_AACDecInfo->nChans;
        }
    }

    /* save delay */
    for (l = 0; l < HF_GEN; l++) {
        for (k = 0; k < 64; k++) {
            m_PSInfoSBR->XBufDelay[chBase + ch][l][k][0] = m_PSInfoSBR->XBuf[l + 32][k][0];
            m_PSInfoSBR->XBufDelay[chBase + ch][l][k][1] = m_PSInfoSBR->XBuf[l + 32][k][1];
        }
    }
    sbrChan->gbMask[0] = sbrChan->gbMask[1];
    sbrChan->gbMask[1] = 0;

    if (sbrHdr->count > 0)
        sbrChan->reset = 0;
}
sbrFreq->kStartPrev = sbrFreq->kStart;
sbrFreq->numQMFBandsPrev = sbrFreq->numQMFBands;

if (m_AACDecInfo->nChans > 0 && (chBase + ch) == m_AACDecInfo->nChans)
    m_PSInfoSBR->frameCount++;

return ERR_AAC_NONE;
}

#endif

/***********************************************************************************************************************
 * Function:    BubbleSort
 *
 * Description: in-place sort of uint8_ts
 *
 * Inputs:      buffer of elements to sort
 *              number of elements to sort
 *
 * Outputs:     sorted buffer
 *
 * Return:      none
 **********************************************************************************************************************/
void AACDecoder::BubbleSort(uint8_t *v, int32_t nItems) {
    int32_t i;
    uint8_t t;

    while (nItems >= 2) {
        for (i = 0; i < nItems - 1; i++) {
            if (v[i + 1] < v[i]) {
                t = v[i + 1];
                v[i + 1] = v[i];
                v[i] = t;
            }
        }
        nItems--;
    }
}
/***********************************************************************************************************************
 * Function:    VMin
 *
 * Description: find smallest element in a buffer of uint8_ts
 *
 * Inputs:      buffer of elements to search
 *              number of elements to search
 *
 * Outputs:     none
 *
 * Return:      smallest element in buffer
 **********************************************************************************************************************/
uint8_t AACDecoder::VMin(uint8_t *v, int32_t nItems) {
    int32_t i;
    uint8_t vMin;

    vMin = v[0];
    for (i = 1; i < nItems; i++) {
        if (v[i] < vMin)
            vMin = v[i];
    }
    return vMin;
}
/***********************************************************************************************************************
 * Function:    VMax
 *
 * Description: find largest element in a buffer of uint8_ts
 *
 * Inputs:      buffer of elements to search
 *              number of elements to search
 *
 * Outputs:     none
 *
 * Return:      largest element in buffer
 **********************************************************************************************************************/
uint8_t AACDecoder::VMax(uint8_t *v, int32_t nItems) {
    int32_t i;
    uint8_t vMax;

    vMax = v[0];
    for (i = 1; i < nItems; i++) {
        if (v[i] > vMax)
            vMax = v[i];
    }
    return vMax;
}
/***********************************************************************************************************************
 * Function:    CalcFreqMasterScaleZero
 *
 * Description: calculate master frequency table when freqScale == 0
 *                (4.6.18.3.2.1, figure 4.39)
 *
 * Inputs:      alterScale flag
 *              index of first QMF subband in master freq table (k0)
 *              index of last QMF subband (k2)
 *
 * Outputs:     master frequency table
 *
 * Return:      number of bands in master frequency table
 *
 * Notes:       assumes k2 - k0 <= 48 and k2 >= k0 (4.6.18.3.6)
 **********************************************************************************************************************/
int32_t AACDecoder::CalcFreqMasterScaleZero(uint8_t *freqMaster, int32_t alterScale, int32_t k0, int32_t k2) {
    int32_t nMaster, k, nBands, k2Achieved, dk, vDk[64], k2Diff;

    if (alterScale) {
        dk = 2;
        nBands = 2 * ((k2 - k0 + 2) >> 2);
    } else {
        dk = 1;
        nBands = 2 * ((k2 - k0) >> 1);
    }

    if (nBands <= 0)
        return 0;

    k2Achieved = k0 + nBands * dk;
    k2Diff = k2 - k2Achieved;
    for (k = 0; k < nBands; k++)
        vDk[k] = dk;

    if (k2Diff > 0) {
        k = nBands - 1;
        while (k2Diff) {
            vDk[k]++;
            k--;
            k2Diff--;
        }
    } else if (k2Diff < 0) {
        k = 0;
        while (k2Diff) {
            vDk[k]--;
            k++;
            k2Diff++;
        }
    }

    nMaster = nBands;
    freqMaster[0] = k0;
    for (k = 1; k <= nBands; k++)
        freqMaster[k] = freqMaster[k - 1] + vDk[k - 1];

    return nMaster;
}

/* mBandTab[i] = temp1[i] / 2 */
static const int32_t mBandTab[3] PROGMEM = {6, 5, 4};

/* invWarpTab[i] = 1.0 / temp2[i], Q30 (see 4.6.18.3.2.1) */
static const int32_t invWarpTab[2] PROGMEM = {0x40000000, 0x313b13b1};

/***********************************************************************************************************************
 * Function:    CalcFreqMasterScale
 *
 * Description: calculate master frequency table when freqScale > 0
 *                (4.6.18.3.2.1, figure 4.39)
 *
 * Inputs:      alterScale flag
 *              freqScale flag
 *              index of first QMF subband in master freq table (k0)
 *              index of last QMF subband (k2)
 *
 * Outputs:     master frequency table
 *
 * Return:      number of bands in master frequency table
 *
 * Notes:       assumes k2 - k0 <= 48 and k2 >= k0 (4.6.18.3.6)
 **********************************************************************************************************************/
int32_t AACDecoder::CalcFreqMaster(uint8_t *freqMaster, int32_t freqScale, int32_t alterScale, int32_t k0, int32_t k2) {
    int32_t bands, twoRegions, k, k1, t, vLast, vCurr, pCurr;
    int32_t invWarp, nBands0, nBands1, change;
    uint8_t vDk1Min, vDk0Max;
    uint8_t *vDelta;

    if (freqScale < 1 || freqScale > 3)
        return -1;

    bands = mBandTab[freqScale - 1];
    invWarp = invWarpTab[alterScale];

    /* tested for all k0 = [5, 64], k2 = [k0, 64] */
    if (k2 * 10000 > 22449 * k0) {
        twoRegions = 1;
        k1 = 2 * k0;
    } else {
        twoRegions = 0;
        k1 = k2;
    }

    /* tested for all k0 = [5, 64], k1 = [k0, 64], freqScale = [1,3] */
    t = (log2Tab[k1] - log2Tab[k0]) >> 3; /* log2(k1/k0), Q28 to Q25 */
    nBands0 = 2 * (((bands * t) + (1 << 24)) >>
                   25); /* multiply by bands/2, round to nearest int32_t (mBandTab has factor of 1/2 rolled in) */

    /* tested for all valid combinations of k0, k1, nBands (from sampRate, freqScale, alterScale)
     * roundoff error can be a problem with fixpt (e.g. pCurr = 12.499999 instead of 12.50003)
     *   because successive multiplication always undershoots a little bit, but this
     *   doesn't occur in any of the ratios we encounter from the valid k0/k1 bands in the spec
     */
    t = RatioPowInv(k1, k0, nBands0);
    pCurr = k0 << 24;
    vLast = k0;
    vDelta = freqMaster + 1; /* operate in-place */
    for (k = 0; k < nBands0; k++) {
        pCurr = MULSHIFT32(pCurr, t) << 8; /* keep in Q24 */
        vCurr = (pCurr + (1 << 23)) >> 24;
        vDelta[k] = (vCurr - vLast);
        vLast = vCurr;
    }

    /* sort the deltas and find max delta for first region */
    BubbleSort(vDelta, nBands0);
    vDk0Max = VMax(vDelta, nBands0);

    /* fill master frequency table with bands from first region */
    freqMaster[0] = k0;
    for (k = 1; k <= nBands0; k++)
        freqMaster[k] += freqMaster[k - 1];

    /* if only one region, then the table is complete */
    if (!twoRegions)
        return nBands0;

    /* tested for all k1 = [10, 64], k2 = [k0, 64], freqScale = [1,3] */
    t = (log2Tab[k2] - log2Tab[k1]) >> 3;    /* log2(k1/k0), Q28 to Q25 */
    t = MULSHIFT32(bands * t, invWarp) << 2; /* multiply by bands/2, divide by warp factor, keep Q25 */
    nBands1 = 2 * ((t + (1 << 24)) >> 25);   /* round to nearest int32_t */

    /* see comments above for calculations in first region */
    t = RatioPowInv(k2, k1, nBands1);
    pCurr = k1 << 24;
    vLast = k1;
    vDelta = freqMaster + nBands0 + 1; /* operate in-place */
    for (k = 0; k < nBands1; k++) {
        pCurr = MULSHIFT32(pCurr, t) << 8; /* keep in Q24 */
        vCurr = (pCurr + (1 << 23)) >> 24;
        vDelta[k] = (vCurr - vLast);
        vLast = vCurr;
    }

    /* sort the deltas, adjusting first and last if the second region has smaller deltas than the first */
    vDk1Min = VMin(vDelta, nBands1);
    if (vDk1Min < vDk0Max) {
        BubbleSort(vDelta, nBands1);
        change = vDk0Max - vDelta[0];
        if (change > ((vDelta[nBands1 - 1] - vDelta[0]) >> 1))
            change = ((vDelta[nBands1 - 1] - vDelta[0]) >> 1);
        vDelta[0] += change;
        vDelta[nBands1 - 1] -= change;
    }
    BubbleSort(vDelta, nBands1);

    /* fill master frequency table with bands from second region
     * Note: freqMaster[nBands0] = k1
     */
    for (k = 1; k <= nBands1; k++)
        freqMaster[k + nBands0] += freqMaster[k + nBands0 - 1];

    return (nBands0 + nBands1);
}
/***********************************************************************************************************************
 * Function:    CalcFreqHigh
 *
 * Description: calculate high resolution frequency table (4.6.18.3.2.2)
 *
 * Inputs:      master frequency table
 *              number of bands in master frequency table
 *              crossover band from header
 *
 * Outputs:     high resolution frequency table
 *
 * Return:      number of bands in high resolution frequency table
 **********************************************************************************************************************/
int32_t AACDecoder::CalcFreqHigh(uint8_t *freqHigh, uint8_t *freqMaster, int32_t nMaster, int32_t crossOverBand) {
    int32_t k, nHigh;

    nHigh = nMaster - crossOverBand;

    for (k = 0; k <= nHigh; k++)
        freqHigh[k] = freqMaster[k + crossOverBand];

    return nHigh;
}
/***********************************************************************************************************************
 * Function:    CalcFreqLow
 *
 * Description: calculate low resolution frequency table (4.6.18.3.2.2)
 *
 * Inputs:      high resolution frequency table
 *              number of bands in high resolution frequency table
 *
 * Outputs:     low resolution frequency table
 *
 * Return:      number of bands in low resolution frequency table
 **********************************************************************************************************************/
int32_t AACDecoder::CalcFreqLow(uint8_t *freqLow, uint8_t *freqHigh, int32_t nHigh) {
    int32_t k, nLow, oddFlag;

    nLow = nHigh - (nHigh >> 1);
    freqLow[0] = freqHigh[0];
    oddFlag = nHigh & 0x01;

    for (k = 1; k <= nLow; k++)
        freqLow[k] = freqHigh[2 * k - oddFlag];

    return nLow;
}
/***********************************************************************************************************************
 * Function:    CalcFreqNoise
 *
 * Description: calculate noise floor frequency table (4.6.18.3.2.2)
 *
 * Inputs:      low resolution frequency table
 *              number of bands in low resolution frequency table
 *              index of starting QMF subband for SBR (kStart)
 *              index of last QMF subband (k2)
 *              number of noise bands
 *
 * Outputs:     noise floor frequency table
 *
 * Return:      number of bands in noise floor frequency table
 **********************************************************************************************************************/
int32_t AACDecoder::CalcFreqNoise(uint8_t *freqNoise, uint8_t *freqLow, int32_t nLow, int32_t kStart, int32_t k2,
                                  int32_t noiseBands) {
    int32_t i, iLast, k, nQ, lTop, lBottom;

    lTop = log2Tab[k2];
    lBottom = log2Tab[kStart];
    nQ = noiseBands * ((lTop - lBottom) >> 2); /* Q28 to Q26, noiseBands = [0,3] */
    nQ = (nQ + (1 << 25)) >> 26;
    if (nQ < 1)
        nQ = 1;

    ASSERT(nQ <= MAX_NUM_NOISE_FLOOR_BANDS); /* required from 4.6.18.3.6 */

    iLast = 0;
    freqNoise[0] = freqLow[0];
    for (k = 1; k <= nQ; k++) {
        i = iLast + (nLow - iLast) / (nQ + 1 - k); /* truncating division */
        freqNoise[k] = freqLow[i];
        iLast = i;
    }

    return nQ;
}
/***********************************************************************************************************************
 * Function:    BuildPatches
 *
 * Description: build high frequency patches (4.6.18.6.3)
 *
 * Inputs:      master frequency table
 *              number of bands in low resolution frequency table
 *              index of first QMF subband in master freq table (k0)
 *              index of starting QMF subband for SBR (kStart)
 *              number of QMF bands in high resolution frequency table
 *              sample rate index
 *
 * Outputs:     starting subband for each patch
 *              number of subbands in each patch
 *
 * Return:      number of patches
 **********************************************************************************************************************/
int32_t AACDecoder::BuildPatches(uint8_t *patchNumSubbands, uint8_t *patchStartSubband, uint8_t *freqMaster,
                                 int32_t nMaster, int32_t k0, int32_t kStart, int32_t numQMFBands,
                                 int32_t sampRateIdx) {
    int32_t i, j, k;
    int32_t msb, sb, usb, numPatches, goalSB, oddFlag;

    msb = k0;
    usb = kStart;
    numPatches = 0;
    goalSB = goalSBTab[sampRateIdx];

    if (nMaster == 0) {
        patchNumSubbands[0] = 0;
        patchStartSubband[0] = 0;
        return 0;
    }

    if (goalSB < kStart + numQMFBands) {
        k = 0;
        for (i = 0; freqMaster[i] < goalSB; i++)
            k = i + 1;
    } else {
        k = nMaster;
    }

    do {
        j = k + 1;
        do {
            j--;
            sb = freqMaster[j];
            oddFlag = (sb - 2 + k0) & 0x01;
        } while (sb > k0 - 1 + msb - oddFlag);

        patchNumSubbands[numPatches] = MAX(sb - usb, (int32_t)0);
        patchStartSubband[numPatches] = k0 - oddFlag - patchNumSubbands[numPatches];

        /* from MPEG reference code - slightly different from spec */
        if ((patchNumSubbands[numPatches] < 3) && (numPatches > 0))
            break;

        if (patchNumSubbands[numPatches] > 0) {
            usb = sb;
            msb = sb;
            numPatches++;
        } else {
            msb = kStart;
        }

        if (freqMaster[k] - sb < 3)
            k = nMaster;

    } while (sb != (kStart + numQMFBands) && numPatches <= MAX_NUM_PATCHES);

    return numPatches;
}
/***********************************************************************************************************************
 * Function:    FindFreq
 *
 * Description: search buffer of uint8_ts for a specific value
 *
 * Inputs:      buffer of elements to search
 *              number of elements to search
 *              value to search for
 *
 * Outputs:     none
 *
 * Return:      non-zero if the value is found anywhere in the buffer, zero otherwise
 **********************************************************************************************************************/
int32_t AACDecoder::FindFreq(uint8_t *freq, int32_t nFreq, uint8_t val) {
    int32_t k;

    for (k = 0; k < nFreq; k++) {
        if (freq[k] == val)
            return 1;
    }

    return 0;
}
/***********************************************************************************************************************
 * Function:    RemoveFreq
 *
 * Description: remove one element from a buffer of uint8_ts
 *
 * Inputs:      buffer of elements
 *              number of elements
 *              index of element to remove
 *
 * Outputs:     new buffer of length nFreq-1
 *
 * Return:      none
 **********************************************************************************************************************/
void AACDecoder::RemoveFreq(uint8_t *freq, int32_t nFreq, int32_t removeIdx) {
    int32_t k;

    if (removeIdx >= nFreq)
        return;

    for (k = removeIdx; k < nFreq - 1; k++)
        freq[k] = freq[k + 1];
}
/***********************************************************************************************************************
 * Function:    CalcFreqLimiter
 *
 * Description: calculate limiter frequency table (4.6.18.3.2.3)
 *
 * Inputs:      number of subbands in each patch
 *              low resolution frequency table
 *              number of bands in low resolution frequency table
 *              index of starting QMF subband for SBR (kStart)
 *              number of limiter bands
 *              number of patches
 *
 * Outputs:     limiter frequency table
 *
 * Return:      number of bands in limiter frequency table
 **********************************************************************************************************************/
int32_t AACDecoder::CalcFreqLimiter(uint8_t *freqLimiter, uint8_t *patchNumSubbands, uint8_t *freqLow, int32_t nLow,
                                    int32_t kStart, int32_t limiterBands, int32_t numPatches) {
    int32_t k, bands, nLimiter, nOctaves;
    int32_t limBandsPerOctave[3] = {120, 200, 300}; /* [1.2, 2.0, 3.0] * 100 */
    uint8_t patchBorders[MAX_NUM_PATCHES + 1];

    /* simple case */
    if (limiterBands == 0) {
        freqLimiter[0] = freqLow[0] - kStart;
        freqLimiter[1] = freqLow[nLow] - kStart;
        return 1;
    }

    bands = limBandsPerOctave[limiterBands - 1];
    patchBorders[0] = kStart;

    /* from MPEG reference code - slightly different from spec (top border) */
    for (k = 1; k < numPatches; k++)
        patchBorders[k] = patchBorders[k - 1] + patchNumSubbands[k - 1];
    patchBorders[k] = freqLow[nLow];

    for (k = 0; k <= nLow; k++)
        freqLimiter[k] = freqLow[k];

    for (k = 1; k < numPatches; k++)
        freqLimiter[k + nLow] = patchBorders[k];

    k = 1;
    nLimiter = nLow + numPatches - 1;
    BubbleSort(freqLimiter, nLimiter + 1);

    while (k <= nLimiter) {
        nOctaves = log2Tab[freqLimiter[k]] - log2Tab[freqLimiter[k - 1]]; /* Q28 */
        nOctaves = (nOctaves >> 9) * bands;                               /* Q19, max bands = 300 < 2^9 */
        if (nOctaves < (49 << 19)) {                                      /* compare with 0.49*100, in Q19 */
            if (freqLimiter[k] == freqLimiter[k - 1] || FindFreq(patchBorders, numPatches + 1, freqLimiter[k]) == 0) {
                RemoveFreq(freqLimiter, nLimiter + 1, k);
                nLimiter--;
            } else if (FindFreq(patchBorders, numPatches + 1, freqLimiter[k - 1]) == 0) {
                RemoveFreq(freqLimiter, nLimiter + 1, k - 1);
                nLimiter--;
            } else {
                k++;
            }
        } else {
            k++;
        }
    }

    /* store limiter boundaries as offsets from kStart */
    for (k = 0; k <= nLimiter; k++)
        freqLimiter[k] -= kStart;

    return nLimiter;
}
/***********************************************************************************************************************
 * Function:    CalcFreqTables
 *
 * Description: calulate master and derived frequency tables, and patches
 *
 * Inputs:      initialized SBRHeader struct for this SCE/CPE block
 *              initialized SBRFreq struct for this SCE/CPE block
 *              sample rate index of output sample rate (after SBR)
 *
 * Outputs:     master and derived frequency tables, and patches
 *
 * Return:      non-zero if error, zero otherwise
 **********************************************************************************************************************/
int32_t AACDecoder::CalcFreqTables(SBRHeader *sbrHdr, SBRFreq *sbrFreq, int32_t sampRateIdx) {
    int32_t k0, k2;

    k0 = k0Tab[sampRateIdx][sbrHdr->startFreq];

    if (sbrHdr->stopFreq == 14)
        k2 = 2 * k0;
    else if (sbrHdr->stopFreq == 15)
        k2 = 3 * k0;
    else
        k2 = k2Tab[sampRateIdx][sbrHdr->stopFreq];
    if (k2 > 64)
        k2 = 64;

    /* calculate master frequency table */
    if (sbrHdr->freqScale == 0)
        sbrFreq->nMaster = CalcFreqMasterScaleZero(sbrFreq->freqMaster, sbrHdr->alterScale, k0, k2);
    else
        sbrFreq->nMaster = CalcFreqMaster(sbrFreq->freqMaster, sbrHdr->freqScale, sbrHdr->alterScale, k0, k2);

    /* calculate high frequency table and related parameters */
    sbrFreq->nHigh = CalcFreqHigh(sbrFreq->freqHigh, sbrFreq->freqMaster, sbrFreq->nMaster, sbrHdr->crossOverBand);
    sbrFreq->numQMFBands = sbrFreq->freqHigh[sbrFreq->nHigh] - sbrFreq->freqHigh[0];
    sbrFreq->kStart = sbrFreq->freqHigh[0];

    /* calculate low frequency table */
    sbrFreq->nLow = CalcFreqLow(sbrFreq->freqLow, sbrFreq->freqHigh, sbrFreq->nHigh);

    /* calculate noise floor frequency table */
    sbrFreq->numNoiseFloorBands =
        CalcFreqNoise(sbrFreq->freqNoise, sbrFreq->freqLow, sbrFreq->nLow, sbrFreq->kStart, k2, sbrHdr->noiseBands);

    /* calculate limiter table */
    sbrFreq->numPatches = BuildPatches(sbrFreq->patchNumSubbands, sbrFreq->patchStartSubband, sbrFreq->freqMaster,
                                       sbrFreq->nMaster, k0, sbrFreq->kStart, sbrFreq->numQMFBands, sampRateIdx);
    sbrFreq->nLimiter = CalcFreqLimiter(sbrFreq->freqLimiter, sbrFreq->patchNumSubbands, sbrFreq->freqLow,
                                        sbrFreq->nLow, sbrFreq->kStart, sbrHdr->limiterBands, sbrFreq->numPatches);

    return 0;
}
/***********************************************************************************************************************
 * Function:    EstimateEnvelope
 *
 * Description: estimate power of generated HF QMF bands in one time-domain envelope
 *                (4.6.18.7.3)
 *
 * Inputs:      initialized PSInfoSBR struct
 *              initialized SBRHeader struct for this SCE/CPE block
 *              initialized SBRGrid struct for this channel
 *              initialized SBRFreq struct for this SCE/CPE block
 *              index of current envelope
 *
 * Outputs:     power of each QMF subband, stored as integer (Q0) * 2^N, N >= 0
 *
 * Return:      none
 **********************************************************************************************************************/
void AACDecoder::EstimateEnvelope(SBRHeader *sbrHdr, SBRGrid *sbrGrid, SBRFreq *sbrFreq, int32_t env) {
    int32_t i, m, iStart, iEnd, xre, xim, nScale, expMax;
    int32_t p, n, mStart, mEnd, invFact, t;
    int32_t *XBuf;
    U64 eCurr;
    uint8_t *freqBandTab;

    /* estimate current envelope */
    iStart = sbrGrid->envTimeBorder[env] + HF_ADJ;
    iEnd = sbrGrid->envTimeBorder[env + 1] + HF_ADJ;
    if (sbrGrid->freqRes[env]) {
        n = sbrFreq->nHigh;
        freqBandTab = sbrFreq->freqHigh;
    } else {
        n = sbrFreq->nLow;
        freqBandTab = sbrFreq->freqLow;
    }

    /* ADS should inline MADD64 (smlal) properly, but check to make sure */
    expMax = 0;
    if (sbrHdr->interpFreq) {
        for (m = 0; m < sbrFreq->numQMFBands; m++) {
            eCurr.w64 = 0;
            XBuf = m_PSInfoSBR->XBuf[iStart][sbrFreq->kStart + m];
            for (i = iStart; i < iEnd; i++) {
                /* scale to int32_t before calculating power (precision not critical, and avoids overflow) */
                xre = (*XBuf) >> FBITS_OUT_QMFA;
                XBuf += 1;
                xim = (*XBuf) >> FBITS_OUT_QMFA;
                XBuf += (2 * 64 - 1);
                eCurr.w64 = MADD64(eCurr.w64, xre, xre);
                eCurr.w64 = MADD64(eCurr.w64, xim, xim);
            }

            /* eCurr.w64 is now Q(64 - 2*FBITS_OUT_QMFA) (64-bit word)
             * if energy is too big to fit in 32-bit word (> 2^31) scale down by power of 2
             */
            nScale = 0;
            if (eCurr.r.hi32) {
                nScale = (32 - CLZ(eCurr.r.hi32)) + 1;
                t = (int32_t)(eCurr.r.lo32 >> nScale); /* logical (unsigned) >> */
                t |= eCurr.r.hi32 << (32 - nScale);
            } else if (eCurr.r.lo32 >> 31) {
                nScale = 1;
                t = (int32_t)(eCurr.r.lo32 >> nScale); /* logical (unsigned) >> */
            } else {
                t = (int32_t)eCurr.r.lo32;
            }

            invFact = invBandTab[(iEnd - iStart) - 1];
            m_PSInfoSBR->eCurr[m] = MULSHIFT32(t, invFact);
            m_PSInfoSBR->eCurrExp[m] = nScale + 1; /* +1 for invFact = Q31 */
            if (m_PSInfoSBR->eCurrExp[m] > expMax)
                expMax = m_PSInfoSBR->eCurrExp[m];
        }
    } else {
        for (p = 0; p < n; p++) {
            mStart = freqBandTab[p];
            mEnd = freqBandTab[p + 1];
            eCurr.w64 = 0;
            for (i = iStart; i < iEnd; i++) {
                XBuf = m_PSInfoSBR->XBuf[i][mStart];
                for (m = mStart; m < mEnd; m++) {
                    xre = (*XBuf++) >> FBITS_OUT_QMFA;
                    xim = (*XBuf++) >> FBITS_OUT_QMFA;
                    eCurr.w64 = MADD64(eCurr.w64, xre, xre);
                    eCurr.w64 = MADD64(eCurr.w64, xim, xim);
                }
            }

            nScale = 0;
            if (eCurr.r.hi32) {
                nScale = (32 - CLZ(eCurr.r.hi32)) + 1;
                t = (int32_t)(eCurr.r.lo32 >> nScale); /* logical (unsigned) >> */
                t |= eCurr.r.hi32 << (32 - nScale);
            } else if (eCurr.r.lo32 >> 31) {
                nScale = 1;
                t = (int32_t)(eCurr.r.lo32 >> nScale); /* logical (unsigned) >> */
            } else {
                t = (int32_t)eCurr.r.lo32;
            }

            invFact = invBandTab[(iEnd - iStart) - 1];
            invFact = MULSHIFT32(invBandTab[(mEnd - mStart) - 1], invFact) << 1;
            t = MULSHIFT32(t, invFact);

            for (m = mStart; m < mEnd; m++) {
                m_PSInfoSBR->eCurr[m - sbrFreq->kStart] = t;
                m_PSInfoSBR->eCurrExp[m - sbrFreq->kStart] = nScale + 1; /* +1 for invFact = Q31 */
            }
            if (m_PSInfoSBR->eCurrExp[mStart - sbrFreq->kStart] > expMax)
                expMax = m_PSInfoSBR->eCurrExp[mStart - sbrFreq->kStart];
        }
    }
    m_PSInfoSBR->eCurrExpMax = expMax;
}
/***********************************************************************************************************************
 * Function:    GetSMapped
 *
 * Description: calculate SMapped (4.6.18.7.2)
 *
 * Inputs:      initialized SBRGrid struct for this channel
 *              initialized SBRFreq struct for this SCE/CPE block
 *              initialized SBRChan struct for this channel
 *              index of current envelope
 *              index of current QMF band
 *              la flag for this envelope
 *
 * Outputs:     none
 *
 * Return:      1 if a sinusoid is present in this band, 0 if not
 **********************************************************************************************************************/
int32_t AACDecoder::GetSMapped(SBRGrid *sbrGrid, SBRFreq *sbrFreq, SBRChan *sbrChan, int32_t env, int32_t band,
                               int32_t la) {
    int32_t bandStart, bandEnd, oddFlag, r;

    if (sbrGrid->freqRes[env]) {
        /* high resolution */
        bandStart = band;
        bandEnd = band + 1;
    } else {
        /* low resolution (see CalcFreqLow() for mapping) */
        oddFlag = sbrFreq->nHigh & 0x01;
        bandStart = (band > 0 ? 2 * band - oddFlag : 0); /* starting index for freqLow[band] */
        bandEnd = 2 * (band + 1) - oddFlag;              /* ending index for freqLow[band+1] */
    }

    /* sMapped = 1 if sIndexMapped == 1 for any frequency in this band */
    for (band = bandStart; band < bandEnd; band++) {
        if (sbrChan->addHarmonic[1][band]) {
            r = ((sbrFreq->freqHigh[band + 1] + sbrFreq->freqHigh[band]) >> 1);
            if (env >= la || sbrChan->addHarmonic[0][r] == 1)
                return 1;
        }
    }
    return 0;
}

#define GBOOST_MAX 0x2830afd3 /* Q28, 1.584893192 squared */
#define ACC_SCALE 6

/* squared version of table in 4.6.18.7.5 */ /* Q30 (0x80000000 = sentinel for GMAX) */
static const uint32_t limGainTab[4] PROGMEM = {0x20138ca7, 0x40000000, 0x7fb27dce, 0x80000000};

/***********************************************************************************************************************
 * Function:    CalcMaxGain
 *
 * Description: calculate max gain in one limiter band (4.6.18.7.5)
 *
 * Inputs:      initialized SBRHeader struct for this SCE/CPE block
 *              initialized SBRGrid struct for this channel
 *              initialized SBRFreq struct for this SCE/CPE block
 *              index of current channel (0 for SCE, 0 or 1 for CPE)
 *              index of current envelope
 *              index of current limiter band
 *              number of fraction bits in dequantized envelope
 *                (max = Q(FBITS_OUT_DQ_ENV - 6) = Q23, can go negative)
 *
 * Outputs:     updated gainMax, gainMaxFBits, and sumEOrigMapped in PSInfoSBR struct
 *
 * Return:      none
 **********************************************************************************************************************/
void AACDecoder::CalcMaxGain(SBRHeader *sbrHdr, SBRGrid *sbrGrid, SBRFreq *sbrFreq, int32_t ch, int32_t env,
                             int32_t lim, int32_t fbitsDQ) {
    int32_t m, mStart, mEnd, q, z, r;
    int32_t sumEOrigMapped, sumECurr, gainMax, eOMGainMax, envBand;
    uint8_t eCurrExpMax;
    uint8_t *freqBandTab;

    mStart = sbrFreq->freqLimiter[lim]; /* these are offsets from kStart */
    mEnd = sbrFreq->freqLimiter[lim + 1];
    freqBandTab = (sbrGrid->freqRes[env] ? sbrFreq->freqHigh : sbrFreq->freqLow);

    /* calculate max gain to apply to signal in this limiter band */
    sumECurr = 0;
    sumEOrigMapped = 0;
    eCurrExpMax = m_PSInfoSBR->eCurrExpMax;
    eOMGainMax = m_PSInfoSBR->eOMGainMax;
    envBand = m_PSInfoSBR->envBand;
    for (m = mStart; m < mEnd; m++) {
        /* map current QMF band to appropriate envelope band */
        if (m == freqBandTab[envBand + 1] - sbrFreq->kStart) {
            envBand++;
            eOMGainMax = m_PSInfoSBR->envDataDequant[ch][env][envBand] >> ACC_SCALE; /* summing max 48 bands */
        }
        sumEOrigMapped += eOMGainMax;

        /* easy test for overflow on ARM */
        sumECurr += (m_PSInfoSBR->eCurr[m] >> (eCurrExpMax - m_PSInfoSBR->eCurrExp[m]));
        if (sumECurr >> 30) {
            sumECurr >>= 1;
            eCurrExpMax++;
        }
    }
    m_PSInfoSBR->eOMGainMax = eOMGainMax;
    m_PSInfoSBR->envBand = envBand;

    m_PSInfoSBR->gainMaxFBits = 30; /* Q30 tables */
    if (sumECurr == 0) {
        /* any non-zero numerator * 1/EPS_0 is > G_MAX */
        gainMax = (sumEOrigMapped == 0 ? (int32_t)limGainTab[sbrHdr->limiterGains] : (int32_t)0x80000000);
    } else if (sumEOrigMapped == 0) {
        /* 1/(any non-zero denominator) * EPS_0 * limGainTab[x] is appx. 0 */
        gainMax = 0;
    } else {
        /* sumEOrigMapped = Q(fbitsDQ - ACC_SCALE), sumECurr = Q(-eCurrExpMax) */
        gainMax = limGainTab[sbrHdr->limiterGains];
        if (sbrHdr->limiterGains != 3) {
            q = MULSHIFT32(sumEOrigMapped, gainMax); /* Q(fbitsDQ - ACC_SCALE - 2), gainMax = Q30  */
            z = CLZ(sumECurr) - 1;
            r = InvRNormalized(sumECurr << z); /* in =  Q(z - eCurrExpMax), out = Q(29 + 31 - z + eCurrExpMax) */
            gainMax = MULSHIFT32(q, r);        /* Q(29 + 31 - z + eCurrExpMax + fbitsDQ - ACC_SCALE - 2 - 32) */
            m_PSInfoSBR->gainMaxFBits = 26 - z + eCurrExpMax + fbitsDQ - ACC_SCALE;
        }
    }
    m_PSInfoSBR->sumEOrigMapped = sumEOrigMapped;
    m_PSInfoSBR->gainMax = gainMax;
}
/***********************************************************************************************************************
 * Function:    CalcNoiseDivFactors
 *
 * Description: calculate 1/(1+Q) and Q/(1+Q) (4.6.18.7.4; 4.6.18.7.5)
 *
 * Inputs:      dequantized noise floor scalefactor
 *
 * Outputs:     1/(1+Q) and Q/(1+Q), format = Q31
 *
 * Return:      none
 **********************************************************************************************************************/
void AACDecoder::CalcNoiseDivFactors(int32_t q, int32_t *qp1Inv, int32_t *qqp1Inv) {
    int32_t z, qp1, t, s;

    /* 1 + Q_orig */
    qp1 = (q >> 1);
    qp1 += (1 << (FBITS_OUT_DQ_NOISE - 1)); /* >> 1 to avoid overflow when adding 1.0 */
    z = CLZ(qp1) - 1;                       /* z <= 31 - FBITS_OUT_DQ_NOISE */
    qp1 <<= z;                              /* Q(FBITS_OUT_DQ_NOISE + z) = Q31 * 2^-(31 - (FBITS_OUT_DQ_NOISE + z)) */
    t = InvRNormalized(qp1) << 1;           /* Q30 * 2^(31 - (FBITS_OUT_DQ_NOISE + z)), guaranteed not to overflow */

    /* normalize to Q31 */
    s = (31 - (FBITS_OUT_DQ_NOISE - 1) - z - 1); /* clearly z >= 0, z <= (30 - (FBITS_OUT_DQ_NOISE - 1)) */
    *qp1Inv = (t >> s);                          /* s = [0, 31 - FBITS_OUT_DQ_NOISE] */
    *qqp1Inv = MULSHIFT32(t, q) << (32 - FBITS_OUT_DQ_NOISE - s);
}
/***********************************************************************************************************************
 * Function:    CalcComponentGains
 *
 * Description: calculate gain of envelope, sinusoids, and noise in one limiter band
 *                (4.6.18.7.5)
 *
 * Inputs:      initialized SBRHeader struct for this SCE/CPE block
 *              initialized SBRGrid struct for this channel
 *              initialized SBRFreq struct for this SCE/CPE block
 *              initialized SBRChan struct for this channel
 *              index of current channel (0 for SCE, 0 or 1 for CPE)
 *              index of current envelope
 *              index of current limiter band
 *              number of fraction bits in dequantized envelope
 *
 * Outputs:     gains for envelope, sinusoids and noise
 *              number of fraction bits for envelope gain
 *              sum of the total gain for each component in this band
 *              other updated state variables
 *
 * Return:      none
 **********************************************************************************************************************/
void AACDecoder::CalcComponentGains(SBRGrid *sbrGrid, SBRFreq *sbrFreq, SBRChan *sbrChan, int32_t ch, int32_t env,
                                    int32_t lim, int32_t fbitsDQ) {
    int32_t d, m, mStart, mEnd, q, qm, noiseFloor, sIndexMapped;
    int32_t shift, eCurr, maxFlag, gainMax, gainMaxFBits;
    int32_t gain, sm, z, r, fbitsGain, gainScale;
    uint8_t *freqBandTab;

    mStart = sbrFreq->freqLimiter[lim]; /* these are offsets from kStart */
    mEnd = sbrFreq->freqLimiter[lim + 1];

    gainMax = m_PSInfoSBR->gainMax;
    gainMaxFBits = m_PSInfoSBR->gainMaxFBits;

    d = (env == m_PSInfoSBR->la || env == sbrChan->laPrev ? 0 : 1);
    freqBandTab = (sbrGrid->freqRes[env] ? sbrFreq->freqHigh : sbrFreq->freqLow);

    /* figure out which noise floor this envelope is in (only 1 or 2 noise floors allowed) */
    noiseFloor = 0;
    if (sbrGrid->numNoiseFloors == 2 && sbrGrid->noiseTimeBorder[1] <= sbrGrid->envTimeBorder[env])
        noiseFloor++;

    m_PSInfoSBR->sumECurrGLim = 0;
    m_PSInfoSBR->sumSM = 0;
    m_PSInfoSBR->sumQM = 0;
    /* calculate energy of noise to add in this limiter band */
    for (m = mStart; m < mEnd; m++) {
        if (m == sbrFreq->freqNoise[m_PSInfoSBR->noiseFloorBand + 1] - sbrFreq->kStart) {
            /* map current QMF band to appropriate noise floor band (NOTE: freqLimiter[0] == freqLow[0] = freqHigh[0]) */
            m_PSInfoSBR->noiseFloorBand++;
            CalcNoiseDivFactors(m_PSInfoSBR->noiseDataDequant[ch][noiseFloor][m_PSInfoSBR->noiseFloorBand],
                                &(m_PSInfoSBR->qp1Inv), &(m_PSInfoSBR->qqp1Inv));
        }
        if (m == sbrFreq->freqHigh[m_PSInfoSBR->highBand + 1] - sbrFreq->kStart)
            m_PSInfoSBR->highBand++;
        if (m == freqBandTab[m_PSInfoSBR->sBand + 1] - sbrFreq->kStart) {
            m_PSInfoSBR->sBand++;
            m_PSInfoSBR->sMapped = GetSMapped(sbrGrid, sbrFreq, sbrChan, env, m_PSInfoSBR->sBand, m_PSInfoSBR->la);
        }

        /* get sIndexMapped for this QMF subband */
        sIndexMapped = 0;
        r = ((sbrFreq->freqHigh[m_PSInfoSBR->highBand + 1] + sbrFreq->freqHigh[m_PSInfoSBR->highBand]) >> 1);
        if (m + sbrFreq->kStart == r) {
            /* r = center frequency, deltaStep = (env >= la || sIndexMapped'(r, numEnv'-1) == 1) */
            if (env >= m_PSInfoSBR->la || sbrChan->addHarmonic[0][r] == 1)
                sIndexMapped = sbrChan->addHarmonic[1][m_PSInfoSBR->highBand];
        }

        /* save sine flags from last envelope in this frame:
         *   addHarmonic[0][0...63] = saved sine present flag from previous frame, for each QMF subband
         *   addHarmonic[1][0...nHigh-1] = addHarmonic bit from current frame, for each high-res frequency band
         * from MPEG reference code - slightly different from spec
         *   (sIndexMapped'(m,LE'-1) can still be 0 when numEnv == psi->la)
         */
        if (env == sbrGrid->numEnv - 1) {
            if (m + sbrFreq->kStart == r)
                sbrChan->addHarmonic[0][m + sbrFreq->kStart] = sbrChan->addHarmonic[1][m_PSInfoSBR->highBand];
            else
                sbrChan->addHarmonic[0][m + sbrFreq->kStart] = 0;
        }

        gain = m_PSInfoSBR->envDataDequant[ch][env][m_PSInfoSBR->sBand];
        qm = MULSHIFT32(gain, m_PSInfoSBR->qqp1Inv) << 1;
        sm = (sIndexMapped ? MULSHIFT32(gain, m_PSInfoSBR->qp1Inv) << 1 : 0);

        /* three cases: (sMapped == 0 && delta == 1), (sMapped == 0 && delta == 0), (sMapped == 1) */
        if (d == 1 && m_PSInfoSBR->sMapped == 0)
            gain = MULSHIFT32(m_PSInfoSBR->qp1Inv, gain) << 1;
        else if (m_PSInfoSBR->sMapped != 0)
            gain = MULSHIFT32(m_PSInfoSBR->qqp1Inv, gain) << 1;

        /* gain, qm, sm = Q(fbitsDQ), gainMax = Q(fbitsGainMax) */
        eCurr = m_PSInfoSBR->eCurr[m];
        if (eCurr) {
            z = CLZ(eCurr) - 1;
            r = InvRNormalized(eCurr << z);  /* in = Q(z - eCurrExp), out = Q(29 + 31 - z + eCurrExp) */
            gainScale = MULSHIFT32(gain, r); /* out = Q(29 + 31 - z + eCurrExp + fbitsDQ - 32) */
            fbitsGain = 29 + 31 - z + m_PSInfoSBR->eCurrExp[m] + fbitsDQ - 32;
        } else {
            /* if eCurr == 0, then gain is unchanged (divide by EPS = 1) */
            gainScale = gain;
            fbitsGain = fbitsDQ;
        }

        /* see if gain for this band exceeds max gain */
        maxFlag = 0;
        if (gainMax != (int32_t)0x80000000) {
            if (fbitsGain >= gainMaxFBits) {
                shift = MIN(fbitsGain - gainMaxFBits, (int32_t)31);
                maxFlag = ((gainScale >> shift) > gainMax ? 1 : 0);
            } else {
                shift = MIN(gainMaxFBits - fbitsGain, (int32_t)31);
                maxFlag = (gainScale > (gainMax >> shift) ? 1 : 0);
            }
        }

        if (maxFlag) {
            /* gainScale > gainMax, calculate ratio with 32/16 division */
            q = 0;
            r = gainScale; /* guaranteed > 0, else maxFlag could not have been set */
            z = CLZ(r);
            if (z < 16) {
                q = 16 - z;
                r >>= q; /* out = Q(fbitsGain - q) */
            }

            z = CLZ(gainMax) - 1;
            r = (gainMax << z) / r;                   /* out = Q((fbitsGainMax + z) - (fbitsGain - q)) */
            q = (gainMaxFBits + z) - (fbitsGain - q); /* r = Q(q) */
            if (q > 30) {
                r >>= MIN(q - 30, (int32_t)31);
            } else {
                z = MIN((int32_t)30 - q, (int32_t)30);
                r = CLIP_2N_SHIFT30(r, z); /* let r = Q30 since range = [0.0, 1.0) (clip to 0x3fffffff = 0.99999) */
            }

            qm = MULSHIFT32(qm, r) << 2;
            gain = MULSHIFT32(gain, r) << 2;
            m_PSInfoSBR->gLimBuf[m] = gainMax;
            m_PSInfoSBR->gLimFbits[m] = gainMaxFBits;
        } else {
            m_PSInfoSBR->gLimBuf[m] = gainScale;
            m_PSInfoSBR->gLimFbits[m] = fbitsGain;
        }

        /* sumSM, sumQM, sumECurrGLim = Q(fbitsDQ - ACC_SCALE) */
        m_PSInfoSBR->smBuf[m] = sm;
        m_PSInfoSBR->sumSM += (sm >> ACC_SCALE);

        m_PSInfoSBR->qmLimBuf[m] = qm;
        if (env != m_PSInfoSBR->la && env != sbrChan->laPrev && sm == 0)
            m_PSInfoSBR->sumQM += (qm >> ACC_SCALE);

        /* eCurr * gain^2 same as gain^2, before division by eCurr
         * (but note that gain != 0 even if eCurr == 0, since it's divided by eps)
         */
        if (eCurr)
            m_PSInfoSBR->sumECurrGLim += (gain >> ACC_SCALE);
    }
}
/***********************************************************************************************************************
 * Function:    ApplyBoost
 *
 * Description: calculate and apply boost factor for envelope, sinusoids, and noise
 *                in this limiter band (4.6.18.7.5)
 *
 * Inputs:      initialized SBRFreq struct for this SCE/CPE block
 *              index of current limiter band
 *              number of fraction bits in dequantized envelope
 *
 * Outputs:     envelope gain, sinusoids and noise after scaling by gBoost
 *              format = Q(FBITS_GLIM_BOOST) for envelope gain,
 *                     = Q(FBITS_QLIM_BOOST) for noise
 *                     = Q(FBITS_OUT_QMFA) for sinusoids
 *
 * Return:      none
 *
 * Notes:       after scaling, each component has at least 1 GB
 **********************************************************************************************************************/
void AACDecoder::ApplyBoost(SBRFreq *sbrFreq, int32_t lim, int32_t fbitsDQ) {
    int32_t m, mStart, mEnd, q, z, r;
    int32_t sumEOrigMapped, gBoost;

    mStart = sbrFreq->freqLimiter[lim]; /* these are offsets from kStart */
    mEnd = sbrFreq->freqLimiter[lim + 1];

    sumEOrigMapped = m_PSInfoSBR->sumEOrigMapped >> 1;
    r = (m_PSInfoSBR->sumECurrGLim >> 1) + (m_PSInfoSBR->sumSM >> 1) +
        (m_PSInfoSBR->sumQM >> 1); /* 1 GB fine (sm and qm are mutually exclusive in acc) */
    if (r < (1 << (31 - 28))) {
        /* any non-zero numerator * 1/EPS_0 is > GBOOST_MAX
         * round very small r to zero to avoid scaling problems
         */
        gBoost = (sumEOrigMapped == 0 ? (1 << 28) : GBOOST_MAX);
        z = 0;
    } else if (sumEOrigMapped == 0) {
        /* 1/(any non-zero denominator) * EPS_0 is appx. 0 */
        gBoost = 0;
        z = 0;
    } else {
        /* numerator (sumEOrigMapped) and denominator (r) have same Q format (before << z) */
        z = CLZ(r) - 1; /* z = [0, 27] */
        r = InvRNormalized(r << z);
        gBoost = MULSHIFT32(sumEOrigMapped, r);
    }

    /* gBoost = Q(28 - z) */
    if (gBoost > (GBOOST_MAX >> z)) {
        gBoost = GBOOST_MAX;
        z = 0;
    }
    gBoost <<= z; /* gBoost = Q28, minimum 1 GB */

    /* convert gain, noise, sinusoids to fixed Q format, clipping if necessary
     *   (rare, usually only happens at very low bitrates, introduces slight
     *    distortion into final HF mapping, but should be inaudible)
     */
    for (m = mStart; m < mEnd; m++) {
        /* let gLimBoost = Q24, since in practice the max values are usually 16 to 20
         *   unless limiterGains == 3 (limiter off) and eCurr ~= 0 (i.e. huge gain, but only
         *   because the envelope has 0 power anyway)
         */
        q = MULSHIFT32(m_PSInfoSBR->gLimBuf[m], gBoost) << 2; /* Q(gLimFbits) * Q(28) --> Q(gLimFbits[m]-2) */
        r = SqrtFix(q, m_PSInfoSBR->gLimFbits[m] - 2, &z);
        z -= FBITS_GLIM_BOOST;
        if (z >= 0) {
            m_PSInfoSBR->gLimBoost[m] = r >> MIN(z, (int32_t)31);
        } else {
            z = MIN((int32_t)30, -z);
            r = CLIP_2N_SHIFT30(r, z);
            m_PSInfoSBR->gLimBoost[m] = r;
        }

        q = MULSHIFT32(m_PSInfoSBR->qmLimBuf[m], gBoost) << 2; /* Q(fbitsDQ) * Q(28) --> Q(fbitsDQ-2) */
        r = SqrtFix(q, fbitsDQ - 2, &z);
        z -= FBITS_QLIM_BOOST; /* << by 14, since integer sqrt of x < 2^16, and we want to leave 1 GB */
        if (z >= 0) {
            m_PSInfoSBR->qmLimBoost[m] = r >> MIN((int32_t)31, z);
        } else {
            z = MIN((int32_t)30, -z);
            r = CLIP_2N_SHIFT30(r, z);
            m_PSInfoSBR->qmLimBoost[m] = r;
        }

        q = MULSHIFT32(m_PSInfoSBR->smBuf[m], gBoost) << 2; /* Q(fbitsDQ) * Q(28) --> Q(fbitsDQ-2) */
        r = SqrtFix(q, fbitsDQ - 2, &z);
        z -= FBITS_OUT_QMFA; /* justify for adding to signal (xBuf) later */
        if (z >= 0) {
            m_PSInfoSBR->smBoost[m] = r >> MIN((int32_t)31, z);
        } else {
            z = MIN((int32_t)30, -z);
            r = CLIP_2N_SHIFT30(r, z);
            m_PSInfoSBR->smBoost[m] = r;
        }
    }
}
/***********************************************************************************************************************
 * Function:    CalcGain
 *
 * Description: calculate and apply proper gain to HF components in one envelope
 *                (4.6.18.7.5)
 *
 * Inputs:      initialized SBRHeader struct for this SCE/CPE block
 *              initialized SBRGrid struct for this channel
 *              initialized SBRFreq struct for this SCE/CPE block
 *              initialized SBRChan struct for this channel
 *              index of current channel (0 for SCE, 0 or 1 for CPE)
 *              index of current envelope
 *
 * Outputs:     envelope gain, sinusoids and noise after scaling
 *
 * Return:      none
 **********************************************************************************************************************/
void AACDecoder::CalcGain(SBRHeader *sbrHdr, SBRGrid *sbrGrid, SBRFreq *sbrFreq, SBRChan *sbrChan, int32_t ch,
                          int32_t env) {
    int32_t lim, fbitsDQ;

    /* initialize to -1 so that mapping limiter bands to env/noise bands works right on first pass */
    m_PSInfoSBR->envBand = -1;
    m_PSInfoSBR->noiseFloorBand = -1;
    m_PSInfoSBR->sBand = -1;
    m_PSInfoSBR->highBand = -1;

    fbitsDQ = (FBITS_OUT_DQ_ENV - m_PSInfoSBR->envDataDequantScale[ch][env]); /* Q(29 - optional scalefactor) */
    for (lim = 0; lim < sbrFreq->nLimiter; lim++) {
        /* the QMF bands are divided into lim regions (consecutive, non-overlapping) */
        CalcMaxGain(sbrHdr, sbrGrid, sbrFreq, ch, env, lim, fbitsDQ);
        CalcComponentGains(sbrGrid, sbrFreq, sbrChan, ch, env, lim, fbitsDQ);
        ApplyBoost(sbrFreq, lim, fbitsDQ);
    }
}

/* hSmooth table from 4.7.18.7.6, format = Q31 */
static const int32_t hSmoothCoef[MAX_NUM_SMOOTH_COEFS] PROGMEM = {
    0x2aaaaaab, 0x2697a512, 0x1becfa68, 0x0ebdb043, 0x04130598,
};

/***********************************************************************************************************************
 * Function:    MapHF
 *
 * Description: map HF components to proper QMF bands, with optional gain smoothing
 *                filter (4.6.18.7.6)
 *
 * Inputs:      initialized SBRHeader struct for this SCE/CPE block
 *              initialized SBRGrid struct for this channel
 *              initialized SBRFreq struct for this SCE/CPE block
 *              initialized SBRChan struct for this channel
 *              index of current envelope
 *              reset flag (can be non-zero for first envelope only)
 *
 * Outputs:     complete reconstructed subband QMF samples for this envelope
 *
 * Return:      none
 *
 * Notes:       ensures that output has >= MIN_GBITS_IN_QMFS guard bits,
 *                so it's not necessary to check anything in the synth QMF
 **********************************************************************************************************************/
void AACDecoder::MapHF(SBRHeader *sbrHdr, SBRGrid *sbrGrid, SBRFreq *sbrFreq, SBRChan *sbrChan, int32_t env,
                       int32_t hfReset) {
    int32_t noiseTabIndex, sinIndex, gainNoiseIndex, hSL;
    int32_t i, iStart, iEnd, m, idx, j, s, n, smre, smim;
    int32_t gFilt, qFilt, xre, xim, gbMask, gbIdx;
    int32_t *XBuf;

    noiseTabIndex = sbrChan->noiseTabIndex;
    sinIndex = sbrChan->sinIndex;
    gainNoiseIndex = sbrChan->gainNoiseIndex; /* oldest entries in filter delay buffer */

    if (hfReset)
        noiseTabIndex = 2; /* starts at 1, double since complex */
    hSL = (sbrHdr->smoothMode ? 0 : 4);

    if (hfReset) {
        for (i = 0; i < hSL; i++) {
            for (m = 0; m < sbrFreq->numQMFBands; m++) {
                sbrChan->gTemp[gainNoiseIndex][m] = m_PSInfoSBR->gLimBoost[m];
                sbrChan->qTemp[gainNoiseIndex][m] = m_PSInfoSBR->qmLimBoost[m];
            }
            gainNoiseIndex++;
            if (gainNoiseIndex == MAX_NUM_SMOOTH_COEFS)
                gainNoiseIndex = 0;
        }
        ASSERT(env == 0); /* should only be reset when env == 0 */
    }

    iStart = sbrGrid->envTimeBorder[env];
    iEnd = sbrGrid->envTimeBorder[env + 1];
    for (i = iStart; i < iEnd; i++) {
        /* save new values in temp buffers (delay)
         * we only store MAX_NUM_SMOOTH_COEFS most recent values,
         *   so don't keep storing the same value over and over
         */
        if (i - iStart < MAX_NUM_SMOOTH_COEFS) {
            for (m = 0; m < sbrFreq->numQMFBands; m++) {
                sbrChan->gTemp[gainNoiseIndex][m] = m_PSInfoSBR->gLimBoost[m];
                sbrChan->qTemp[gainNoiseIndex][m] = m_PSInfoSBR->qmLimBoost[m];
            }
        }

        /* see 4.6.18.7.6 */
        XBuf = m_PSInfoSBR->XBuf[i + HF_ADJ][sbrFreq->kStart];
        gbMask = 0;
        for (m = 0; m < sbrFreq->numQMFBands; m++) {
            if (env == m_PSInfoSBR->la || env == sbrChan->laPrev) {
                /* no smoothing filter for gain, and qFilt = 0 (only need to do once) */
                if (i == iStart) {
                    m_PSInfoSBR->gFiltLast[m] = sbrChan->gTemp[gainNoiseIndex][m];
                    m_PSInfoSBR->qFiltLast[m] = 0;
                }
            } else if (hSL == 0) {
                /* no smoothing filter for gain, (only need to do once) */
                if (i == iStart) {
                    m_PSInfoSBR->gFiltLast[m] = sbrChan->gTemp[gainNoiseIndex][m];
                    m_PSInfoSBR->qFiltLast[m] = sbrChan->qTemp[gainNoiseIndex][m];
                }
            } else {
                /* apply smoothing filter to gain and noise (after MAX_NUM_SMOOTH_COEFS, it's always the same) */
                if (i - iStart < MAX_NUM_SMOOTH_COEFS) {
                    gFilt = 0;
                    qFilt = 0;
                    idx = gainNoiseIndex;
                    for (j = 0; j < MAX_NUM_SMOOTH_COEFS; j++) {
                        /* sum(abs(hSmoothCoef[j])) for all j < 1.0 */
                        gFilt += MULSHIFT32(sbrChan->gTemp[idx][m], hSmoothCoef[j]);
                        qFilt += MULSHIFT32(sbrChan->qTemp[idx][m], hSmoothCoef[j]);
                        idx--;
                        if (idx < 0)
                            idx += MAX_NUM_SMOOTH_COEFS;
                    }
                    m_PSInfoSBR->gFiltLast[m] =
                        gFilt << 1; /* restore to Q(FBITS_GLIM_BOOST) (gain of filter < 1.0, so no overflow) */
                    m_PSInfoSBR->qFiltLast[m] = qFilt << 1; /* restore to Q(FBITS_QLIM_BOOST) */
                }
            }

            if (m_PSInfoSBR->smBoost[m] != 0) {
                /* add scaled signal and sinusoid, don't add noise (qFilt = 0) */
                smre = m_PSInfoSBR->smBoost[m];
                smim = smre;

                /* sinIndex:  [0] xre += sm   [1] xim += sm*s   [2] xre -= sm   [3] xim -= sm*s  */
                s = (sinIndex >> 1); /* if 2 or 3, flip sign to subtract sm */
                s <<= 31;
                smre ^= (s >> 31);
                smre -= (s >> 31);
                s ^= ((m + sbrFreq->kStart) << 31);
                smim ^= (s >> 31);
                smim -= (s >> 31);

                /* if sinIndex == 0 or 2, smim = 0; if sinIndex == 1 or 3, smre = 0 */
                s = sinIndex << 31;
                smim &= (s >> 31);
                s ^= 0x80000000;
                smre &= (s >> 31);

                noiseTabIndex += 2; /* noise filtered by 0, but still need to bump index */
            } else {
                /* add scaled signal and scaled noise */
                qFilt = m_PSInfoSBR->qFiltLast[m];
                n = noiseTab[noiseTabIndex++];
                smre = MULSHIFT32(n, qFilt) >> (FBITS_QLIM_BOOST - 1 - FBITS_OUT_QMFA);

                n = noiseTab[noiseTabIndex++];
                smim = MULSHIFT32(n, qFilt) >> (FBITS_QLIM_BOOST - 1 - FBITS_OUT_QMFA);
            }
            noiseTabIndex &= 1023; /* 512 complex numbers */

            gFilt = m_PSInfoSBR->gFiltLast[m];
            xre = MULSHIFT32(gFilt, XBuf[0]);
            xim = MULSHIFT32(gFilt, XBuf[1]);
            xre = CLIP_2N_SHIFT30(xre, 32 - FBITS_GLIM_BOOST);
            xim = CLIP_2N_SHIFT30(xim, 32 - FBITS_GLIM_BOOST);

            xre += smre;
            *XBuf++ = xre;
            xim += smim;
            *XBuf++ = xim;

            gbMask |= FASTABS(xre);
            gbMask |= FASTABS(xim);
        }
        /* update circular buffer index */
        gainNoiseIndex++;
        if (gainNoiseIndex == MAX_NUM_SMOOTH_COEFS)
            gainNoiseIndex = 0;

        sinIndex++;
        sinIndex &= 3;

        /* ensure MIN_GBITS_IN_QMFS guard bits in output
         * almost never occurs in practice, but checking here makes synth QMF logic very simple
         */
        if (gbMask >> (31 - MIN_GBITS_IN_QMFS)) {
            XBuf = m_PSInfoSBR->XBuf[i + HF_ADJ][sbrFreq->kStart];
            for (m = 0; m < sbrFreq->numQMFBands; m++) {
                xre = XBuf[0];
                xim = XBuf[1];
                xre = CLIP_2N(xre, (31 - MIN_GBITS_IN_QMFS));
                xim = CLIP_2N(xim, (31 - MIN_GBITS_IN_QMFS));
                *XBuf++ = xre;
                *XBuf++ = xim;
            }
            gbMask = CLIP_2N(gbMask, (31 - MIN_GBITS_IN_QMFS));
        }
        gbIdx = ((i + HF_ADJ) >> 5) & 0x01;
        sbrChan->gbMask[gbIdx] |= gbMask;
    }
    sbrChan->noiseTabIndex = noiseTabIndex;
    sbrChan->sinIndex = sinIndex;
    sbrChan->gainNoiseIndex = gainNoiseIndex;
}
/***********************************************************************************************************************
 * Function:    AdjustHighFreq
 *
 * Description: adjust high frequencies and add noise and sinusoids (4.6.18.7)
 *
 * Inputs:      initialized SBRHeader struct for this SCE/CPE block
 *              initialized SBRGrid struct for this channel
 *              initialized SBRFreq struct for this SCE/CPE block
 *              initialized SBRChan struct for this channel
 *              index of current channel (0 for SCE, 0 or 1 for CPE)
 *
 * Outputs:     complete reconstructed subband QMF samples for this channel
 *
 * Return:      none
 **********************************************************************************************************************/
void AACDecoder::AdjustHighFreq(SBRHeader *sbrHdr, SBRGrid *sbrGrid, SBRFreq *sbrFreq, SBRChan *sbrChan, int32_t ch) {
    int32_t i, env, hfReset;
    uint8_t frameClass, pointer;

    frameClass = sbrGrid->frameClass;
    pointer = sbrGrid->pointer;

    /* derive la from table 4.159 */
    if ((frameClass == SBR_GRID_FIXVAR || frameClass == SBR_GRID_VARVAR) && pointer > 0)
        m_PSInfoSBR->la = sbrGrid->numEnv + 1 - pointer;
    else if (frameClass == SBR_GRID_VARFIX && pointer > 1)
        m_PSInfoSBR->la = pointer - 1;
    else
        m_PSInfoSBR->la = -1;

    /* for each envelope, estimate gain and adjust SBR QMF bands */
    hfReset = sbrChan->reset;
    for (env = 0; env < sbrGrid->numEnv; env++) {
        EstimateEnvelope(sbrHdr, sbrGrid, sbrFreq, env);
        CalcGain(sbrHdr, sbrGrid, sbrFreq, sbrChan, ch, env);
        MapHF(sbrHdr, sbrGrid, sbrFreq, sbrChan, env, hfReset);
        hfReset = 0; /* only set for first envelope after header reset */
    }

    /* set saved sine flags to 0 for QMF bands outside of current frequency range */
    for (i = 0; i < sbrFreq->freqLimiter[0] + sbrFreq->kStart; i++)
        sbrChan->addHarmonic[0][i] = 0;
    for (i = sbrFreq->freqLimiter[sbrFreq->nLimiter] + sbrFreq->kStart; i < 64; i++)
        sbrChan->addHarmonic[0][i] = 0;
    sbrChan->addHarmonicFlag[0] = sbrChan->addHarmonicFlag[1];

    /* save la for next frame */
    if (m_PSInfoSBR->la == sbrGrid->numEnv)
        sbrChan->laPrev = 0;
    else
        sbrChan->laPrev = -1;
}
/***********************************************************************************************************************
 * Function:    CalcCovariance1
 *
 * Description: calculate covariance matrix for p01, p12, p11, p22 (4.6.18.6.2)
 *
 * Inputs:      buffer of low-freq samples, starting at time index 0,
 *                freq index = patch subband
 *
 * Outputs:     complex covariance elements p01re, p01im, p12re, p12im, p11re, p22re
 *                (p11im = p22im = 0)
 *              format = integer (Q0) * 2^N, with scalefactor N >= 0
 *
 * Return:      scalefactor N
 *
 * Notes:       outputs are normalized to have 1 GB (sign in at least top 2 bits)
 **********************************************************************************************************************/
int32_t AACDecoder::CalcCovariance1(int32_t *XBuf, int32_t *p01reN, int32_t *p01imN, int32_t *p12reN, int32_t *p12imN,
                                    int32_t *p11reN, int32_t *p22reN) {
    int32_t accBuf[2 * 6];
    int32_t n, z, s, loShift, hiShift, gbMask;
    U64 p01re, p01im, p12re, p12im, p11re, p22re;

    CVKernel1(XBuf, accBuf);
    p01re.r.lo32 = accBuf[0];
    p01re.r.hi32 = accBuf[1];
    p01im.r.lo32 = accBuf[2];
    p01im.r.hi32 = accBuf[3];
    p11re.r.lo32 = accBuf[4];
    p11re.r.hi32 = accBuf[5];
    p12re.r.lo32 = accBuf[6];
    p12re.r.hi32 = accBuf[7];
    p12im.r.lo32 = accBuf[8];
    p12im.r.hi32 = accBuf[9];
    p22re.r.lo32 = accBuf[10];
    p22re.r.hi32 = accBuf[11];

    /* 64-bit accumulators now have 2*FBITS_OUT_QMFA fraction bits
     * want to scale them down to integers (32-bit signed, Q0)
     *   with scale factor of 2^n, n >= 0
     * leave 2 GB's for calculating determinant, so take top 30 non-zero bits
     */
    gbMask = ((p01re.r.hi32) ^ (p01re.r.hi32 >> 31)) | ((p01im.r.hi32) ^ (p01im.r.hi32 >> 31));
    gbMask |= ((p12re.r.hi32) ^ (p12re.r.hi32 >> 31)) | ((p12im.r.hi32) ^ (p12im.r.hi32 >> 31));
    gbMask |= ((p11re.r.hi32) ^ (p11re.r.hi32 >> 31)) | ((p22re.r.hi32) ^ (p22re.r.hi32 >> 31));
    if (gbMask == 0) {
        s = p01re.r.hi32 >> 31;
        gbMask = (p01re.r.lo32 ^ s) - s;
        s = p01im.r.hi32 >> 31;
        gbMask |= (p01im.r.lo32 ^ s) - s;
        s = p12re.r.hi32 >> 31;
        gbMask |= (p12re.r.lo32 ^ s) - s;
        s = p12im.r.hi32 >> 31;
        gbMask |= (p12im.r.lo32 ^ s) - s;
        s = p11re.r.hi32 >> 31;
        gbMask |= (p11re.r.lo32 ^ s) - s;
        s = p22re.r.hi32 >> 31;
        gbMask |= (p22re.r.lo32 ^ s) - s;
        z = 32 + CLZ(gbMask);
    } else {
        gbMask = FASTABS(p01re.r.hi32) | FASTABS(p01im.r.hi32);
        gbMask |= FASTABS(p12re.r.hi32) | FASTABS(p12im.r.hi32);
        gbMask |= FASTABS(p11re.r.hi32) | FASTABS(p22re.r.hi32);
        z = CLZ(gbMask);
    }

    n = 64 - z; /* number of non-zero bits in bottom of 64-bit word */
    if (n <= 30) {
        loShift = (30 - n);
        *p01reN = p01re.r.lo32 << loShift;
        *p01imN = p01im.r.lo32 << loShift;
        *p12reN = p12re.r.lo32 << loShift;
        *p12imN = p12im.r.lo32 << loShift;
        *p11reN = p11re.r.lo32 << loShift;
        *p22reN = p22re.r.lo32 << loShift;
        return -(loShift + 2 * FBITS_OUT_QMFA);
    } else if (n < 32 + 30) {
        loShift = (n - 30);
        hiShift = 32 - loShift;
        *p01reN = (p01re.r.hi32 << hiShift) | (p01re.r.lo32 >> loShift);
        *p01imN = (p01im.r.hi32 << hiShift) | (p01im.r.lo32 >> loShift);
        *p12reN = (p12re.r.hi32 << hiShift) | (p12re.r.lo32 >> loShift);
        *p12imN = (p12im.r.hi32 << hiShift) | (p12im.r.lo32 >> loShift);
        *p11reN = (p11re.r.hi32 << hiShift) | (p11re.r.lo32 >> loShift);
        *p22reN = (p22re.r.hi32 << hiShift) | (p22re.r.lo32 >> loShift);
        return (loShift - 2 * FBITS_OUT_QMFA);
    } else {
        hiShift = n - (32 + 30);
        *p01reN = p01re.r.hi32 >> hiShift;
        *p01imN = p01im.r.hi32 >> hiShift;
        *p12reN = p12re.r.hi32 >> hiShift;
        *p12imN = p12im.r.hi32 >> hiShift;
        *p11reN = p11re.r.hi32 >> hiShift;
        *p22reN = p22re.r.hi32 >> hiShift;
        return (32 - 2 * FBITS_OUT_QMFA - hiShift);
    }

    return 0;
}
/***********************************************************************************************************************
 * Function:    CalcCovariance2
 *
 * Description: calculate covariance matrix for p02 (4.6.18.6.2)
 *
 * Inputs:      buffer of low-freq samples, starting at time index = 0,
 *                freq index = patch subband
 *
 * Outputs:     complex covariance element p02re, p02im
 *              format = integer (Q0) * 2^N, with scalefactor N >= 0
 *
 * Return:      scalefactor N
 *
 * Notes:       outputs are normalized to have 1 GB (sign in at least top 2 bits)
 **********************************************************************************************************************/
int32_t AACDecoder::CalcCovariance2(int32_t *XBuf, int32_t *p02reN, int32_t *p02imN) {
    U64 p02re, p02im;
    int32_t n, z, s, loShift, hiShift, gbMask;
    int32_t accBuf[2 * 2];

    CVKernel2(XBuf, accBuf);
    p02re.r.lo32 = accBuf[0];
    p02re.r.hi32 = accBuf[1];
    p02im.r.lo32 = accBuf[2];
    p02im.r.hi32 = accBuf[3];

    /* 64-bit accumulators now have 2*FBITS_OUT_QMFA fraction bits
     * want to scale them down to integers (32-bit signed, Q0)
     *   with scale factor of 2^n, n >= 0
     * leave 1 GB for calculating determinant, so take top 30 non-zero bits
     */
    gbMask = ((p02re.r.hi32) ^ (p02re.r.hi32 >> 31)) | ((p02im.r.hi32) ^ (p02im.r.hi32 >> 31));
    if (gbMask == 0) {
        s = p02re.r.hi32 >> 31;
        gbMask = (p02re.r.lo32 ^ s) - s;
        s = p02im.r.hi32 >> 31;
        gbMask |= (p02im.r.lo32 ^ s) - s;
        z = 32 + CLZ(gbMask);
    } else {
        gbMask = FASTABS(p02re.r.hi32) | FASTABS(p02im.r.hi32);
        z = CLZ(gbMask);
    }
    n = 64 - z; /* number of non-zero bits in bottom of 64-bit word */

    if (n <= 30) {
        loShift = (30 - n);
        *p02reN = p02re.r.lo32 << loShift;
        *p02imN = p02im.r.lo32 << loShift;
        return -(loShift + 2 * FBITS_OUT_QMFA);
    } else if (n < 32 + 30) {
        loShift = (n - 30);
        hiShift = 32 - loShift;
        *p02reN = (p02re.r.hi32 << hiShift) | (p02re.r.lo32 >> loShift);
        *p02imN = (p02im.r.hi32 << hiShift) | (p02im.r.lo32 >> loShift);
        return (loShift - 2 * FBITS_OUT_QMFA);
    } else {
        hiShift = n - (32 + 30);
        *p02reN = p02re.r.hi32 >> hiShift;
        *p02imN = p02im.r.hi32 >> hiShift;
        return (32 - 2 * FBITS_OUT_QMFA - hiShift);
    }

    return 0;
}
/***********************************************************************************************************************
 * Function:    CalcLPCoefs
 *
 * Description: calculate linear prediction coefficients for one subband (4.6.18.6.2)
 *
 * Inputs:      buffer of low-freq samples, starting at time index = 0,
 *                freq index = patch subband
 *              number of guard bits in input sample buffer
 *
 * Outputs:     complex LP coefficients a0re, a0im, a1re, a1im, format = Q29
 *
 * Return:      none
 *
 * Notes:       output coefficients (a0re, a0im, a1re, a1im) clipped to range (-4, 4)
 *              if the comples coefficients have magnitude >= 4.0, they are all
 *                set to 0 (see spec)
 **********************************************************************************************************************/
void AACDecoder::CalcLPCoefs(int32_t *XBuf, int32_t *a0re, int32_t *a0im, int32_t *a1re, int32_t *a1im, int32_t gb) {
    int32_t zFlag, n1, n2, nd, d, dInv, tre, tim;
    int32_t p01re, p01im, p02re, p02im, p12re, p12im, p11re, p22re;

    /* pre-scale to avoid overflow - probably never happens in practice (see QMFA)
     *   max bit growth per accumulator = 38*2 = 76 mul-adds (X * X)
     *   using 64-bit MADD, so if X has n guard bits, X*X has 2n+1 guard bits
     *   gain 1 extra sign bit per multiply, so ensure ceil(log2(76/2) / 2) = 3 guard bits on inputs
     */
    if (gb < 3) {
        nd = 3 - gb;
        for (n1 = (NUM_TIME_SLOTS * SAMPLES_PER_SLOT + 6 + 2); n1 != 0; n1--) {
            XBuf[0] >>= nd;
            XBuf[1] >>= nd;
            XBuf += (2 * 64);
        }
        XBuf -= (2 * 64 * (NUM_TIME_SLOTS * SAMPLES_PER_SLOT + 6 + 2));
    }

    /* calculate covariance elements */
    n1 = CalcCovariance1(XBuf, &p01re, &p01im, &p12re, &p12im, &p11re, &p22re);
    n2 = CalcCovariance2(XBuf, &p02re, &p02im);

    /* normalize everything to larger power of 2 scalefactor, call it n1 */
    if (n1 < n2) {
        nd = MIN(n2 - n1, (int32_t)31);
        p01re >>= nd;
        p01im >>= nd;
        p12re >>= nd;
        p12im >>= nd;
        p11re >>= nd;
        p22re >>= nd;
        n1 = n2;
    } else if (n1 > n2) {
        nd = MIN(n1 - n2, (int32_t)31);
        p02re >>= nd;
        p02im >>= nd;
    }

    /* calculate determinant of covariance matrix (at least 1 GB in pXX) */
    d = MULSHIFT32(p12re, p12re) + MULSHIFT32(p12im, p12im);
    d = MULSHIFT32(d, RELAX_COEF) << 1;
    d = MULSHIFT32(p11re, p22re) - d;
    ASSERT(d >= 0); /* should never be < 0 */

    zFlag = 0;
    *a0re = *a0im = 0;
    *a1re = *a1im = 0;
    if (d > 0) {
        /* input =   Q31  d    = Q(-2*n1 - 32 + nd) = Q31 * 2^(31 + 2*n1 + 32 - nd)
         * inverse = Q29  dInv = Q29 * 2^(-31 - 2*n1 - 32 + nd) = Q(29 + 31 + 2*n1 + 32 - nd)
         *
         * numerator has same Q format as d, since it's sum of normalized squares
         * so num * inverse = Q(-2*n1 - 32) * Q(29 + 31 + 2*n1 + 32 - nd)
         *                  = Q(29 + 31 - nd), drop low 32 in MULSHIFT32
         *                  = Q(29 + 31 - 32 - nd) = Q(28 - nd)
         */
        nd = CLZ(d) - 1;
        d <<= nd;
        dInv = InvRNormalized(d);

        /* 1 GB in pXX */
        tre = MULSHIFT32(p01re, p12re) - MULSHIFT32(p01im, p12im) - MULSHIFT32(p02re, p11re);
        tre = MULSHIFT32(tre, dInv);
        tim = MULSHIFT32(p01re, p12im) + MULSHIFT32(p01im, p12re) - MULSHIFT32(p02im, p11re);
        tim = MULSHIFT32(tim, dInv);

        /* if d is extremely small, just set coefs to 0 (would have poor precision anyway) */
        if (nd > 28 || (FASTABS(tre) >> (28 - nd)) >= 4 || (FASTABS(tim) >> (28 - nd)) >= 4) {
            zFlag = 1;
        } else {
            *a1re = tre << (FBITS_LPCOEFS - 28 + nd); /* i.e. convert Q(28 - nd) to Q(29) */
            *a1im = tim << (FBITS_LPCOEFS - 28 + nd);
        }
    }

    if (p11re) {
        /* input =   Q31  p11re = Q(-n1 + nd) = Q31 * 2^(31 + n1 - nd)
         * inverse = Q29  dInv  = Q29 * 2^(-31 - n1 + nd) = Q(29 + 31 + n1 - nd)
         *
         * numerator is Q(-n1 - 3)
         * so num * inverse = Q(-n1 - 3) * Q(29 + 31 + n1 - nd)
         *                  = Q(29 + 31 - 3 - nd), drop low 32 in MULSHIFT32
         *                  = Q(29 + 31 - 3 - 32 - nd) = Q(25 - nd)
         */
        nd = CLZ(p11re) - 1; /* assume positive */
        p11re <<= nd;
        dInv = InvRNormalized(p11re);

        /* a1re, a1im = Q29, so scaled by (n1 + 3) */
        tre = (p01re >> 3) + MULSHIFT32(p12re, *a1re) + MULSHIFT32(p12im, *a1im);
        tre = -MULSHIFT32(tre, dInv);
        tim = (p01im >> 3) - MULSHIFT32(p12im, *a1re) + MULSHIFT32(p12re, *a1im);
        tim = -MULSHIFT32(tim, dInv);

        if (nd > 25 || (FASTABS(tre) >> (25 - nd)) >= 4 || (FASTABS(tim) >> (25 - nd)) >= 4) {
            zFlag = 1;
        } else {
            *a0re = tre << (FBITS_LPCOEFS - 25 + nd); /* i.e. convert Q(25 - nd) to Q(29) */
            *a0im = tim << (FBITS_LPCOEFS - 25 + nd);
        }
    }

    /* see 4.6.18.6.2 - if magnitude of a0 or a1 >= 4 then a0 = a1 = 0
     * i.e. a0re < 4, a0im < 4, a1re < 4, a1im < 4
     * Q29*Q29 = Q26
     */
    if (zFlag || MULSHIFT32(*a0re, *a0re) + MULSHIFT32(*a0im, *a0im) >= MAG_16 ||
        MULSHIFT32(*a1re, *a1re) + MULSHIFT32(*a1im, *a1im) >= MAG_16) {
        *a0re = *a0im = 0;
        *a1re = *a1im = 0;
    }

    /* no need to clip - we never changed the XBuf data, just used it to calculate a0 and a1 */
    if (gb < 3) {
        nd = 3 - gb;
        for (n1 = (NUM_TIME_SLOTS * SAMPLES_PER_SLOT + 6 + 2); n1 != 0; n1--) {
            XBuf[0] <<= nd;
            XBuf[1] <<= nd;
            XBuf += (2 * 64);
        }
    }
}
/***********************************************************************************************************************
 * Function:    GenerateHighFreq
 *
 * Description: generate high frequencies with SBR (4.6.18.6)
 *
 * Inputs:      initialized SBRGrid struct for this channel
 *              initialized SBRFreq struct for this SCE/CPE block
 *              initialized SBRChan struct for this channel
 *              index of current channel (0 for SCE, 0 or 1 for CPE)
 *
 * Outputs:     new high frequency samples starting at frequency kStart
 *
 * Return:      none
 **********************************************************************************************************************/
void AACDecoder::GenerateHighFreq(SBRGrid *sbrGrid, SBRFreq *sbrFreq, SBRChan *sbrChan, int32_t ch) {
    int32_t band, newBW, c, t, gb, gbMask, gbIdx;
    int32_t currPatch, p, x, k, g, i, iStart, iEnd, bw, bwsq;
    int32_t a0re, a0im, a1re, a1im;
    int32_t x1re, x1im, x2re, x2im;
    int32_t ACCre, ACCim;
    int32_t *XBufLo, *XBufHi;
    (void)ch;

    /* calculate array of chirp factors */
    for (band = 0; band < sbrFreq->numNoiseFloorBands; band++) {
        c = sbrChan->chirpFact[band]; /* previous (bwArray') */
        newBW = newBWTab[sbrChan->invfMode[0][band]][sbrChan->invfMode[1][band]];

        /* weighted average of new and old (can't overflow - total gain = 1.0) */
        if (newBW < c)
            t = MULSHIFT32(newBW, 0x60000000) + MULSHIFT32(0x20000000, c); /* new is smaller: 0.75*new + 0.25*old */
        else
            t = MULSHIFT32(newBW, 0x74000000) +
                MULSHIFT32(0x0c000000, c); /* new is larger: 0.90625*new + 0.09375*old */
        t <<= 1;

        if (t < 0x02000000) /* below 0.015625, clip to 0 */
            t = 0;
        if (t > 0x7f800000) /* clip to 0.99609375 */
            t = 0x7f800000;

        /* save curr as prev for next time */
        sbrChan->chirpFact[band] = t;
        sbrChan->invfMode[0][band] = sbrChan->invfMode[1][band];
    }

    iStart = sbrGrid->envTimeBorder[0] + HF_ADJ;
    iEnd = sbrGrid->envTimeBorder[sbrGrid->numEnv] + HF_ADJ;

    /* generate new high freqs from low freqs, patches, and chirp factors */
    k = sbrFreq->kStart;
    g = 0;
    bw = sbrChan->chirpFact[g];
    bwsq = MULSHIFT32(bw, bw) << 1;

    gbMask = (sbrChan->gbMask[0] | sbrChan->gbMask[1]); /* older 32 | newer 8 */
    gb = CLZ(gbMask) - 1;

    for (currPatch = 0; currPatch < sbrFreq->numPatches; currPatch++) {
        for (x = 0; x < sbrFreq->patchNumSubbands[currPatch]; x++) {
            /* map k to corresponding noise floor band */
            if (k >= sbrFreq->freqNoise[g + 1]) {
                g++;
                bw = sbrChan->chirpFact[g];     /* Q31 */
                bwsq = MULSHIFT32(bw, bw) << 1; /* Q31 */
            }

            p = sbrFreq->patchStartSubband[currPatch] + x; /* low QMF band */
            XBufHi = m_PSInfoSBR->XBuf[iStart][k];
            if (bw) {
                CalcLPCoefs(m_PSInfoSBR->XBuf[0][p], &a0re, &a0im, &a1re, &a1im, gb);

                a0re = MULSHIFT32(bw, a0re); /* Q31 * Q29 = Q28 */
                a0im = MULSHIFT32(bw, a0im);
                a1re = MULSHIFT32(bwsq, a1re);
                a1im = MULSHIFT32(bwsq, a1im);

                XBufLo = m_PSInfoSBR->XBuf[iStart - 2][p];

                x2re = XBufLo[0]; /* RE{XBuf[n-2]} */
                x2im = XBufLo[1]; /* IM{XBuf[n-2]} */
                XBufLo += (64 * 2);

                x1re = XBufLo[0]; /* RE{XBuf[n-1]} */
                x1im = XBufLo[1]; /* IM{XBuf[n-1]} */
                XBufLo += (64 * 2);

                for (i = iStart; i < iEnd; i++) {
                    /* a0re/im, a1re/im are Q28 with at least 1 GB,
                     *   so the summing for AACre/im is fine (1 GB in, plus 1 from MULSHIFT32)
                     */
                    ACCre = MULSHIFT32(x2re, a1re) - MULSHIFT32(x2im, a1im);
                    ACCim = MULSHIFT32(x2re, a1im) + MULSHIFT32(x2im, a1re);
                    x2re = x1re;
                    x2im = x1im;

                    ACCre += MULSHIFT32(x1re, a0re) - MULSHIFT32(x1im, a0im);
                    ACCim += MULSHIFT32(x1re, a0im) + MULSHIFT32(x1im, a0re);
                    x1re = XBufLo[0]; /* RE{XBuf[n]} */
                    x1im = XBufLo[1]; /* IM{XBuf[n]} */
                    XBufLo += (64 * 2);

                    /* lost 4 fbits when scaling by a0re/im, a1re/im (Q28) */
                    ACCre = CLIP_2N_SHIFT30_4(ACCre);
                    ACCre += x1re;
                    ACCim = CLIP_2N_SHIFT30_4(ACCim);
                    ACCim += x1im;

                    XBufHi[0] = ACCre;
                    XBufHi[1] = ACCim;
                    XBufHi += (64 * 2);

                    /* update guard bit masks */
                    gbMask = FASTABS(ACCre);
                    gbMask |= FASTABS(ACCim);
                    gbIdx = (i >> 5) & 0x01; /* 0 if i < 32, 1 if i >= 32 */
                    sbrChan->gbMask[gbIdx] |= gbMask;
                }
            } else {
                XBufLo = (int32_t *)m_PSInfoSBR->XBuf[iStart][p];
                for (i = iStart; i < iEnd; i++) {
                    XBufHi[0] = XBufLo[0];
                    XBufHi[1] = XBufLo[1];
                    XBufLo += (64 * 2);
                    XBufHi += (64 * 2);
                }
            }
            k++; /* high QMF band */
        }
    }
}
/***********************************************************************************************************************
 * Function:    DecodeHuffmanScalar
 *
 * Description: decode one Huffman symbol from bitstream
 *
 * Inputs:      pointers to Huffman table and info struct
 *              left-aligned bit buffer with >= huffTabInfo->maxBits bits
 *
 * Outputs:     decoded symbol in *val
 *
 * Return:      number of bits in symbol
 *
 * Notes:       assumes canonical Huffman codes:
 *                first CW always 0, we have "count" CW's of length "nBits" bits
 *                starting CW for codes of length nBits+1 =
 *                  (startCW[nBits] + count[nBits]) << 1
 *                if there are no codes at nBits, then we just keep << 1 each time
 *                  (since count[nBits] = 0)
 **********************************************************************************************************************/
int32_t AACDecoder::DecodeHuffmanScalar(const int16_t *huffTab, const HuffInfo_t *huffTabInfo, uint32_t bitBuf,
                                        int32_t *val) {
    uint32_t count, start, shift, t;
    const uint8_t *countPtr;
    const int16_t *map;

    map = huffTab + huffTabInfo->offset;
    countPtr = huffTabInfo->count;

    start = 0;
    count = 0;
    shift = 32;
    do {
        start += count;
        start <<= 1;
        map += count;
        count = *countPtr++;
        shift--;
        t = (bitBuf >> shift) - start;
    } while (t >= count);

    *val = (int32_t)map[t];
    return (countPtr - huffTabInfo->count);
}
/***********************************************************************************************************************
 * Function:    DecodeOneSymbol
 *
 * Description: dequantize one Huffman symbol from bitstream,
 *                using table huffTabSBR[huffTabIndex]
 *
 * Inputs:      index of Huffman table
 *
 * Outputs:     bitstream advanced by number of bits in codeword
 *
 * Return:      one decoded symbol
 **********************************************************************************************************************/
int32_t AACDecoder::DecodeOneSymbol(int32_t huffTabIndex) {
    int32_t nBits, val;
    uint32_t bitBuf;
    const HuffInfo_t *hi;

    hi = &(huffTabSBRInfo[huffTabIndex]);

    bitBuf = GetBitsNoAdvance(hi->maxBits) << (32 - hi->maxBits);
    nBits = DecodeHuffmanScalar(huffTabSBR, hi, bitBuf, &val);
    AdvanceBitstream(nBits);

    return val;
}

/* [1.0, sqrt(2)], format = Q29 (one guard bit for decoupling) */
static const int32_t envDQTab[2] PROGMEM = {0x20000000, 0x2d413ccc};

/***********************************************************************************************************************
 * Function:    DequantizeEnvelope
 *
 * Description: dequantize envelope scalefactors
 *
 * Inputs:      number of scalefactors to process
 *              amplitude resolution flag for this frame (0 or 1)
 *              quantized envelope scalefactors
 *
 * Outputs:     dequantized envelope scalefactors
 *
 * Return:      extra int32_t bits in output (6 + expMax)
 *              in other words, output format = Q(FBITS_OUT_DQ_ENV - (6 + expMax))
 *
 * Notes:       dequantized scalefactors have at least 2 GB
 **********************************************************************************************************************/
int32_t AACDecoder::DequantizeEnvelope(int32_t nBands, int32_t ampRes, int8_t *envQuant, int32_t *envDequant) {
    int32_t exp, expMax, i, scalei;

    if (nBands <= 0)
        return 0;

    /* scan for largest dequant value (do separately from envelope decoding to keep code cleaner) */
    expMax = 0;
    for (i = 0; i < nBands; i++) {
        if (envQuant[i] > expMax)
            expMax = envQuant[i];
    }

    /* dequantized envelope gains
     *   envDequant = 64*2^(envQuant / alpha) = 2^(6 + envQuant / alpha)
     *     if ampRes == 0, alpha = 2 and range of envQuant = [0, 127]
     *     if ampRes == 1, alpha = 1 and range of envQuant = [0, 63]
     * also if coupling is on, envDequant is scaled by something in range [0, 2]
     * so range of envDequant = [2^6, 2^69] (no coupling), [2^6, 2^70] (with coupling)
     *
     * typical range (from observation) of envQuant/alpha = [0, 27] --> largest envQuant ~= 2^33
     * output: Q(29 - (6 + expMax))
     *
     * reference: 14496-3:2001(E)/4.6.18.3.5 and 14496-4:200X/FPDAM8/5.6.5.1.2.1.5
     */
    if (ampRes) {
        do {
            exp = *envQuant++;
            scalei = MIN(expMax - exp, (int32_t)31);
            *envDequant++ = envDQTab[0] >> scalei;
        } while (--nBands);

        return (6 + expMax);
    } else {
        expMax >>= 1;
        do {
            exp = *envQuant++;
            scalei = MIN(expMax - (exp >> 1), (int32_t)31);
            *envDequant++ = envDQTab[exp & 0x01] >> scalei;
        } while (--nBands);

        return (6 + expMax);
    }
}
/***********************************************************************************************************************
 * Function:    DequantizeNoise
 *
 * Description: dequantize noise scalefactors
 *
 * Inputs:      number of scalefactors to process
 *              quantized noise scalefactors
 *
 * Outputs:     dequantized noise scalefactors, format = Q(FBITS_OUT_DQ_NOISE)
 *
 * Return:      none
 *
 * Notes:       dequantized scalefactors have at least 2 GB
 **********************************************************************************************************************/
void AACDecoder::DequantizeNoise(int32_t nBands, int8_t *noiseQuant, int32_t *noiseDequant) {
    int32_t exp, scalei;

    if (nBands <= 0)
        return;

    /* dequantize noise floor gains (4.6.18.3.5):
     *   noiseDequant = 2^(NOISE_FLOOR_OFFSET - noiseQuant)
     *
     * range of noiseQuant = [0, 30] (see 4.6.18.3.6), NOISE_FLOOR_OFFSET = 6
     *   so range of noiseDequant = [2^-24, 2^6]
     */
    do {
        exp = *noiseQuant++;
        scalei = NOISE_FLOOR_OFFSET - exp + FBITS_OUT_DQ_NOISE; /* 6 + 24 - exp, exp = [0,30] */

        if (scalei < 0)
            *noiseDequant++ = 0;
        else if (scalei < 30)
            *noiseDequant++ = 1 << scalei;
        else
            *noiseDequant++ = 0x3fffffff; /* leave 2 GB */

    } while (--nBands);
}
/***********************************************************************************************************************
 * Function:    DecodeSBREnvelope
 *
 * Description: decode delta Huffman coded envelope scalefactors from bitstream
 *
 * Inputs:      initialized SBRGrid struct for this channel
 *              initialized SBRFreq struct for this SCE/CPE block
 *              initialized SBRChan struct for this channel
 *              index of current channel (0 for SCE, 0 or 1 for CPE)
 *
 * Outputs:     dequantized env scalefactors for left channel (before decoupling)
 *              dequantized env scalefactors for right channel (if coupling off)
 *                or raw decoded env scalefactors for right channel (if coupling on)
 *
 * Return:      none
 **********************************************************************************************************************/
void AACDecoder::DecodeSBREnvelope(SBRGrid *sbrGrid, SBRFreq *sbrFreq, SBRChan *sbrChan, int32_t ch) {
    int32_t huffIndexTime, huffIndexFreq, env, envStartBits, band, nBands, sf, lastEnv;
    int32_t freqRes, freqResPrev, dShift, i;

    if (m_PSInfoSBR->couplingFlag && ch) {
        dShift = 1;
        if (sbrGrid->ampResFrame) {
            huffIndexTime = HuffTabSBR_tEnv30b;
            huffIndexFreq = HuffTabSBR_fEnv30b;
            envStartBits = 5;
        } else {
            huffIndexTime = HuffTabSBR_tEnv15b;
            huffIndexFreq = HuffTabSBR_fEnv15b;
            envStartBits = 6;
        }
    } else {
        dShift = 0;
        if (sbrGrid->ampResFrame) {
            huffIndexTime = HuffTabSBR_tEnv30;
            huffIndexFreq = HuffTabSBR_fEnv30;
            envStartBits = 6;
        } else {
            huffIndexTime = HuffTabSBR_tEnv15;
            huffIndexFreq = HuffTabSBR_fEnv15;
            envStartBits = 7;
        }
    }

    /* range of envDataQuant[] = [0, 127] (see comments in DequantizeEnvelope() for reference) */
    for (env = 0; env < sbrGrid->numEnv; env++) {
        nBands = (sbrGrid->freqRes[env] ? sbrFreq->nHigh : sbrFreq->nLow);
        freqRes = (sbrGrid->freqRes[env]);
        freqResPrev = (env == 0 ? sbrGrid->freqResPrev : sbrGrid->freqRes[env - 1]);
        lastEnv = (env == 0 ? sbrGrid->numEnvPrev - 1 : env - 1);
        if (lastEnv < 0)
            lastEnv = 0; /* first frame */

        ASSERT(nBands <= MAX_QMF_BANDS);

        if (sbrChan->deltaFlagEnv[env] == 0) {
            /* delta coding in freq */
            sf = GetBits(envStartBits) << dShift;
            sbrChan->envDataQuant[env][0] = sf;
            for (band = 1; band < nBands; band++) {
                sf = DecodeOneSymbol(huffIndexFreq) << dShift;
                sbrChan->envDataQuant[env][band] = sf + sbrChan->envDataQuant[env][band - 1];
            }
        } else if (freqRes == freqResPrev) {
            /* delta coding in time - same freq resolution for both frames */
            for (band = 0; band < nBands; band++) {
                sf = DecodeOneSymbol(huffIndexTime) << dShift;
                sbrChan->envDataQuant[env][band] = sf + sbrChan->envDataQuant[lastEnv][band];
            }
        } else if (freqRes == 0 && freqResPrev == 1) {
            /* delta coding in time - low freq resolution for new frame, high freq resolution for old frame */
            for (band = 0; band < nBands; band++) {
                sf = DecodeOneSymbol(huffIndexTime) << dShift;
                sbrChan->envDataQuant[env][band] = sf;
                for (i = 0; i < sbrFreq->nHigh; i++) {
                    if (sbrFreq->freqHigh[i] == sbrFreq->freqLow[band]) {
                        sbrChan->envDataQuant[env][band] += sbrChan->envDataQuant[lastEnv][i];
                        break;
                    }
                }
            }
        } else if (freqRes == 1 && freqResPrev == 0) {
            /* delta coding in time - high freq resolution for new frame, low freq resolution for old frame */
            for (band = 0; band < nBands; band++) {
                sf = DecodeOneSymbol(huffIndexTime) << dShift;
                sbrChan->envDataQuant[env][band] = sf;
                for (i = 0; i < sbrFreq->nLow; i++) {
                    if (sbrFreq->freqLow[i] <= sbrFreq->freqHigh[band] &&
                        sbrFreq->freqHigh[band] < sbrFreq->freqLow[i + 1]) {
                        sbrChan->envDataQuant[env][band] += sbrChan->envDataQuant[lastEnv][i];
                        break;
                    }
                }
            }
        }

        /* skip coupling channel */
        if (ch != 1 || m_PSInfoSBR->couplingFlag != 1)
            m_PSInfoSBR->envDataDequantScale[ch][env] = DequantizeEnvelope(
                nBands, sbrGrid->ampResFrame, sbrChan->envDataQuant[env], m_PSInfoSBR->envDataDequant[ch][env]);
    }
    sbrGrid->numEnvPrev = sbrGrid->numEnv;
    sbrGrid->freqResPrev = sbrGrid->freqRes[sbrGrid->numEnv - 1];
}
/***********************************************************************************************************************
 * Function:    DecodeSBRNoise
 *
 * Description: decode delta Huffman coded noise scalefactors from bitstream
 *
 * Inputs:      initialized SBRGrid struct for this channel
 *              initialized SBRFreq struct for this SCE/CPE block
 *              initialized SBRChan struct for this channel
 *              index of current channel (0 for SCE, 0 or 1 for CPE)
 *
 * Outputs:     dequantized noise scalefactors for left channel (before decoupling)
 *              dequantized noise scalefactors for right channel (if coupling off)
 *                or raw decoded noise scalefactors for right channel (if coupling on)
 *
 * Return:      none
 **********************************************************************************************************************/
void AACDecoder::DecodeSBRNoise(SBRGrid *sbrGrid, SBRFreq *sbrFreq, SBRChan *sbrChan, int32_t ch) {
    int32_t huffIndexTime, huffIndexFreq, noiseFloor, band, dShift, sf, lastNoiseFloor;

    if (m_PSInfoSBR->couplingFlag && ch) {
        dShift = 1;
        huffIndexTime = HuffTabSBR_tNoise30b;
        huffIndexFreq = HuffTabSBR_fNoise30b;
    } else {
        dShift = 0;
        huffIndexTime = HuffTabSBR_tNoise30;
        huffIndexFreq = HuffTabSBR_fNoise30;
    }

    for (noiseFloor = 0; noiseFloor < sbrGrid->numNoiseFloors; noiseFloor++) {
        lastNoiseFloor = (noiseFloor == 0 ? sbrGrid->numNoiseFloorsPrev - 1 : noiseFloor - 1);
        if (lastNoiseFloor < 0)
            lastNoiseFloor = 0; /* first frame */

        ASSERT(sbrFreq->numNoiseFloorBands <= MAX_QMF_BANDS);

        if (sbrChan->deltaFlagNoise[noiseFloor] == 0) {
            /* delta coding in freq */
            sbrChan->noiseDataQuant[noiseFloor][0] = GetBits(5) << dShift;
            for (band = 1; band < sbrFreq->numNoiseFloorBands; band++) {
                sf = DecodeOneSymbol(huffIndexFreq) << dShift;
                sbrChan->noiseDataQuant[noiseFloor][band] = sf + sbrChan->noiseDataQuant[noiseFloor][band - 1];
            }
        } else {
            /* delta coding in time */
            for (band = 0; band < sbrFreq->numNoiseFloorBands; band++) {
                sf = DecodeOneSymbol(huffIndexTime) << dShift;
                sbrChan->noiseDataQuant[noiseFloor][band] = sf + sbrChan->noiseDataQuant[lastNoiseFloor][band];
            }
        }

        /* skip coupling channel */
        if (ch != 1 || m_PSInfoSBR->couplingFlag != 1)
            DequantizeNoise(sbrFreq->numNoiseFloorBands, sbrChan->noiseDataQuant[noiseFloor],
                            m_PSInfoSBR->noiseDataDequant[ch][noiseFloor]);
    }
    sbrGrid->numNoiseFloorsPrev = sbrGrid->numNoiseFloors;
}

/* dqTabCouple[i] = 2 / (1 + 2^(12 - i)), format = Q30 */
static const int32_t dqTabCouple[25] PROGMEM = {
    0x0007ff80, 0x000ffe00, 0x001ff802, 0x003fe010, 0x007f8080, 0x00fe03f8, 0x01f81f82, 0x03e0f83e, 0x07878788,
    0x0e38e38e, 0x1999999a, 0x2aaaaaab, 0x40000000, 0x55555555, 0x66666666, 0x71c71c72, 0x78787878, 0x7c1f07c2,
    0x7e07e07e, 0x7f01fc08, 0x7f807f80, 0x7fc01ff0, 0x7fe007fe, 0x7ff00200, 0x7ff80080,
};

/***********************************************************************************************************************
 * Function:    UncoupleSBREnvelope
 *
 * Description: scale dequantized envelope scalefactors according to channel
 *                coupling rules
 *
 * Inputs:      initialized SBRGrid struct for this channel
 *              initialized SBRFreq struct for this SCE/CPE block
 *              initialized SBRChan struct for right channel including
 *                quantized envelope scalefactors
 *
 * Outputs:     dequantized envelope data for left channel (after decoupling)
 *              dequantized envelope data for right channel (after decoupling)
 *
 * Return:      none
 **********************************************************************************************************************/
void AACDecoder::UncoupleSBREnvelope(SBRGrid *sbrGrid, SBRFreq *sbrFreq, SBRChan *sbrChanR) {
    int32_t env, band, nBands, scalei, E_1;

    scalei = (sbrGrid->ampResFrame ? 0 : 1);
    for (env = 0; env < sbrGrid->numEnv; env++) {
        nBands = (sbrGrid->freqRes[env] ? sbrFreq->nHigh : sbrFreq->nLow);
        m_PSInfoSBR->envDataDequantScale[1][env] = m_PSInfoSBR->envDataDequantScale[0][env];
        for (band = 0; band < nBands; band++) {
            /* clip E_1 to [0, 24] (scalefactors approach 0 or 2) */
            E_1 = sbrChanR->envDataQuant[env][band] >> scalei;
            if (E_1 < 0)
                E_1 = 0;
            if (E_1 > 24)
                E_1 = 24;

            /* envDataDequant[0] has 1 GB, so << by 2 is okay */
            m_PSInfoSBR->envDataDequant[1][env][band] =
                MULSHIFT32(m_PSInfoSBR->envDataDequant[0][env][band], dqTabCouple[24 - E_1]) << 2;
            m_PSInfoSBR->envDataDequant[0][env][band] =
                MULSHIFT32(m_PSInfoSBR->envDataDequant[0][env][band], dqTabCouple[E_1]) << 2;
        }
    }
}
/***********************************************************************************************************************
 * Function:    UncoupleSBRNoise
 *
 * Description: scale dequantized noise floor scalefactors according to channel
 *                coupling rules
 *
 * Inputs:      initialized SBRGrid struct for this channel
 *              initialized SBRFreq struct for this SCE/CPE block
 *              initialized SBRChan struct for this channel including
 *                quantized noise scalefactors
 *
 * Outputs:     dequantized noise data for left channel (after decoupling)
 *              dequantized noise data for right channel (after decoupling)
 *
 * Return:      none
 **********************************************************************************************************************/
void AACDecoder::UncoupleSBRNoise(SBRGrid *sbrGrid, SBRFreq *sbrFreq, SBRChan *sbrChanR) {
    int32_t noiseFloor, band, Q_1;

    for (noiseFloor = 0; noiseFloor < sbrGrid->numNoiseFloors; noiseFloor++) {
        for (band = 0; band < sbrFreq->numNoiseFloorBands; band++) {
            /* Q_1 should be in range [0, 24] according to 4.6.18.3.6, but check to make sure */
            Q_1 = sbrChanR->noiseDataQuant[noiseFloor][band];
            if (Q_1 < 0)
                Q_1 = 0;
            if (Q_1 > 24)
                Q_1 = 24;

            /* noiseDataDequant[0] has 1 GB, so << by 2 is okay */
            m_PSInfoSBR->noiseDataDequant[1][noiseFloor][band] =
                MULSHIFT32(m_PSInfoSBR->noiseDataDequant[0][noiseFloor][band], dqTabCouple[24 - Q_1]) << 2;
            m_PSInfoSBR->noiseDataDequant[0][noiseFloor][band] =
                MULSHIFT32(m_PSInfoSBR->noiseDataDequant[0][noiseFloor][band], dqTabCouple[Q_1]) << 2;
        }
    }
}
/***********************************************************************************************************************
 * Function:    DecWindowOverlapNoClip
 *
 * Description: apply synthesis window, do overlap-add without clipping,
 *                for winSequence LONG-LONG
 *
 * Inputs:      input buffer (output of type-IV DCT)
 *              overlap buffer (saved from last time)
 *              window type (sin or KBD) for input buffer
 *              window type (sin or KBD) for overlap buffer
 *
 * Outputs:     one channel, one frame of 32-bit PCM, non-interleaved
 *
 * Return:      none
 *
 * Notes:       use this function when the decoded PCM is going to the SBR decoder
 **********************************************************************************************************************/
void AACDecoder::DecWindowOverlapNoClip(int32_t *buf0, int32_t *over0, int32_t *out0, int32_t winTypeCurr,
                                        int32_t winTypePrev) {
    int32_t in, w0, w1, f0, f1;
    int32_t *buf1, *over1, *out1;
    const int32_t *wndPrev, *wndCurr;

    buf0 += (1024 >> 1);
    buf1 = buf0 - 1;
    out1 = out0 + 1024 - 1;
    over1 = over0 + 1024 - 1;

    wndPrev = (winTypePrev == 1 ? kbdWindow + kbdWindowOffset[1] : sinWindow + sinWindowOffset[1]);
    if (winTypeCurr == winTypePrev) {
        /* cut window loads in half since current and overlap sections use same symmetric window */
        do {
            w0 = *wndPrev++;
            w1 = *wndPrev++;
            in = *buf0++;

            f0 = MULSHIFT32(w0, in);
            f1 = MULSHIFT32(w1, in);

            in = *over0;
            *out0++ = in - f0;

            in = *over1;
            *out1-- = in + f1;

            in = *buf1--;
            *over1-- = MULSHIFT32(w0, in);
            *over0++ = MULSHIFT32(w1, in);
        } while (over0 < over1);
    } else {
        /* different windows for current and overlap parts - should still fit in registers on ARM w/o stack spill */
        wndCurr = (winTypeCurr == 1 ? kbdWindow + kbdWindowOffset[1] : sinWindow + sinWindowOffset[1]);
        do {
            w0 = *wndPrev++;
            w1 = *wndPrev++;
            in = *buf0++;

            f0 = MULSHIFT32(w0, in);
            f1 = MULSHIFT32(w1, in);

            in = *over0;
            *out0++ = in - f0;

            in = *over1;
            *out1-- = in + f1;

            w0 = *wndCurr++;
            w1 = *wndCurr++;
            in = *buf1--;

            *over1-- = MULSHIFT32(w0, in);
            *over0++ = MULSHIFT32(w1, in);
        } while (over0 < over1);
    }
}
/***********************************************************************************************************************
 * Function:    DecWindowOverlapLongStart
 *
 * Description: apply synthesis window, do overlap-add, without clipping
 *                for winSequence LONG-START
 *
 * Inputs:      input buffer (output of type-IV DCT)
 *              overlap buffer (saved from last time)
 *              window type (sin or KBD) for input buffer
 *              window type (sin or KBD) for overlap buffer
 *
 * Outputs:     one channel, one frame of 32-bit PCM, non-interleaved
 *
 * Return:      none
 *
 * Notes:       use this function when the decoded PCM is going to the SBR decoder
 **********************************************************************************************************************/
void AACDecoder::DecWindowOverlapLongStartNoClip(int32_t *buf0, int32_t *over0, int32_t *out0, int32_t winTypeCurr,
                                                 int32_t winTypePrev) {
    int32_t i, in, w0, w1, f0, f1;
    int32_t *buf1, *over1, *out1;
    const int32_t *wndPrev, *wndCurr;

    buf0 += (1024 >> 1);
    buf1 = buf0 - 1;
    out1 = out0 + 1024 - 1;
    over1 = over0 + 1024 - 1;

    wndPrev = (winTypePrev == 1 ? kbdWindow + kbdWindowOffset[1] : sinWindow + sinWindowOffset[1]);
    i = 448; /* 2 outputs, 2 overlaps per loop */
    do {
        w0 = *wndPrev++;
        w1 = *wndPrev++;
        in = *buf0++;

        f0 = MULSHIFT32(w0, in);
        f1 = MULSHIFT32(w1, in);

        in = *over0;
        *out0++ = in - f0;

        in = *over1;
        *out1-- = in + f1;

        in = *buf1--;

        *over1-- = 0;       /* Wn = 0 for n = (2047, 2046, ... 1600) */
        *over0++ = in >> 1; /* Wn = 1 for n = (1024, 1025, ... 1471) */
    } while (--i);

    wndCurr = (winTypeCurr == 1 ? kbdWindow + kbdWindowOffset[0] : sinWindow + sinWindowOffset[0]);

    /* do 64 more loops - 2 outputs, 2 overlaps per loop */
    do {
        w0 = *wndPrev++;
        w1 = *wndPrev++;
        in = *buf0++;

        f0 = MULSHIFT32(w0, in);
        f1 = MULSHIFT32(w1, in);

        in = *over0;
        *out0++ = in - f0;

        in = *over1;
        *out1-- = in + f1;

        w0 = *wndCurr++; /* W[0], W[1], ... --> W[255], W[254], ... */
        w1 = *wndCurr++; /* W[127], W[126], ... --> W[128], W[129], ... */
        in = *buf1--;

        *over1-- = MULSHIFT32(w0, in); /* Wn = short window for n = (1599, 1598, ... , 1536) */
        *over0++ = MULSHIFT32(w1, in); /* Wn = short window for n = (1472, 1473, ... , 1535) */
    } while (over0 < over1);
}
/***********************************************************************************************************************
 * Function:    DecWindowOverlapLongStop
 *
 * Description: apply synthesis window, do overlap-add, without clipping
 *                for winSequence LONG-STOP
 *
 * Inputs:      input buffer (output of type-IV DCT)
 *              overlap buffer (saved from last time)
 *              window type (sin or KBD) for input buffer
 *              window type (sin or KBD) for overlap buffer
 *
 * Outputs:     one channel, one frame of 32-bit PCM, non-interleaved
 *
 * Return:      none
 *
 * Notes:       use this function when the decoded PCM is going to the SBR decoder
 **********************************************************************************************************************/
void AACDecoder::DecWindowOverlapLongStopNoClip(int32_t *buf0, int32_t *over0, int32_t *out0, int32_t winTypeCurr,
                                                int32_t winTypePrev) {
    int32_t i, in, w0, w1, f0, f1;
    int32_t *buf1, *over1, *out1;
    const int32_t *wndPrev, *wndCurr;

    buf0 += (1024 >> 1);
    buf1 = buf0 - 1;
    out1 = out0 + 1024 - 1;
    over1 = over0 + 1024 - 1;

    wndPrev = (winTypePrev == 1 ? kbdWindow + kbdWindowOffset[0] : sinWindow + sinWindowOffset[0]);
    wndCurr = (winTypeCurr == 1 ? kbdWindow + kbdWindowOffset[1] : sinWindow + sinWindowOffset[1]);

    i = 448; /* 2 outputs, 2 overlaps per loop */
    do {
        /* Wn = 0 for n = (0, 1, ... 447) */
        /* Wn = 1 for n = (576, 577, ... 1023) */
        in = *buf0++;
        f1 = in >> 1; /* scale since skipping multiply by Q31 */

        in = *over0;
        *out0++ = in;

        in = *over1;
        *out1-- = in + f1;

        w0 = *wndCurr++;
        w1 = *wndCurr++;
        in = *buf1--;

        *over1-- = MULSHIFT32(w0, in);
        *over0++ = MULSHIFT32(w1, in);
    } while (--i);

    /* do 64 more loops - 2 outputs, 2 overlaps per loop */
    do {
        w0 = *wndPrev++; /* W[0], W[1], ...W[63] */
        w1 = *wndPrev++; /* W[127], W[126], ... W[64] */
        in = *buf0++;

        f0 = MULSHIFT32(w0, in);
        f1 = MULSHIFT32(w1, in);

        in = *over0;
        *out0++ = in - f0;

        in = *over1;
        *out1-- = in + f1;

        w0 = *wndCurr++;
        w1 = *wndCurr++;
        in = *buf1--;

        *over1-- = MULSHIFT32(w0, in);
        *over0++ = MULSHIFT32(w1, in);
    } while (over0 < over1);
}
/***********************************************************************************************************************
 * Function:    DecWindowOverlapShort
 *
 * Description: apply synthesis window, do overlap-add, without clipping
 *                for winSequence EIGHT-SHORT (does all 8 short blocks)
 *
 * Inputs:      input buffer (output of type-IV DCT)
 *              overlap buffer (saved from last time)
 *              window type (sin or KBD) for input buffer
 *              window type (sin or KBD) for overlap buffer
 *
 * Outputs:     one channel, one frame of 32-bit PCM, non-interleaved
 *
 * Return:      none
 *
 * Notes:       use this function when the decoded PCM is going to the SBR decoder
 **********************************************************************************************************************/
void AACDecoder::DecWindowOverlapShortNoClip(int32_t *buf0, int32_t *over0, int32_t *out0, int32_t winTypeCurr,
                                             int32_t winTypePrev) {
    int32_t i, in, w0, w1, f0, f1;
    int32_t *buf1, *over1, *out1;
    const int32_t *wndPrev, *wndCurr;

    wndPrev = (winTypePrev == 1 ? kbdWindow + kbdWindowOffset[0] : sinWindow + sinWindowOffset[0]);
    wndCurr = (winTypeCurr == 1 ? kbdWindow + kbdWindowOffset[0] : sinWindow + sinWindowOffset[0]);

    /* pcm[0-447] = 0 + overlap[0-447] */
    /*
    i = 448;
    do {
        f0 = *over0++;
        f1 = *over0++;
        *out0++ = f0;
        *out0++ = f1;
        i -= 2;
    } while (i);
    */
    { //fb
        memcpy(out0, over0, 448 * sizeof(int32_t));
        out0 += 448;
        over0 += 448;
    }

    /* pcm[448-575] = Wp[0-127] * block0[0-127] + overlap[448-575] */
    out1 = out0 + (128 - 1);
    over1 = over0 + 128 - 1;
    buf0 += 64;
    buf1 = buf0 - 1;
    do {
        w0 = *wndPrev++; /* W[0], W[1], ...W[63] */
        w1 = *wndPrev++; /* W[127], W[126], ... W[64] */
        in = *buf0++;

        f0 = MULSHIFT32(w0, in);
        f1 = MULSHIFT32(w1, in);

        in = *over0;
        *out0++ = in - f0;

        in = *over1;
        *out1-- = in + f1;

        w0 = *wndCurr++;
        w1 = *wndCurr++;
        in = *buf1--;

        /* save over0/over1 for next short block, in the slots just vacated */
        *over1-- = MULSHIFT32(w0, in);
        *over0++ = MULSHIFT32(w1, in);
    } while (over0 < over1);

    /* pcm[576-703] = Wc[128-255] * block0[128-255] + Wc[0-127] * block1[0-127] + overlap[576-703]
     * pcm[704-831] = Wc[128-255] * block1[128-255] + Wc[0-127] * block2[0-127] + overlap[704-831]
     * pcm[832-959] = Wc[128-255] * block2[128-255] + Wc[0-127] * block3[0-127] + overlap[832-959]
     */
    for (i = 0; i < 3; i++) {
        out0 += 64;
        out1 = out0 + 128 - 1;
        over0 += 64;
        over1 = over0 + 128 - 1;
        buf0 += 64;
        buf1 = buf0 - 1;
        wndCurr -= 128;

        do {
            w0 = *wndCurr++; /* W[0], W[1], ...W[63] */
            w1 = *wndCurr++; /* W[127], W[126], ... W[64] */
            in = *buf0++;

            f0 = MULSHIFT32(w0, in);
            f1 = MULSHIFT32(w1, in);

            in = *(over0 - 128); /* from last short block */
            in += *(over0 + 0);  /* from last full frame */
            *out0++ = in - f0;

            in = *(over1 - 128); /* from last short block */
            in += *(over1 + 0);  /* from last full frame */
            *out1-- = in + f1;

            /* save over0/over1 for next short block, in the slots just vacated */
            in = *buf1--;
            *over1-- = MULSHIFT32(w0, in);
            *over0++ = MULSHIFT32(w1, in);
        } while (over0 < over1);
    }

    /* pcm[960-1023] = Wc[128-191] * block3[128-191] + Wc[0-63]   * block4[0-63] + overlap[960-1023]
     * over[0-63]    = Wc[192-255] * block3[192-255] + Wc[64-127] * block4[64-127]
     */
    out0 += 64;
    over0 -= 832;            /* points at overlap[64] */
    over1 = over0 + 128 - 1; /* points at overlap[191] */
    buf0 += 64;
    buf1 = buf0 - 1;
    wndCurr -= 128;
    do {
        w0 = *wndCurr++; /* W[0], W[1], ...W[63] */
        w1 = *wndCurr++; /* W[127], W[126], ... W[64] */
        in = *buf0++;

        f0 = MULSHIFT32(w0, in);
        f1 = MULSHIFT32(w1, in);

        in = *(over0 + 768);  /* from last short block */
        in += *(over0 + 896); /* from last full frame */
        *out0++ = in - f0;

        in = *(over1 + 768); /* from last short block */
        *(over1 - 128) = in + f1;

        in = *buf1--;
        *over1-- = MULSHIFT32(w0, in); /* save in overlap[128-191] */
        *over0++ = MULSHIFT32(w1, in); /* save in overlap[64-127] */
    } while (over0 < over1);

    /* over0 now points at overlap[128] */

    /* over[64-191]   = Wc[128-255] * block4[128-255] + Wc[0-127] * block5[0-127]
     * over[192-319]  = Wc[128-255] * block5[128-255] + Wc[0-127] * block6[0-127]
     * over[320-447]  = Wc[128-255] * block6[128-255] + Wc[0-127] * block7[0-127]
     * over[448-576]  = Wc[128-255] * block7[128-255]
     */
    for (i = 0; i < 3; i++) {
        over0 += 64;
        over1 = over0 + 128 - 1;
        buf0 += 64;
        buf1 = buf0 - 1;
        wndCurr -= 128;
        do {
            w0 = *wndCurr++; /* W[0], W[1], ...W[63] */
            w1 = *wndCurr++; /* W[127], W[126], ... W[64] */
            in = *buf0++;

            f0 = MULSHIFT32(w0, in);
            f1 = MULSHIFT32(w1, in);

            /* from last short block */
            *(over0 - 128) -= f0;
            *(over1 - 128) += f1;

            in = *buf1--;
            *over1-- = MULSHIFT32(w0, in);
            *over0++ = MULSHIFT32(w1, in);
        } while (over0 < over1);
    }

    /* over[576-1024] = 0 */
    i = 448;
    over0 += 64;
    do {
        *over0++ = 0;
        *over0++ = 0;
        *over0++ = 0;
        *over0++ = 0;
        i -= 4;
    } while (i);
}
/***********************************************************************************************************************
 * Function:    PreMultiply64
 *
 * Description: pre-twiddle stage of 64-point DCT-IV
 *
 * Inputs:      buffer of 64 samples
 *
 * Outputs:     processed samples in same buffer
 *
 * Return:      none
 *
 * Notes:       minimum 1 GB in, 2 GB out, gains 2 int32_t bits
 *              gbOut = gbIn + 1
 *              output is limited to sqrt(2)/2 plus GB in full GB
 *              uses 3-mul, 3-add butterflies instead of 4-mul, 2-add
 **********************************************************************************************************************/
void AACDecoder::PreMultiply64(int32_t *zbuf1) {
    int32_t i, ar1, ai1, ar2, ai2, z1, z2;
    int32_t t, cms2, cps2a, sin2a, cps2b, sin2b;
    int32_t *zbuf2;
    const int32_t *csptr;

    zbuf2 = zbuf1 + 64 - 1;
    csptr = cos4sin4tab64;

    /* whole thing should fit in registers - verify that compiler does this */
    for (i = 64 >> 2; i != 0; i--) {
        /* cps2 = (cos+sin), sin2 = sin, cms2 = (cos-sin) */
        cps2a = *csptr++;
        sin2a = *csptr++;
        cps2b = *csptr++;
        sin2b = *csptr++;

        ar1 = *(zbuf1 + 0);
        ai2 = *(zbuf1 + 1);
        ai1 = *(zbuf2 + 0);
        ar2 = *(zbuf2 - 1);

        /* gain 2 ints bit from MULSHIFT32 by Q30
         * max per-sample gain (ignoring implicit scaling) = MAX(sin(angle)+cos(angle)) = 1.414
         * i.e. gain 1 GB since worst case is sin(angle) = cos(angle) = 0.707 (Q30), gain 2 from
         *   extra sign bits, and eat one in adding
         */
        t = MULSHIFT32(sin2a, ar1 + ai1);
        z2 = MULSHIFT32(cps2a, ai1) - t;
        cms2 = cps2a - 2 * sin2a;
        z1 = MULSHIFT32(cms2, ar1) + t;
        *zbuf1++ = z1; /* cos*ar1 + sin*ai1 */
        *zbuf1++ = z2; /* cos*ai1 - sin*ar1 */

        t = MULSHIFT32(sin2b, ar2 + ai2);
        z2 = MULSHIFT32(cps2b, ai2) - t;
        cms2 = cps2b - 2 * sin2b;
        z1 = MULSHIFT32(cms2, ar2) + t;
        *zbuf2-- = z2; /* cos*ai2 - sin*ar2 */
        *zbuf2-- = z1; /* cos*ar2 + sin*ai2 */
    }
}
/***********************************************************************************************************************
 * Function:    PostMultiply64
 *
 * Description: post-twiddle stage of 64-point type-IV DCT
 *
 * Inputs:      buffer of 64 samples
 *              number of output samples to calculate
 *
 * Outputs:     processed samples in same buffer
 *
 * Return:      none
 *
 * Notes:       minimum 1 GB in, 2 GB out, gains 2 int32_t bits
 *              gbOut = gbIn + 1
 *              output is limited to sqrt(2)/2 plus GB in full GB
 *              nSampsOut is rounded up to next multiple of 4, since we calculate
 *                4 samples per loop
 **********************************************************************************************************************/
void AACDecoder::PostMultiply64(int32_t *fft1, int32_t nSampsOut) {
    int32_t i, ar1, ai1, ar2, ai2;
    int32_t t, cms2, cps2, sin2;
    int32_t *fft2;
    const int32_t *csptr;

    csptr = cos1sin1tab64;
    fft2 = fft1 + 64 - 1;

    /* load coeffs for first pass
     * cps2 = (cos+sin)/2, sin2 = sin/2, cms2 = (cos-sin)/2
     */
    cps2 = *csptr++;
    sin2 = *csptr++;
    cms2 = cps2 - 2 * sin2;

    for (i = (nSampsOut + 3) >> 2; i != 0; i--) {
        ar1 = *(fft1 + 0);
        ai1 = *(fft1 + 1);
        ar2 = *(fft2 - 1);
        ai2 = *(fft2 + 0);

        /* gain 2 int32_t bits (multiplying by Q30), max gain = sqrt(2) */
        t = MULSHIFT32(sin2, ar1 + ai1);
        *fft2-- = t - MULSHIFT32(cps2, ai1);
        *fft1++ = t + MULSHIFT32(cms2, ar1);

        cps2 = *csptr++;
        sin2 = *csptr++;

        ai2 = -ai2;
        t = MULSHIFT32(sin2, ar2 + ai2);
        *fft2-- = t - MULSHIFT32(cps2, ai2);
        cms2 = cps2 - 2 * sin2;
        *fft1++ = t + MULSHIFT32(cms2, ar2);
    }
}
/***********************************************************************************************************************
 * Function:    QMFAnalysisConv
 *
 * Description: convolution kernel for analysis QMF
 *
 * Inputs:      pointer to coefficient table, reordered for sequential access
 *              delay buffer of size 32*10 = 320 real-valued PCM samples
 *              index for delay ring buffer (range = [0, 9])
 *
 * Outputs:     64 consecutive 32-bit samples
 *
 * Return:      none
 *
 * Notes:       this is carefully written to be efficient on ARM
 *              use the assembly code version in sbrqmfak.s when building for ARM!
 **********************************************************************************************************************/
void AACDecoder::QMFAnalysisConv(int32_t *cTab, int32_t *delay, int32_t dIdx, int32_t *uBuf) {
    int32_t k, dOff;
    int32_t *cPtr0, *cPtr1;
    U64 u64lo, u64hi;

    dOff = dIdx * 32 + 31;
    cPtr0 = cTab;
    cPtr1 = cTab + 33 * 5 - 1;

    /* special first pass since we need to flip sign to create cTab[384], cTab[512] */
    u64lo.w64 = 0;
    u64hi.w64 = 0;
    u64lo.w64 = MADD64(u64lo.w64, *cPtr0++, delay[dOff]);
    dOff -= 32;
    if (dOff < 0) {
        dOff += 320;
    }
    u64hi.w64 = MADD64(u64hi.w64, *cPtr0++, delay[dOff]);
    dOff -= 32;
    if (dOff < 0) {
        dOff += 320;
    }
    u64lo.w64 = MADD64(u64lo.w64, *cPtr0++, delay[dOff]);
    dOff -= 32;
    if (dOff < 0) {
        dOff += 320;
    }
    u64hi.w64 = MADD64(u64hi.w64, *cPtr0++, delay[dOff]);
    dOff -= 32;
    if (dOff < 0) {
        dOff += 320;
    }
    u64lo.w64 = MADD64(u64lo.w64, *cPtr0++, delay[dOff]);
    dOff -= 32;
    if (dOff < 0) {
        dOff += 320;
    }
    u64hi.w64 = MADD64(u64hi.w64, *cPtr1--, delay[dOff]);
    dOff -= 32;
    if (dOff < 0) {
        dOff += 320;
    }
    u64lo.w64 = MADD64(u64lo.w64, -(*cPtr1--), delay[dOff]);
    dOff -= 32;
    if (dOff < 0) {
        dOff += 320;
    }
    u64hi.w64 = MADD64(u64hi.w64, *cPtr1--, delay[dOff]);
    dOff -= 32;
    if (dOff < 0) {
        dOff += 320;
    }
    u64lo.w64 = MADD64(u64lo.w64, -(*cPtr1--), delay[dOff]);
    dOff -= 32;
    if (dOff < 0) {
        dOff += 320;
    }
    u64hi.w64 = MADD64(u64hi.w64, *cPtr1--, delay[dOff]);
    dOff -= 32;
    if (dOff < 0) {
        dOff += 320;
    }

    uBuf[0] = u64lo.r.hi32;
    uBuf[32] = u64hi.r.hi32;
    uBuf++;
    dOff--;

    /* max gain for any sample in uBuf, after scaling by cTab, ~= 0.99
     * so we can just sum the uBuf values with no overflow problems
     */
    for (k = 1; k <= 31; k++) {
        u64lo.w64 = 0;
        u64hi.w64 = 0;
        u64lo.w64 = MADD64(u64lo.w64, *cPtr0++, delay[dOff]);
        dOff -= 32;
        if (dOff < 0) {
            dOff += 320;
        }
        u64hi.w64 = MADD64(u64hi.w64, *cPtr0++, delay[dOff]);
        dOff -= 32;
        if (dOff < 0) {
            dOff += 320;
        }
        u64lo.w64 = MADD64(u64lo.w64, *cPtr0++, delay[dOff]);
        dOff -= 32;
        if (dOff < 0) {
            dOff += 320;
        }
        u64hi.w64 = MADD64(u64hi.w64, *cPtr0++, delay[dOff]);
        dOff -= 32;
        if (dOff < 0) {
            dOff += 320;
        }
        u64lo.w64 = MADD64(u64lo.w64, *cPtr0++, delay[dOff]);
        dOff -= 32;
        if (dOff < 0) {
            dOff += 320;
        }
        u64hi.w64 = MADD64(u64hi.w64, *cPtr1--, delay[dOff]);
        dOff -= 32;
        if (dOff < 0) {
            dOff += 320;
        }
        u64lo.w64 = MADD64(u64lo.w64, *cPtr1--, delay[dOff]);
        dOff -= 32;
        if (dOff < 0) {
            dOff += 320;
        }
        u64hi.w64 = MADD64(u64hi.w64, *cPtr1--, delay[dOff]);
        dOff -= 32;
        if (dOff < 0) {
            dOff += 320;
        }
        u64lo.w64 = MADD64(u64lo.w64, *cPtr1--, delay[dOff]);
        dOff -= 32;
        if (dOff < 0) {
            dOff += 320;
        }
        u64hi.w64 = MADD64(u64hi.w64, *cPtr1--, delay[dOff]);
        dOff -= 32;
        if (dOff < 0) {
            dOff += 320;
        }

        uBuf[0] = u64lo.r.hi32;
        uBuf[32] = u64hi.r.hi32;
        uBuf++;
        dOff--;
    }
}
/***********************************************************************************************************************
 * Function:    QMFAnalysis
 *
 * Description: 32-subband analysis QMF (4.6.18.4.1)
 *
 * Inputs:      32 consecutive samples of decoded 32-bit PCM, format = Q(fBitsIn)
 *              delay buffer of size 32*10 = 320 PCM samples
 *              number of fraction bits in input PCM
 *              index for delay ring buffer (range = [0, 9])
 *              number of subbands to calculate (range = [0, 32])
 *
 * Outputs:     qmfaBands complex subband samples, format = Q(FBITS_OUT_QMFA)
 *              updated delay buffer
 *              updated delay index
 *
 * Return:      guard bit mask
 *
 * Notes:       output stored as RE{X0}, IM{X0}, RE{X1}, IM{X1}, ... RE{X31}, IM{X31}
 *              output stored in int32_t buffer of size 64*2 = 128
 *                (zero-filled from XBuf[2*qmfaBands] to XBuf[127])
 **********************************************************************************************************************/
int32_t AACDecoder::QMFAnalysis(int32_t *inbuf, int32_t *delay, int32_t *XBuf, int32_t fBitsIn, int32_t *delayIdx,
                                int32_t qmfaBands) {
    int32_t n, y, shift, gbMask;
    int32_t *delayPtr, *uBuf, *tBuf;

    /* use XBuf[128] as temp buffer for reordering */
    uBuf = XBuf;      /* first 64 samples */
    tBuf = XBuf + 64; /* second 64 samples */

    /* overwrite oldest PCM with new PCM
     * delay[n] has 1 GB after shifting (either << or >>)
     */
    delayPtr = delay + (*delayIdx * 32);
    if (fBitsIn > FBITS_IN_QMFA) {
        shift = MIN(fBitsIn - FBITS_IN_QMFA, (int32_t)31);
        for (n = 32; n != 0; n--) {
            y = (*inbuf) >> shift;
            inbuf++;
            *delayPtr++ = y;
        }
    } else {
        shift = MIN(FBITS_IN_QMFA - fBitsIn, (int32_t)30);
        for (n = 32; n != 0; n--) {
            y = *inbuf++;
            y = CLIP_2N_SHIFT30(y, shift);
            *delayPtr++ = y;
        }
    }

    QMFAnalysisConv((int32_t *)cTabA, delay, *delayIdx, uBuf);

    /* uBuf has at least 2 GB right now (1 from clipping to Q(FBITS_IN_QMFA), one from
     *   the scaling by cTab (MULSHIFT32(*delayPtr--, *cPtr++), with net gain of < 1.0)
     */
    tBuf[2 * 0 + 0] = uBuf[0];
    tBuf[2 * 0 + 1] = uBuf[1];
    for (n = 1; n < 31; n++) {
        tBuf[2 * n + 0] = -uBuf[64 - n];
        tBuf[2 * n + 1] = uBuf[n + 1];
    }
    tBuf[2 * 31 + 1] = uBuf[32];
    tBuf[2 * 31 + 0] = -uBuf[33];

    /* fast in-place DCT-IV - only need 2*qmfaBands output samples */
    PreMultiply64(tBuf);                 /* 2 GB in, 3 GB out */
    FFT32C(tBuf);                        /* 3 GB in, 1 GB out */
    PostMultiply64(tBuf, qmfaBands * 2); /* 1 GB in, 2 GB out */

    gbMask = 0;
    for (n = 0; n < qmfaBands; n++) {
        XBuf[2 * n + 0] = tBuf[n + 0]; /* implicit scaling of 2 in our output Q format */
        gbMask |= FASTABS(XBuf[2 * n + 0]);
        XBuf[2 * n + 1] = -tBuf[63 - n];
        gbMask |= FASTABS(XBuf[2 * n + 1]);
    }

    /* fill top section with zeros for HF generation */
    for (; n < 64; n++) {
        XBuf[2 * n + 0] = 0;
        XBuf[2 * n + 1] = 0;
    }

    *delayIdx = (*delayIdx == NUM_QMF_DELAY_BUFS - 1 ? 0 : *delayIdx + 1);

    /* minimum of 2 GB in output */
    return gbMask;
}
/***********************************************************************************************************************
 * Function:    QMFSynthesisConv
 *
 * Description: final convolution kernel for synthesis QMF
 *
 * Inputs:      pointer to coefficient table, reordered for sequential access
 *              delay buffer of size 64*10 = 640 complex samples (1280 ints)
 *              index for delay ring buffer (range = [0, 9])
 *              number of QMF subbands to process (range = [0, 64])
 *              number of channels
 *
 * Outputs:     64 consecutive 16-bit PCM samples, interleaved by factor of nChans
 *
 * Return:      none
 *
 * Notes:       this is carefully written to be efficient on ARM
 *              use the assembly code version in sbrqmfsk.s when building for ARM!
 **********************************************************************************************************************/
void AACDecoder::QMFSynthesisConv(int32_t *cPtr, int32_t *delay, int32_t dIdx, int16_t *outbuf, int32_t nChans) {
    int32_t k, dOff0, dOff1;
    U64 sum64;

    dOff0 = (dIdx) * 128;
    dOff1 = dOff0 - 1;
    if (dOff1 < 0)
        dOff1 += 1280;

    /* scaling note: total gain of coefs (cPtr[0]-cPtr[9] for any k) is < 2.0, so 1 GB in delay values is adequate */
    for (k = 0; k <= 63; k++) {
        sum64.w64 = 0;
        sum64.w64 = MADD64(sum64.w64, *cPtr++, delay[dOff0]);
        dOff0 -= 256;
        if (dOff0 < 0) {
            dOff0 += 1280;
        }
        sum64.w64 = MADD64(sum64.w64, *cPtr++, delay[dOff1]);
        dOff1 -= 256;
        if (dOff1 < 0) {
            dOff1 += 1280;
        }
        sum64.w64 = MADD64(sum64.w64, *cPtr++, delay[dOff0]);
        dOff0 -= 256;
        if (dOff0 < 0) {
            dOff0 += 1280;
        }
        sum64.w64 = MADD64(sum64.w64, *cPtr++, delay[dOff1]);
        dOff1 -= 256;
        if (dOff1 < 0) {
            dOff1 += 1280;
        }
        sum64.w64 = MADD64(sum64.w64, *cPtr++, delay[dOff0]);
        dOff0 -= 256;
        if (dOff0 < 0) {
            dOff0 += 1280;
        }
        sum64.w64 = MADD64(sum64.w64, *cPtr++, delay[dOff1]);
        dOff1 -= 256;
        if (dOff1 < 0) {
            dOff1 += 1280;
        }
        sum64.w64 = MADD64(sum64.w64, *cPtr++, delay[dOff0]);
        dOff0 -= 256;
        if (dOff0 < 0) {
            dOff0 += 1280;
        }
        sum64.w64 = MADD64(sum64.w64, *cPtr++, delay[dOff1]);
        dOff1 -= 256;
        if (dOff1 < 0) {
            dOff1 += 1280;
        }
        sum64.w64 = MADD64(sum64.w64, *cPtr++, delay[dOff0]);
        dOff0 -= 256;
        if (dOff0 < 0) {
            dOff0 += 1280;
        }
        sum64.w64 = MADD64(sum64.w64, *cPtr++, delay[dOff1]);
        dOff1 -= 256;
        if (dOff1 < 0) {
            dOff1 += 1280;
        }

        dOff0++;
        dOff1--;
        *outbuf = CLIPTOSHORT((sum64.r.hi32 + RND_VAL) >> FBITS_OUT_QMFS);
        outbuf += nChans;
    }
}
/***********************************************************************************************************************
 * Function:    QMFSynthesis
 *
 * Description: 64-subband synthesis QMF (4.6.18.4.2)
 *
 * Inputs:      64 consecutive complex subband QMF samples, format = Q(FBITS_IN_QMFS)
 *              delay buffer of size 64*10 = 640 complex samples (1280 ints)
 *              index for delay ring buffer (range = [0, 9])
 *              number of QMF subbands to process (range = [0, 64])
 *              number of channels
 *
 * Outputs:     64 consecutive 16-bit PCM samples, interleaved by factor of nChans
 *              updated delay buffer
 *              updated delay index
 *
 * Return:      none
 *
 * Notes:       assumes MIN_GBITS_IN_QMFS guard bits in input, either from
 *                QMFAnalysis (if upsampling only) or from MapHF (if SBR on)
 **********************************************************************************************************************/
void AACDecoder::QMFSynthesis(int32_t *inbuf, int32_t *delay, int32_t *delayIdx, int32_t qmfsBands, int16_t *outbuf,
                              int32_t nChans) {
    int32_t n, a0, a1, b0, b1, dOff0, dOff1, dIdx;
    int32_t *tBufLo, *tBufHi;

    dIdx = *delayIdx;
    tBufLo = delay + dIdx * 128 + 0;
    tBufHi = delay + dIdx * 128 + 127;

    /* reorder inputs to DCT-IV, only use first qmfsBands (complex) samples
     */
    for (n = 0; n < qmfsBands >> 1; n++) {
        a0 = *inbuf++;
        b0 = *inbuf++;
        a1 = *inbuf++;
        b1 = *inbuf++;
        *tBufLo++ = a0;
        *tBufLo++ = a1;
        *tBufHi-- = b0;
        *tBufHi-- = b1;
    }
    if (qmfsBands & 0x01) {
        a0 = *inbuf++;
        b0 = *inbuf++;
        *tBufLo++ = a0;
        *tBufHi-- = b0;
        *tBufLo++ = 0;
        *tBufHi-- = 0;
        n++;
    }
    for (; n < 32; n++) {
        *tBufLo++ = 0;
        *tBufHi-- = 0;
        *tBufLo++ = 0;
        *tBufHi-- = 0;
    }

    tBufLo = delay + dIdx * 128 + 0;
    tBufHi = delay + dIdx * 128 + 64;

    /* 2 GB in, 3 GB out */
    PreMultiply64(tBufLo);
    PreMultiply64(tBufHi);

    /* 3 GB in, 1 GB out */
    FFT32C(tBufLo);
    FFT32C(tBufHi);

    /* 1 GB in, 2 GB out */
    PostMultiply64(tBufLo, 64);
    PostMultiply64(tBufHi, 64);

    /* could fuse with PostMultiply64 to avoid separate pass */
    dOff0 = dIdx * 128;
    dOff1 = dIdx * 128 + 64;
    for (n = 32; n != 0; n--) {
        a0 = (*tBufLo++);
        a1 = (*tBufLo++);
        b0 = (*tBufHi++);
        b1 = -(*tBufHi++);

        delay[dOff0++] = (b0 - a0);
        delay[dOff0++] = (b1 - a1);
        delay[dOff1++] = (b0 + a0);
        delay[dOff1++] = (b1 + a1);
    }

    QMFSynthesisConv((int32_t *)cTabS, delay, dIdx, outbuf, nChans);

    *delayIdx = (*delayIdx == NUM_QMF_DELAY_BUFS - 1 ? 0 : *delayIdx + 1);
}
/***********************************************************************************************************************
 * Function:    UnpackSBRHeader
 *
 * Description: unpack SBR header (table 4.56)
 *
 * Inputs:      BitStreamInfo struct pointing to start of SBR header
 *
 * Outputs:     initialized SBRHeader struct for this SCE/CPE block
 *
 * Return:      non-zero if frame reset is triggered, zero otherwise
 **********************************************************************************************************************/
int32_t AACDecoder::UnpackSBRHeader(SBRHeader *sbrHdr) {
    SBRHeader sbrHdrPrev;

    /* save previous values so we know whether to reset decoder */
    sbrHdrPrev.startFreq = sbrHdr->startFreq;
    sbrHdrPrev.stopFreq = sbrHdr->stopFreq;
    sbrHdrPrev.freqScale = sbrHdr->freqScale;
    sbrHdrPrev.alterScale = sbrHdr->alterScale;
    sbrHdrPrev.crossOverBand = sbrHdr->crossOverBand;
    sbrHdrPrev.noiseBands = sbrHdr->noiseBands;

    sbrHdr->ampRes = GetBits(1);
    sbrHdr->startFreq = GetBits(4);
    sbrHdr->stopFreq = GetBits(4);
    sbrHdr->crossOverBand = GetBits(3);
    sbrHdr->resBitsHdr = GetBits(2);
    sbrHdr->hdrExtra1 = GetBits(1);
    sbrHdr->hdrExtra2 = GetBits(1);

    if (sbrHdr->hdrExtra1) {
        sbrHdr->freqScale = GetBits(2);
        sbrHdr->alterScale = GetBits(1);
        sbrHdr->noiseBands = GetBits(2);
    } else {
        /* defaults */
        sbrHdr->freqScale = 2;
        sbrHdr->alterScale = 1;
        sbrHdr->noiseBands = 2;
    }

    if (sbrHdr->hdrExtra2) {
        sbrHdr->limiterBands = GetBits(2);
        sbrHdr->limiterGains = GetBits(2);
        sbrHdr->interpFreq = GetBits(1);
        sbrHdr->smoothMode = GetBits(1);
    } else {
        /* defaults */
        sbrHdr->limiterBands = 2;
        sbrHdr->limiterGains = 2;
        sbrHdr->interpFreq = 1;
        sbrHdr->smoothMode = 1;
    }
    sbrHdr->count++;

    /* if any of these have changed from previous frame, reset the SBR module */
    if (sbrHdr->startFreq != sbrHdrPrev.startFreq || sbrHdr->stopFreq != sbrHdrPrev.stopFreq ||
        sbrHdr->freqScale != sbrHdrPrev.freqScale || sbrHdr->alterScale != sbrHdrPrev.alterScale ||
        sbrHdr->crossOverBand != sbrHdrPrev.crossOverBand || sbrHdr->noiseBands != sbrHdrPrev.noiseBands)
        return -1;
    else
        return 0;
}

/* cLog2[i] = ceil(log2(i)) (disregard i == 0) */
static const uint8_t cLog2[9] = {0, 0, 1, 2, 2, 3, 3, 3, 3};
/***********************************************************************************************************************
 * Function:    UnpackSBRGrid
 *
 * Description: unpack SBR grid (table 4.62)
 *
 * Inputs:      BitStreamInfo struct pointing to start of SBR grid
 *              initialized SBRHeader struct for this SCE/CPE block
 *
 * Outputs:     initialized SBRGrid struct for this channel
 *
 * Return:      none
 **********************************************************************************************************************/
void AACDecoder::UnpackSBRGrid(SBRHeader *sbrHdr, SBRGrid *sbrGrid) {
    int32_t numEnvRaw, env, rel, pBits, border, middleBorder = 0;
    uint8_t relBordLead[MAX_NUM_ENV], relBordTrail[MAX_NUM_ENV];
    uint8_t relBorder0[3], relBorder1[3], relBorder[3];
    uint8_t numRelBorder0, numRelBorder1, numRelBorder, numRelLead = 0, numRelTrail;
    uint8_t absBordLead = 0, absBordTrail = 0, absBorder;

    sbrGrid->ampResFrame = sbrHdr->ampRes;
    sbrGrid->frameClass = GetBits(2);
    switch (sbrGrid->frameClass) {
    case SBR_GRID_FIXFIX:
        numEnvRaw = GetBits(2);
        sbrGrid->numEnv = (1 << numEnvRaw);
        if (sbrGrid->numEnv == 1)
            sbrGrid->ampResFrame = 0;

        ASSERT(sbrGrid->numEnv == 1 || sbrGrid->numEnv == 2 || sbrGrid->numEnv == 4);

        sbrGrid->freqRes[0] = GetBits(1);
        for (env = 1; env < sbrGrid->numEnv; env++)
            sbrGrid->freqRes[env] = sbrGrid->freqRes[0];

        absBordLead = 0;
        absBordTrail = NUM_TIME_SLOTS;
        numRelLead = sbrGrid->numEnv - 1;
        numRelTrail = 0;

        /* numEnv = 1, 2, or 4 */
        if (sbrGrid->numEnv == 1)
            border = NUM_TIME_SLOTS / 1;
        else if (sbrGrid->numEnv == 2)
            border = NUM_TIME_SLOTS / 2;
        else
            border = NUM_TIME_SLOTS / 4;

        for (rel = 0; rel < numRelLead; rel++)
            relBordLead[rel] = border;

        middleBorder = (sbrGrid->numEnv >> 1);

        break;

    case SBR_GRID_FIXVAR:
        absBorder = GetBits(2) + NUM_TIME_SLOTS;
        numRelBorder = GetBits(2);
        sbrGrid->numEnv = numRelBorder + 1;
        for (rel = 0; rel < numRelBorder; rel++)
            relBorder[rel] = 2 * GetBits(2) + 2;

        pBits = cLog2[sbrGrid->numEnv + 1];
        sbrGrid->pointer = GetBits(pBits);

        for (env = sbrGrid->numEnv - 1; env >= 0; env--)
            sbrGrid->freqRes[env] = GetBits(1);

        absBordLead = 0;
        absBordTrail = absBorder;
        numRelLead = 0;
        numRelTrail = numRelBorder;

        for (rel = 0; rel < numRelTrail; rel++)
            relBordTrail[rel] = relBorder[rel];

        if (sbrGrid->pointer > 1)
            middleBorder = sbrGrid->numEnv + 1 - sbrGrid->pointer;
        else
            middleBorder = sbrGrid->numEnv - 1;

        break;

    case SBR_GRID_VARFIX:
        absBorder = GetBits(2);
        numRelBorder = GetBits(2);
        sbrGrid->numEnv = numRelBorder + 1;
        for (rel = 0; rel < numRelBorder; rel++)
            relBorder[rel] = 2 * GetBits(2) + 2;

        pBits = cLog2[sbrGrid->numEnv + 1];
        sbrGrid->pointer = GetBits(pBits);

        for (env = 0; env < sbrGrid->numEnv; env++)
            sbrGrid->freqRes[env] = GetBits(1);

        absBordLead = absBorder;
        absBordTrail = NUM_TIME_SLOTS;
        numRelLead = numRelBorder;
        numRelTrail = 0;

        for (rel = 0; rel < numRelLead; rel++)
            relBordLead[rel] = relBorder[rel];

        if (sbrGrid->pointer == 0)
            middleBorder = 1;
        else if (sbrGrid->pointer == 1)
            middleBorder = sbrGrid->numEnv - 1;
        else
            middleBorder = sbrGrid->pointer - 1;

        break;

    case SBR_GRID_VARVAR:
        absBordLead = GetBits(2);                   /* absBorder0 */
        absBordTrail = GetBits(2) + NUM_TIME_SLOTS; /* absBorder1 */
        numRelBorder0 = GetBits(2);
        numRelBorder1 = GetBits(2);

        sbrGrid->numEnv = numRelBorder0 + numRelBorder1 + 1;
        ASSERT(sbrGrid->numEnv <= 5);

        for (rel = 0; rel < numRelBorder0; rel++)
            relBorder0[rel] = 2 * GetBits(2) + 2;

        for (rel = 0; rel < numRelBorder1; rel++)
            relBorder1[rel] = 2 * GetBits(2) + 2;

        pBits = cLog2[numRelBorder0 + numRelBorder1 + 2];
        sbrGrid->pointer = GetBits(pBits);

        for (env = 0; env < sbrGrid->numEnv; env++)
            sbrGrid->freqRes[env] = GetBits(1);

        numRelLead = numRelBorder0;
        numRelTrail = numRelBorder1;

        for (rel = 0; rel < numRelLead; rel++)
            relBordLead[rel] = relBorder0[rel];

        for (rel = 0; rel < numRelTrail; rel++)
            relBordTrail[rel] = relBorder1[rel];

        if (sbrGrid->pointer > 1)
            middleBorder = sbrGrid->numEnv + 1 - sbrGrid->pointer;
        else
            middleBorder = sbrGrid->numEnv - 1;

        break;
    }

    /* build time border vector */
    sbrGrid->envTimeBorder[0] = absBordLead * SAMPLES_PER_SLOT;

    rel = 0;
    border = absBordLead;
    for (env = 1; env <= numRelLead; env++) {
        border += relBordLead[rel++];
        sbrGrid->envTimeBorder[env] = border * SAMPLES_PER_SLOT;
    }

    rel = 0;
    border = absBordTrail;
    for (env = sbrGrid->numEnv - 1; env > numRelLead; env--) {
        border -= relBordTrail[rel++];
        sbrGrid->envTimeBorder[env] = border * SAMPLES_PER_SLOT;
    }

    sbrGrid->envTimeBorder[sbrGrid->numEnv] = absBordTrail * SAMPLES_PER_SLOT;

    if (sbrGrid->numEnv > 1) {
        sbrGrid->numNoiseFloors = 2;
        sbrGrid->noiseTimeBorder[0] = sbrGrid->envTimeBorder[0];
        sbrGrid->noiseTimeBorder[1] = sbrGrid->envTimeBorder[middleBorder];
        sbrGrid->noiseTimeBorder[2] = sbrGrid->envTimeBorder[sbrGrid->numEnv];
    } else {
        sbrGrid->numNoiseFloors = 1;
        sbrGrid->noiseTimeBorder[0] = sbrGrid->envTimeBorder[0];
        sbrGrid->noiseTimeBorder[1] = sbrGrid->envTimeBorder[1];
    }
}
/***********************************************************************************************************************
 * Function:    UnpackDeltaTimeFreq
 *
 * Description: unpack time/freq flags for delta coding of SBR envelopes (table 4.63)
 *
 * Inputs:      BitStreamInfo struct pointing to start of dt/df flags
 *              number of envelopes
 *              number of noise floors
 *
 * Outputs:     delta flags for envelope and noise floors
 *
 * Return:      none
 **********************************************************************************************************************/
void AACDecoder::UnpackDeltaTimeFreq(int32_t numEnv, uint8_t *deltaFlagEnv, int32_t numNoiseFloors,
                                     uint8_t *deltaFlagNoise) {
    int32_t env, noiseFloor;

    for (env = 0; env < numEnv; env++)
        deltaFlagEnv[env] = GetBits(1);

    for (noiseFloor = 0; noiseFloor < numNoiseFloors; noiseFloor++)
        deltaFlagNoise[noiseFloor] = GetBits(1);
}
/***********************************************************************************************************************
 * Function:    UnpackInverseFilterMode
 *
 * Description: unpack invf flags for chirp factor calculation (table 4.64)
 *
 * Inputs:      BitStreamInfo struct pointing to start of invf flags
 *              number of noise floor bands
 *
 * Outputs:     invf flags for noise floor bands
 *
 * Return:      none
 **********************************************************************************************************************/
void AACDecoder::UnpackInverseFilterMode(int32_t numNoiseFloorBands, uint8_t *mode) {
    int32_t n;

    for (n = 0; n < numNoiseFloorBands; n++)
        mode[n] = GetBits(2);
}
/***********************************************************************************************************************
 * Function:    UnpackSinusoids
 *
 * Description: unpack sinusoid (harmonic) flags for each SBR subband (table 4.67)
 *
 * Inputs:      BitStreamInfo struct pointing to start of sinusoid flags
 *              number of high resolution SBR subbands (nHigh)
 *
 * Outputs:     sinusoid flags for each SBR subband, zero-filled above nHigh
 *
 * Return:      none
 **********************************************************************************************************************/
void AACDecoder::UnpackSinusoids(int32_t nHigh, int32_t addHarmonicFlag, uint8_t *addHarmonic) {
    int32_t n;

    n = 0;
    if (addHarmonicFlag) {
        for (; n < nHigh; n++)
            addHarmonic[n] = GetBits(1);
    }

    /* zero out unused bands */
    for (; n < MAX_QMF_BANDS; n++)
        addHarmonic[n] = 0;
}
/***********************************************************************************************************************
 * Function:    CopyCouplingGrid
 *
 * Description: copy grid parameters from left to right for channel coupling
 *
 * Inputs:      initialized SBRGrid struct for left channel
 *
 * Outputs:     initialized SBRGrid struct for right channel
 *
 * Return:      none
 **********************************************************************************************************************/
void AACDecoder::CopyCouplingGrid(SBRGrid *sbrGridLeft, SBRGrid *sbrGridRight) {
    int32_t env, noiseFloor;

    sbrGridRight->frameClass = sbrGridLeft->frameClass;
    sbrGridRight->ampResFrame = sbrGridLeft->ampResFrame;
    sbrGridRight->pointer = sbrGridLeft->pointer;

    sbrGridRight->numEnv = sbrGridLeft->numEnv;
    for (env = 0; env < sbrGridLeft->numEnv; env++) {
        sbrGridRight->envTimeBorder[env] = sbrGridLeft->envTimeBorder[env];
        sbrGridRight->freqRes[env] = sbrGridLeft->freqRes[env];
    }
    sbrGridRight->envTimeBorder[env] = sbrGridLeft->envTimeBorder[env]; /* borders are [0, numEnv] inclusive */

    sbrGridRight->numNoiseFloors = sbrGridLeft->numNoiseFloors;
    for (noiseFloor = 0; noiseFloor <= sbrGridLeft->numNoiseFloors; noiseFloor++)
        sbrGridRight->noiseTimeBorder[noiseFloor] = sbrGridLeft->noiseTimeBorder[noiseFloor];

    /* numEnvPrev, numNoiseFloorsPrev, freqResPrev are updated in DecodeSBREnvelope() and DecodeSBRNoise() */
}
/***********************************************************************************************************************
 * Function:    CopyCouplingInverseFilterMode
 *
 * Description: copy invf flags from left to right for channel coupling
 *
 * Inputs:      invf flags for left channel
 *              number of noise floor bands
 *
 * Outputs:     invf flags for right channel
 *
 * Return:      none
 **********************************************************************************************************************/
void AACDecoder::CopyCouplingInverseFilterMode(int32_t numNoiseFloorBands, uint8_t *modeLeft, uint8_t *modeRight) {
    int32_t band;

    for (band = 0; band < numNoiseFloorBands; band++)
        modeRight[band] = modeLeft[band];
}
/***********************************************************************************************************************
 * Function:    UnpackSBRSingleChannel
 *
 * Description: unpack sideband info (grid, delta flags, invf flags, envelope and
 *                noise floor configuration, sinusoids) for a single channel
 *
 * Inputs:      BitStreamInfo struct pointing to start of sideband info
 *              initialized PSInfoSBR struct (after parsing SBR header and building
 *                frequency tables)
 *              base output channel (range = [0, nChans-1])
 *
 * Outputs:     updated PSInfoSBR struct (SBRGrid and SBRChan)
 *
 * Return:      none
 **********************************************************************************************************************/
void AACDecoder::UnpackSBRSingleChannel(int32_t chBase) {
    int32_t bitsLeft;
    SBRHeader *sbrHdr = &(m_PSInfoSBR->sbrHdr[chBase]);
    SBRGrid *sbrGridL = &(m_PSInfoSBR->sbrGrid[chBase + 0]);
    SBRFreq *sbrFreq = &(m_PSInfoSBR->sbrFreq[chBase]);
    SBRChan *sbrChanL = &(m_PSInfoSBR->sbrChan[chBase + 0]);

    m_PSInfoSBR->dataExtra = GetBits(1);
    if (m_PSInfoSBR->dataExtra)
        m_PSInfoSBR->resBitsData = GetBits(4);

    UnpackSBRGrid(sbrHdr, sbrGridL);
    UnpackDeltaTimeFreq(sbrGridL->numEnv, sbrChanL->deltaFlagEnv, sbrGridL->numNoiseFloors, sbrChanL->deltaFlagNoise);
    UnpackInverseFilterMode(sbrFreq->numNoiseFloorBands, sbrChanL->invfMode[1]);

    DecodeSBREnvelope(sbrGridL, sbrFreq, sbrChanL, 0);
    DecodeSBRNoise(sbrGridL, sbrFreq, sbrChanL, 0);

    sbrChanL->addHarmonicFlag[1] = GetBits(1);
    UnpackSinusoids(sbrFreq->nHigh, sbrChanL->addHarmonicFlag[1], sbrChanL->addHarmonic[1]);

    m_PSInfoSBR->extendedDataPresent = GetBits(1);
    if (m_PSInfoSBR->extendedDataPresent) {
        m_PSInfoSBR->extendedDataSize = GetBits(4);
        if (m_PSInfoSBR->extendedDataSize == 15)
            m_PSInfoSBR->extendedDataSize += GetBits(8);

        bitsLeft = 8 * m_PSInfoSBR->extendedDataSize;

        /* get ID, unpack extension info, do whatever is necessary with it... */
        while (bitsLeft > 0) {
            GetBits(8);
            bitsLeft -= 8;
        }
    }
}
/***********************************************************************************************************************
 * Function:    UnpackSBRChannelPair
 *
 * Description: unpack sideband info (grid, delta flags, invf flags, envelope and
 *                noise floor configuration, sinusoids) for a channel pair
 *
 * Inputs:      base output channel (range = [0, nChans-1])
 *
 * Outputs:     updated PSInfoSBR struct (SBRGrid and SBRChan for both channels)
 *
 * Return:      none
 **********************************************************************************************************************/
void AACDecoder::UnpackSBRChannelPair(int32_t chBase) {
    int32_t bitsLeft;
    SBRHeader *sbrHdr = &(m_PSInfoSBR->sbrHdr[chBase]);
    SBRGrid *sbrGridL = &(m_PSInfoSBR->sbrGrid[chBase + 0]), *sbrGridR = &(m_PSInfoSBR->sbrGrid[chBase + 1]);
    SBRFreq *sbrFreq = &(m_PSInfoSBR->sbrFreq[chBase]);
    SBRChan *sbrChanL = &(m_PSInfoSBR->sbrChan[chBase + 0]), *sbrChanR = &(m_PSInfoSBR->sbrChan[chBase + 1]);

    m_PSInfoSBR->dataExtra = GetBits(1);
    if (m_PSInfoSBR->dataExtra) {
        m_PSInfoSBR->resBitsData = GetBits(4);
        m_PSInfoSBR->resBitsData = GetBits(4);
    }

    m_PSInfoSBR->couplingFlag = GetBits(1);
    if (m_PSInfoSBR->couplingFlag) {
        UnpackSBRGrid(sbrHdr, sbrGridL);
        CopyCouplingGrid(sbrGridL, sbrGridR);

        UnpackDeltaTimeFreq(sbrGridL->numEnv, sbrChanL->deltaFlagEnv, sbrGridL->numNoiseFloors,
                            sbrChanL->deltaFlagNoise);
        UnpackDeltaTimeFreq(sbrGridR->numEnv, sbrChanR->deltaFlagEnv, sbrGridR->numNoiseFloors,
                            sbrChanR->deltaFlagNoise);

        UnpackInverseFilterMode(sbrFreq->numNoiseFloorBands, sbrChanL->invfMode[1]);
        CopyCouplingInverseFilterMode(sbrFreq->numNoiseFloorBands, sbrChanL->invfMode[1], sbrChanR->invfMode[1]);

        DecodeSBREnvelope(sbrGridL, sbrFreq, sbrChanL, 0);
        DecodeSBRNoise(sbrGridL, sbrFreq, sbrChanL, 0);
        DecodeSBREnvelope(sbrGridR, sbrFreq, sbrChanR, 1);
        DecodeSBRNoise(sbrGridR, sbrFreq, sbrChanR, 1);

        /* pass RIGHT sbrChan struct */
        UncoupleSBREnvelope(sbrGridL, sbrFreq, sbrChanR);
        UncoupleSBRNoise(sbrGridL, sbrFreq, sbrChanR);

    } else {
        UnpackSBRGrid(sbrHdr, sbrGridL);
        UnpackSBRGrid(sbrHdr, sbrGridR);
        UnpackDeltaTimeFreq(sbrGridL->numEnv, sbrChanL->deltaFlagEnv, sbrGridL->numNoiseFloors,
                            sbrChanL->deltaFlagNoise);
        UnpackDeltaTimeFreq(sbrGridR->numEnv, sbrChanR->deltaFlagEnv, sbrGridR->numNoiseFloors,
                            sbrChanR->deltaFlagNoise);
        UnpackInverseFilterMode(sbrFreq->numNoiseFloorBands, sbrChanL->invfMode[1]);
        UnpackInverseFilterMode(sbrFreq->numNoiseFloorBands, sbrChanR->invfMode[1]);

        DecodeSBREnvelope(sbrGridL, sbrFreq, sbrChanL, 0);
        DecodeSBREnvelope(sbrGridR, sbrFreq, sbrChanR, 1);
        DecodeSBRNoise(sbrGridL, sbrFreq, sbrChanL, 0);
        DecodeSBRNoise(sbrGridR, sbrFreq, sbrChanR, 1);
    }

    sbrChanL->addHarmonicFlag[1] = GetBits(1);
    UnpackSinusoids(sbrFreq->nHigh, sbrChanL->addHarmonicFlag[1], sbrChanL->addHarmonic[1]);

    sbrChanR->addHarmonicFlag[1] = GetBits(1);
    UnpackSinusoids(sbrFreq->nHigh, sbrChanR->addHarmonicFlag[1], sbrChanR->addHarmonic[1]);

    m_PSInfoSBR->extendedDataPresent = GetBits(1);
    if (m_PSInfoSBR->extendedDataPresent) {
        m_PSInfoSBR->extendedDataSize = GetBits(4);
        if (m_PSInfoSBR->extendedDataSize == 15)
            m_PSInfoSBR->extendedDataSize += GetBits(8);

        bitsLeft = 8 * m_PSInfoSBR->extendedDataSize;

        /* get ID, unpack extension info, do whatever is necessary with it... */
        while (bitsLeft > 0) {
            GetBits(8);
            bitsLeft -= 8;
        }
    }
}
