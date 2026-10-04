// based on helix mp3 decoder
#pragma once

#include "Arduino.h"
#include "assert.h"

static constexpr uint8_t  m_HUFF_PAIRTABS          =32;
static constexpr uint8_t  m_BLOCK_SIZE             =18;
static constexpr uint8_t  m_NBANDS                 =32;
static constexpr uint8_t  m_MAX_REORDER_SAMPS      =(192-126)*3;      // largest critical band for short blocks (see sfBandTable)
static constexpr uint16_t m_VBUF_LENGTH            =17*2* m_NBANDS;    // for double-sized vbuf FIFO
static constexpr uint8_t  m_MAX_SCFBD              =4;     // max scalefactor bands per channel
static constexpr uint16_t m_MAINBUF_SIZE           =1940;
static constexpr uint8_t  m_MAX_NGRAN              =2;     // max granules
static constexpr uint8_t  m_MAX_NCHAN              =2;     // max channels
static constexpr uint16_t m_MAX_NSAMP              =576;   // max samples per channel, per granule

enum {
    ERR_MP3_NONE =                  0,
    ERR_MP3_INDATA_UNDERFLOW =     -1,
    ERR_MP3_MAINDATA_UNDERFLOW =   -2,
    ERR_MP3_FREE_BITRATE_SYNC =    -3,
    ERR_MP3_OUT_OF_MEMORY =        -4,
    ERR_MP3_NULL_POINTER =         -5,
    ERR_MP3_INVALID_FRAMEHEADER =  -6,
    ERR_MP3_INVALID_SIDEINFO =     -7,
    ERR_MP3_INVALID_SCALEFACT =    -8,
    ERR_MP3_INVALID_HUFFCODES =    -9,
    ERR_MP3_INVALID_DEQUANTIZE =   -10,
    ERR_MP3_INVALID_IMDCT =        -11,
    ERR_MP3_INVALID_SUBBAND =      -12,

    ERR_UNKNOWN =                  -9999
};

typedef struct MP3FrameInfo {
    int32_t bitrate;
    int32_t nChans;
    int32_t samprate;
    int32_t bitsPerSample;
    int32_t outputSamps;
    int32_t layer;
    int32_t version;
} MP3FrameInfo_t;

typedef struct SFBandTable {
    int32_t l[23];
    int32_t s[14];
} SFBandTable_t;

typedef struct BitStreamInfo {
    uint8_t *bytePtr;
    uint32_t iCache;
    int32_t cachedBits;
    int32_t nBytes;
} BitStreamInfo_t;

typedef enum {          /* map these to the corresponding 2-bit values in the frame header */
    Stereo = 0x00,      /* two independent channels, but L and R frames might have different # of bits */
    Joint = 0x01,       /* coupled channels - layer III: mix of M-S and intensity, Layers I/II: intensity and direct coding only */
    Dual = 0x02,        /* two independent channels, L and R always have exactly 1/2 the total bitrate */
    Mono = 0x03         /* one channel */
} StereoMode_t;

typedef enum {          /* map to 0,1,2 to make table indexing easier */
    MPEG1 =  0,
    MPEG2 =  1,
    MPEG25 = 2
} MPEGVersion_t;

typedef struct FrameHeader {
    int32_t layer;              /* layer index (1, 2, or 3) */
    int32_t crc;                /* CRC flag: 0 = disabled, 1 = enabled */
    int32_t brIdx;              /* bitrate index (0 - 15) */
    int32_t srIdx;              /* sample rate index (0 - 2) */
    int32_t paddingBit;         /* padding flag: 0 = no padding, 1 = single pad byte */
    int32_t privateBit;         /* unused */
    int32_t modeExt;            /* used to decipher joint stereo mode */
    int32_t copyFlag;           /* copyright flag: 0 = no, 1 = yes */
    int32_t origFlag;           /* original flag: 0 = copy, 1 = original */
    int32_t emphasis;           /* deemphasis mode */
    int32_t CRCWord;            /* CRC word (16 bits, 0 if crc not enabled) */
} FrameHeader_t;

typedef struct SideInfoSub {
    int32_t part23Length;       /* number of bits in main data */
    int32_t nBigvals;           /* 2x this = first set of Huffman cw's (maximum amplitude can be > 1) */
    int32_t globalGain;         /* overall gain for dequantizer */
    int32_t sfCompress;         /* unpacked to figure out number of bits in scale factors */
    int32_t winSwitchFlag;      /* window switching flag */
    int32_t blockType;          /* block type */
    int32_t mixedBlock;         /* 0 = regular block (all short or long), 1 = mixed block */
    int32_t tableSelect[3];     /* index of Huffman tables for the big values regions */
    int32_t subBlockGain[3];    /* subblock gain offset, relative to global gain */
    int32_t region0Count;       /* 1+region0Count = num scale factor bands in first region of bigvals */
    int32_t region1Count;       /* 1+region1Count = num scale factor bands in second region of bigvals */
    int32_t preFlag;            /* for optional high frequency boost */
    int32_t sfactScale;         /* scaling of the scalefactors */
    int32_t count1TableSelect;  /* index of Huffman table for quad codewords */
} SideInfoSub_t;

typedef struct SideInfo {
    int32_t mainDataBegin;
    int32_t privateBits;
    int32_t scfsi[m_MAX_NCHAN][m_MAX_SCFBD];                /* 4 scalefactor bands per channel */
} SideInfo_t;

typedef struct {
    int32_t cbType;             /* pure long = 0, pure short = 1, mixed = 2 */
    int32_t cbEndS[3];          /* number nonzero short cb's, per subbblock */
    int32_t cbEndSMax;          /* max of cbEndS[] */
    int32_t cbEndL;             /* number nonzero long cb's  */
} CriticalBandInfo_t;

typedef struct DequantInfo {
    int32_t workBuf[m_MAX_REORDER_SAMPS];             /* workbuf for reordering short blocks */
} DequantInfo_t;

typedef struct HuffmanInfo {
    int32_t huffDecBuf[m_MAX_NCHAN][m_MAX_NSAMP];       /* used both for decoded Huffman values and dequantized coefficients */
    int32_t nonZeroBound[m_MAX_NCHAN];                /* number of coeffs in huffDecBuf[ch] which can be > 0 */
    int32_t gb[m_MAX_NCHAN];                          /* minimum number of guard bits in huffDecBuf[ch] */
} HuffmanInfo_t;

typedef enum HuffTabType {
    noBits,
    oneShot,
    loopNoLinbits,
    loopLinbits,
    quadA,
    quadB,
    invalidTab
} HuffTabType_t;

typedef struct HuffTabLookup {
    int32_t linBits;
    int32_t  tabType; /*HuffTabType*/
} HuffTabLookup_t;

typedef struct IMDCTInfo {
    int32_t outBuf[m_MAX_NCHAN][m_BLOCK_SIZE][m_NBANDS];  /* output of IMDCT */
    int32_t overBuf[m_MAX_NCHAN][m_MAX_NSAMP / 2];      /* overlap-add buffer (by symmetry, only need 1/2 size) */
    int32_t numPrevIMDCT[m_MAX_NCHAN];                /* how many IMDCT's calculated in this channel on prev. granule */
    int32_t prevType[m_MAX_NCHAN];
    int32_t prevWinSwitch[m_MAX_NCHAN];
    int32_t gb[m_MAX_NCHAN];
} IMDCTInfo_t;

typedef struct BlockCount {
    int32_t nBlocksLong;
    int32_t nBlocksTotal;
    int32_t nBlocksPrev;
    int32_t prevType;
    int32_t prevWinSwitch;
    int32_t currWinSwitch;
    int32_t gbIn;
    int32_t gbOut;
} BlockCount_t;

typedef struct ScaleFactorInfoSub {    /* max bits in scalefactors = 5, so use char's to save space */
    char l[23];            /* [band] */
    char s[13][3];         /* [band][window] */
} ScaleFactorInfoSub_t;

typedef struct ScaleFactorJS { /* used in MPEG 2, 2.5 intensity (joint) stereo only */
    int32_t intensityScale;
    int32_t slen[4];
    int32_t nr[4];
} ScaleFactorJS_t;

/* NOTE - could get by with smaller vbuf if memory is more important than speed
 *  (in Subband, instead of replicating each block in FDCT32 you would do a memmove on the
 *   last 15 blocks to shift them down one, a hardware style FIFO)
 */
typedef struct SubbandInfo {
    int32_t vbuf[m_MAX_NCHAN * m_VBUF_LENGTH];      /* vbuf for fast DCT-based synthesis PQMF - double size for speed (no modulo indexing) */
    int32_t vindex;                             /* internal index for tracking position in vbuf */
} SubbandInfo_t;

typedef struct MP3DecInfo {
    /* buffer which must be large enough to hold largest possible main_data section */
    uint8_t mainBuf[m_MAINBUF_SIZE];
    /* special info for "free" bitrate files */
    int32_t freeBitrateFlag;
    int32_t freeBitrateSlots;
    /* user-accessible info */
    int32_t bitrate;
    int32_t nChans;
    int32_t samprate;
    int32_t nGrans;             /* granules per frame */
    int32_t nGranSamps;         /* samples per granule */
    int32_t nSlots;
    int32_t layer;

    int32_t mainDataBegin;
    int32_t mainDataBytes;
    int32_t part23Length[m_MAX_NGRAN][m_MAX_NCHAN];
} MP3DecInfo_t;


/* format = Q30, right shifted by 12 (sign bits only in top 12 - undo this when rounding to short)
 *   this is to enable early-terminating multiplies on ARM
 * range = [-1.144287109, 1.144989014]
 * max gain of filter (per output sample) ~= 2.731
 *
 * new (properly sign-flipped) values
 *  - these actually are correct to 32 bits, (floating-pt coefficients in spec
 *      chosen such that only ~20 bits are required)
 *
 * Reordering - see table 3-B.3 in spec (appendix B)
 *
 * polyCoef[i] =
 *   D[ 0, 32, 64, ... 480],   i = [  0, 15]
 *   D[ 1, 33, 65, ... 481],   i = [ 16, 31]
 *   D[ 2, 34, 66, ... 482],   i = [ 32, 47]
 *     ...
 *   D[15, 47, 79, ... 495],   i = [240,255]
 *
 * also exploits symmetry: D[i] = -D[512 - i], for i = [1, 255]
 *
 * polyCoef[256, 257, ... 263] are for special case of sample 16 (out of 0)
 *   see PolyphaseStereo() and PolyphaseMono()
 */

// Each instance owns its complete decoder state; instances are independent.
class MP3Decoder {
  public:
    MP3Decoder() = default;
    ~MP3Decoder() { FreeBuffers(); }

    MP3Decoder(const MP3Decoder &) = delete;
    MP3Decoder &operator=(const MP3Decoder &) = delete;

    bool AllocateBuffers(void);
    bool IsInit() const;
    void FreeBuffers();
    int32_t  MP3Decode( uint8_t *inbuf, int32_t *bytesLeft, int16_t *outbuf, int32_t useSize);
    void     MP3GetLastFrameInfo();
    int32_t  MP3GetNextFrameInfo(uint8_t *buf);
    int32_t  MP3FindSyncWord(uint8_t *buf, int32_t nBytes);
    int32_t  MP3GetSampRate();
    int32_t  MP3GetChannels();
    int32_t  MP3GetBitsPerSample();
    int32_t  MP3GetBitrate();
    int32_t  MP3GetOutputSamps();
    int32_t  MP3GetLayer();
    int32_t  MP3GetVersion();

  private:

    void ClearBuffer(void);
    void PolyphaseMono(int16_t *pcm, int32_t *vbuf, const uint32_t* coefBase);
    void PolyphaseStereo(int16_t *pcm, int32_t *vbuf, const uint32_t* coefBase);
    void SetBitstreamPointer(BitStreamInfo_t *bsi, int32_t nBytes, uint8_t *buf);
    uint32_t GetBits(BitStreamInfo_t *bsi, int32_t nBits);
    int32_t CalcBitsUsed(BitStreamInfo_t *bsi, uint8_t *startBuf, int32_t startOffset);
    int32_t DequantChannel(int32_t *sampleBuf, int32_t *workBuf, int32_t *nonZeroBound, SideInfoSub_t *sis, ScaleFactorInfoSub_t *sfis, CriticalBandInfo_t *cbi);
    void MidSideProc(int32_t x[m_MAX_NCHAN][m_MAX_NSAMP], int32_t nSamps, int32_t mOut[2]);
    void IntensityProcMPEG1(int32_t x[m_MAX_NCHAN][m_MAX_NSAMP], int32_t nSamps, ScaleFactorInfoSub_t *sfis,	CriticalBandInfo_t *cbi, int32_t midSideFlag, int32_t mixFlag, int32_t mOut[2]);
    void IntensityProcMPEG2(int32_t x[m_MAX_NCHAN][m_MAX_NSAMP], int32_t nSamps, ScaleFactorInfoSub_t *sfis, CriticalBandInfo_t *cbi, ScaleFactorJS_t *sfjs, int32_t midSideFlag, int32_t mixFlag, int32_t mOut[2]);
    void FDCT32(int32_t *x, int32_t *d, int32_t offset, int32_t oddBlock, int32_t gb);// __attribute__ ((section (".data")));
    int32_t CheckPadBit();
    int32_t UnpackFrameHeader(uint8_t *buf);
    int32_t UnpackSideInfo(uint8_t *buf);
    int32_t DecodeHuffman( uint8_t *buf, int32_t *bitOffset, int32_t huffBlockBits, int32_t gr, int32_t ch);
    int32_t MP3Dequantize( int32_t gr);
    int32_t IMDCT( int32_t gr, int32_t ch);
    int32_t UnpackScaleFactors( uint8_t *buf, int32_t *bitOffset, int32_t bitsAvail, int32_t gr, int32_t ch);
    int32_t Subband(int16_t *pcmBuf);
    void RefillBitstreamCache(BitStreamInfo_t *bsi);
    void UnpackSFMPEG1(BitStreamInfo_t *bsi, SideInfoSub_t *sis, ScaleFactorInfoSub_t *sfis, int32_t *scfsi, int32_t gr, ScaleFactorInfoSub_t *sfisGr0);
    void UnpackSFMPEG2(BitStreamInfo_t *bsi, SideInfoSub_t *sis, ScaleFactorInfoSub_t *sfis, int32_t gr, int32_t ch, int32_t modeExt, ScaleFactorJS_t *sfjs);
    int32_t MP3FindFreeSync(uint8_t *buf, uint8_t firstFH[4], int32_t nBytes);
    void MP3ClearBadFrame( int16_t *outbuf);
    int32_t DecodeHuffmanPairs(int32_t *xy, int32_t nVals, int32_t tabIdx, int32_t bitsLeft, uint8_t *buf, int32_t bitOffset);
    int32_t DecodeHuffmanQuads(int32_t *vwxy, int32_t nVals, int32_t tabIdx, int32_t bitsLeft, uint8_t *buf, int32_t bitOffset);
    int32_t DequantBlock(int32_t *inbuf, int32_t *outbuf, int32_t num, int32_t scale);
    void AntiAlias(int32_t *x, int32_t nBfly);
    void WinPrevious(int32_t *xPrev, int32_t *xPrevWin, int32_t btPrev);
    int32_t FreqInvertRescale(int32_t *y, int32_t *xPrev, int32_t blockIdx, int32_t es);
    void idct9(int32_t *x);
    int32_t IMDCT36(int32_t *xCurr, int32_t *xPrev, int32_t *y, int32_t btCurr, int32_t btPrev, int32_t blockIdx, int32_t gb);
    void imdct12(int32_t *x, int32_t *out);
    int32_t IMDCT12x3(int32_t *xCurr, int32_t *xPrev, int32_t *y, int32_t btPrev, int32_t blockIdx, int32_t gb);
    int32_t HybridTransform(int32_t *xCurr, int32_t *xPrev, int32_t y[m_BLOCK_SIZE][m_NBANDS], SideInfoSub_t *sis, BlockCount_t *bc);

    MP3FrameInfo_t *m_MP3FrameInfo = nullptr;
    SFBandTable_t m_SFBandTable = {};
    StereoMode_t m_sMode = Stereo;  /* mono/stereo mode */
    MPEGVersion_t m_MPEGVersion = MPEG1;  /* version ID */
    FrameHeader_t *m_FrameHeader = nullptr;
    SideInfoSub_t m_SideInfoSub[m_MAX_NGRAN][m_MAX_NCHAN] = {};
    SideInfo_t *m_SideInfo = nullptr;
    CriticalBandInfo_t m_CriticalBandInfo[m_MAX_NCHAN] = {};  /* filled in dequantizer, used in joint stereo reconstexprruction */
    DequantInfo_t *m_DequantInfo = nullptr;
    HuffmanInfo_t *m_HuffmanInfo = nullptr;
    IMDCTInfo_t *m_IMDCTInfo = nullptr;
    ScaleFactorInfoSub_t m_ScaleFactorInfoSub[m_MAX_NGRAN][m_MAX_NCHAN] = {};
    ScaleFactorJS_t *m_ScaleFactorJS = nullptr;
    SubbandInfo_t *m_SubbandInfo = nullptr;
    MP3DecInfo_t *m_MP3DecInfo = nullptr;
    uint8_t underflowCounter = 0;
};
