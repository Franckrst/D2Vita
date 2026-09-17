/* glide_ring.h — Glide RING BUFFER wire format, shared between:
 *   * the GUEST side: our glide3x.dll x86 (MinGW), translated by the dynarec
 *     like the rest of the game, so with NO host crossing;
 *   * the HOST side: the native reader (tools/rt_boot.cpp), which replays the
 *     ring into sceGxm (Vita) or into a counting sink (qemu).
 *
 * WHY. D2's Glide path issues on the order of a thousand calls per frame
 * (draws, vertex pushes, texture uploads). Each guest->host crossing costs
 * several microseconds on console, so crossing for every one of those calls
 * would cost multiple milliseconds per frame before anything is even drawn.
 * The ring collapses this to THREE crossing points:
 *      1 x d2vGlideInit      (once for the whole session)
 *      1 x d2vGlideFlush     (once per frame, from grBufferSwap)
 *      N x d2vGlideTexUpload (N = actual texture uploads, a handful per frame)
 *
 * CONVENTIONS.
 *  - Everything is 32-bit words, 4-byte aligned.
 *  - A record starts with a header word:
 *        (op << 24) | (total_length_in_words & 0x00FFFFFF)
 *    Length INCLUDES the header word. A reader that doesn't recognize an op
 *    can therefore always skip the record: the format is forward-extensible
 *    without breaking older readers.
 *  - `head` is written ONLY by the guest, `tail` ONLY by the host. Both are
 *    MONOTONIC byte counters (mod 2^32): head-tail gives occupancy without
 *    full/empty ambiguity, and since there's no shared write there's no lock.
 *  - The guest writes data BEFORE publishing `head`; the host reads `head`
 *    BEFORE reading data. On Vita this needs a memory barrier on both sides
 *    (the two threads run on different cores).
 */
#ifndef D2VITA_GLIDE_RING_H
#define D2VITA_GLIDE_RING_H

#include <stdint.h>

#define D2GR_MAGIC    0x52473244u   /* 'D2GR' little-endian */
#define D2GR_VERSION  1u

/* Ring size. 2 MiB covers about 10 frames of typical traffic (~200 KB/frame:
 * 3900 verts x 44 bytes = 170 KB + ~1000 state changes). */
#define D2GR_RING_BYTES  (2u << 20)
#define D2GR_RING_MASK   (D2GR_RING_BYTES - 1u)

typedef struct {
    uint32_t magic;        /* D2GR_MAGIC */
    uint32_t version;      /* D2GR_VERSION */
    uint32_t size;         /* D2GR_RING_BYTES */
    uint32_t data_off;     /* data offset from the start of the header */
    volatile uint32_t head;/* written by the GUEST: bytes produced (monotonic) */
    volatile uint32_t tail;/* written by the HOST: bytes consumed (monotonic) */
    volatile uint32_t frame;
    volatile uint32_t stalls;   /* times the guest had to wait on the host */
    volatile uint32_t dropped;  /* records lost (ring full, non-blocking mode) */
    volatile uint32_t dedup_evites; /* written by the GUEST on every grBufferSwap: states
                                     * NOT written because identical to the last one
                                     * recorded (D2_GRDEDUP); diagnostic only */
    uint32_t reserved[6];
} D2GRHeader;

/* ---- Opcodes ------------------------------------------------------------- *
 * Rule: one op per Glide call that changes STATE or issues a DRAW. Calls with
 * no observable effect on the GPU side (grFinish, a no-op grDitherMode) are
 * still recorded: the guest decides, keeping a 1:1 record<->call mapping.   */
enum {
    D2GR_OP_NOP                 = 0x00,
    /* --- state --- */
    D2GR_OP_TEXSOURCE           = 0x10, /* tmu, handle, startAddr, evenOdd, lod, aspect, fmt */
    D2GR_OP_TEXCOMBINE          = 0x11, /* tmu, rgb_func, rgb_fact, alpha_func, alpha_fact, rgb_inv, alpha_inv */
    D2GR_OP_TEXFILTERMODE       = 0x12, /* tmu, minf, magf */
    D2GR_OP_TEXMIPMAPMODE       = 0x13, /* tmu, mode, lodBlend */
    D2GR_OP_ALPHABLENDFUNCTION  = 0x14, /* rgb_sf, rgb_df, alpha_sf, alpha_df */
    D2GR_OP_ALPHACOMBINE        = 0x15, /* func, factor, local, other, invert */
    D2GR_OP_COLORCOMBINE        = 0x16, /* func, factor, local, other, invert */
    D2GR_OP_CONSTANTCOLORVALUE  = 0x17, /* argb */
    D2GR_OP_CHROMAKEYMODE       = 0x18, /* mode */
    D2GR_OP_CHROMAKEYVALUE      = 0x19, /* value */
    D2GR_OP_CLIPWINDOW          = 0x1a, /* minx, miny, maxx, maxy */
    D2GR_OP_DITHERMODE          = 0x1b, /* mode */
    D2GR_OP_COLORMASK           = 0x1c, /* rgb, a */
    D2GR_OP_DEPTHMASK           = 0x1d, /* mask */
    D2GR_OP_DEPTHBUFFERMODE     = 0x1e, /* mode */
    D2GR_OP_DEPTHBUFFERFUNCTION = 0x1f, /* func */
    D2GR_OP_VERTEXLAYOUT        = 0x20, /* param, offset, mode */
    D2GR_OP_ENABLE              = 0x21, /* mode */
    D2GR_OP_DISABLE             = 0x22, /* mode */
    D2GR_OP_COORDINATESPACE     = 0x23, /* mode */
    D2GR_OP_TEXDOWNLOADTABLE    = 0x24, /* type, then 256 palette words */
    D2GR_OP_WINOPEN             = 0x25, /* res, refresh, cformat, origin, nColBuf, nAuxBuf */
                                        /* RESOLUTION and color format are state:
                                         * without them the reader doesn't know which window to
                                         * project vertices onto, nor how to read
                                         * grConstantColorValue (RGBA vs ARGB). An old reader
                                         * skips the record (length rule). */
    /* --- drawing --- */
    D2GR_OP_BUFFERCLEAR         = 0x30, /* color, alpha, depth */
    D2GR_OP_DRAWVERTEXARRAY     = 0x31, /* mode, count, stride, then count*stride/4 words */
    D2GR_OP_DRAWVERTEXARRAYCONT = 0x32, /* mode, count, stride, then count*stride/4 words */
    D2GR_OP_DRAWTRIANGLE        = 0x33, /* stride, then 3*stride/4 words */
    D2GR_OP_DRAWLINE            = 0x34, /* stride, then 2*stride/4 words */
    D2GR_OP_DRAWPOINT           = 0x35, /* stride, then stride/4 words */
    D2GR_OP_TEXBIND             = 0x36, /* EMITTED BY THE HOST on upload: tmuAddr, atlas cell, generation — keeps the binding in stream order */
    D2GR_OP_TEXUP               = 0x37, /* EMITTED BY THE HOST (D2_FLUSHFIL=1): tmuAddr, w, h, offset
                                         * into the host staging buffer, bytes. The actual upload
                                         * (hash + copy into the atlas + bind) happens on the FLUSH
                                         * THREAD when it reaches this record — the same place in the
                                         * stream as the synchronous path, where the host would upload
                                         * at crossing time and emit a TEXBIND here.
                                         * Texture BYTES are copied by the GAME thread (guest memory
                                         * pointed to by grTexDownloadMipMap is reusable as soon as it
                                         * returns). */
    /* --- markers --- */
    D2GR_OP_FRAME_END           = 0x3f  /* frame number */
};

#define D2GR_HDR(op,words)   (((uint32_t)(op) << 24) | ((uint32_t)(words) & 0x00FFFFFFu))
#define D2GR_OPOF(w)         ((uint32_t)(w) >> 24)
#define D2GR_LENOF(w)        ((uint32_t)(w) & 0x00FFFFFFu)

/* Glide3 vertex as D2 declares it (grVertexLayout). Not assumed here: stride
 * is recorded with every draw, and layout is published via
 * D2GR_OP_VERTEXLAYOUT. This constant is only used by the diagnostic reader. */
#define D2GR_MAX_STRIDE 256

#endif /* D2VITA_GLIDE_RING_H */
