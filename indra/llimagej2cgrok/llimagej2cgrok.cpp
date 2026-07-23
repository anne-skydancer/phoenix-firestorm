/**
 * @file llimagej2cgrok.cpp
 * @brief This is an implementation of JPEG2000 encode/decode using Grok.
 *
 * Grok (GrokImageCompression/grok) is licensed under the GNU Affero
 * General Public License version 3. This implementation is only safe
 * to bundle into a build that does not also link Kakadu, FMOD, or
 * Vivox in the same binary; see indra/cmake/Grok.cmake.
 *
 * $LicenseInfo:firstyear=2006&license=viewerlgpl$
 * Second Life Viewer Source Code
 * Copyright (C) 2010, Linden Research, Inc.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation;
 * version 2.1 of the License only.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 *
 * Linden Research, Inc., 945 Battery Street, San Francisco, CA  94111  USA
 * $/LicenseInfo$
 */

#include "linden_common.h"
#include "llimagej2cgrok.h"

#include "grok.h"

// Factory function: see declaration in llimagej2c.cpp
LLImageJ2CImpl* fallbackCreateLLImageJ2CImpl()
{
    return new LLImageJ2CGrok();
}

// grk_initialize() must be called once before any other Grok API call.
// The library documents it as safe to call multiple times and, per its
// own change history ("grk_initialize: make thread safe and optional
// for API"), safe to call concurrently, so a simple static-local guard
// is sufficient here without an explicit mutex.
static void ensure_grok_initialized()
{
    static bool s_initialized = false;
    if (!s_initialized)
    {
        // Second arg is grok's GLOBAL taskflow worker-pool size; 0 means
        // "one worker per hardware thread". Those workers busy-wait on
        // sched_yield (taskflow's NonblockingNotifier) whenever idle, so a
        // full-width pool pins every core. The viewer already decodes many
        // textures concurrently on its own LLImageDecodeThread pool, and each
        // of those threads calls into this one global grok pool -- the product
        // is massive thread oversubscription that starves the main render
        // thread (multi-second frame spikes while moving, confirmed by DTrace
        // on-CPU profiling). A single global worker fixed the starvation but
        // throttled J2C decode throughput (slow texture loading), so use a
        // bounded pool of 6: enough decode parallelism to keep textures
        // flowing, still well under the 12-thread all-core saturation.
        grk_initialize(nullptr, 6, nullptr);
        s_initialized = true;
    }
}

static void grok_error_callback(const char* msg, void*)
{
    LL_WARNS() << "LLImageJ2CGrok: " << msg << LL_ENDL;
}

static void grok_warning_callback(const char* msg, void*)
{
    // Grok is stricter than KDU/OpenJPEG about partial streams and emits a
    // warning for every truncated/progressive packet during texture streaming.
    // These are benign but, at LL_WARNS, flooded the log and added locked I/O on
    // the image-decode path (visible as render stutter). Gate behind a debug tag.
    LL_DEBUGS("Texture") << "LLImageJ2CGrok: " << msg << LL_ENDL;
}

std::string LLImageJ2CGrok::getEngineInfo() const
{
    return llformat("Grok: %s", grk_version());
}

// Wraps a grk_object* decompressor so header-only reads and full decodes
// share the same setup and are guaranteed to release the codec exactly
// once regardless of which failure path is taken.
class GrokDecode
{
public:
    GrokDecode(U8* data, U32 data_size, S32 discard_level)
    {
        stream_params.buf = data;
        stream_params.buf_len = data_size;
        stream_params.is_read_stream = true;

        params.core.reduce = static_cast<uint8_t>(llclamp(discard_level, 0, (S32)MAX_DISCARD_LEVEL));
        // Let a decode use up to the full 6-worker global pool (see
        // ensure_grok_initialized()); the global pool size caps total grok
        // threads, so concurrent decodes share those 6 rather than each
        // spawning its own and oversubscribing the cores.
        params.num_threads = 6;

        grk_msg_handlers handlers = {};
        handlers.error_callback = grok_error_callback;
        handlers.warn_callback = grok_warning_callback;
        grk_set_msg_handlers(handlers);

        codec = grk_decompress_init(&stream_params, &params);
    }

    ~GrokDecode()
    {
        if (codec)
        {
            grk_object_unref(codec);
        }
        codec = nullptr;
    }

    bool readHeader()
    {
        if (!codec)
        {
            return false;
        }
        memset(&header_info, 0, sizeof(header_info));
        return grk_decompress_read_header(codec, &header_info);
    }

    bool decode()
    {
        if (!codec)
        {
            return false;
        }
        // Synchronous mode (params.asynchronous left false by zero-init):
        // grk_decompress() blocks until every requested tile is done.
        return grk_decompress(codec, nullptr);
    }

    grk_image* getImage()
    {
        return codec ? grk_decompress_get_image(codec) : nullptr;
    }

    grk_header_info header_info = {};

private:
    grk_stream_params stream_params = {};
    grk_decompress_parameters params = {};
    grk_object* codec = nullptr;
};

LLImageJ2CGrok::LLImageJ2CGrok()
    : LLImageJ2CImpl()
{
    ensure_grok_initialized();
}

LLImageJ2CGrok::~LLImageJ2CGrok()
{
}

bool LLImageJ2CGrok::initDecode(LLImageJ2C &base, LLImageRaw &raw_image, int discard_level, int* region)
{
    base.mDiscardLevel = discard_level;
    return false;
}

bool LLImageJ2CGrok::initEncode(LLImageJ2C &base, LLImageRaw &raw_image, int blocks_size, int precincts_size, int levels)
{
    // No specific implementation for this method in the Grok case.
    return false;
}

bool LLImageJ2CGrok::getMetadata(LLImageJ2C &base)
{
    LLImageDataLock lock(&base);

    GrokDecode decoder(base.getData(), base.getDataSize(), 0);
    if (!decoder.readHeader())
    {
        return false;
    }

    // SL textures are essentially always single-tile, so the tile-grid
    // based discard estimate that OpenJPEG's readHeader performs
    // resolves to 0 for the overwhelming majority of real assets; start
    // from 0 here rather than reimplement that heuristic.
    base.mDiscardLevel = 0;
    base.setSize(static_cast<S32>(decoder.header_info.header_image.x1),
                 static_cast<S32>(decoder.header_info.header_image.y1),
                 static_cast<S32>(decoder.header_info.header_image.numcomps));
    return true;
}

bool LLImageJ2CGrok::decodeImpl(LLImageJ2C &base, LLImageRaw &raw_image, F32 decode_time, S32 first_channel, S32 max_channel_count)
{
    LLImageDataLock lockIn(&base);
    LLImageDataLock lockOut(&raw_image);

    LL_PROFILE_ZONE_SCOPED_CATEGORY_TEXTURE;

    S32 max_bytes = base.getMaxBytes() ? base.getMaxBytes() : base.getDataSize();

    GrokDecode decoder(base.getData(), static_cast<U32>(max_bytes), base.mDiscardLevel);
    if (!decoder.readHeader())
    {
        base.decodeFailed();
        return true; // done
    }

    // Comments are already parsed out of the codestream by Grok, so the
    // manual CME-marker byte scan the OpenJPEG path needs is unnecessary
    // here. Copy the first textual comment before the codec is released,
    // since header_info's comment pointers are only valid until then.
    for (size_t i = 0; i < decoder.header_info.num_comments; ++i)
    {
        if (!decoder.header_info.is_binary_comment[i] && decoder.header_info.comment[i])
        {
            raw_image.mComment.assign(decoder.header_info.comment[i], decoder.header_info.comment_len[i]);
            break;
        }
    }

    U32 image_channels = static_cast<U32>(decoder.header_info.header_image.numcomps);
    S32 channels = static_cast<S32>(image_channels) - first_channel;
    channels = llmin(channels, max_channel_count);

    bool decoded = decoder.decode();
    if (!decoded)
    {
        if (raw_image.getComponents() != channels)
        {
            raw_image.resize(raw_image.getWidth(), raw_image.getHeight(), S8(channels));
        }
        LL_DEBUGS("Texture") << "ERROR -> decodeImpl: failed to decode image!" << LL_ENDL;
        base.decodeFailed();
        return true; // done
    }

    grk_image* image = decoder.getImage();
    if (!image || !image->numcomps)
    {
        base.decodeFailed();
        return true; // done
    }

    // Case 2 of the coordinate-frame contract documented on grk_image:
    // for a full-image decompress via grk_decompress_get_image(), the
    // image bounds and each component's (x0,y0,w,h) frame agree, so the
    // component's own w/h/stride describe the decoded rectangle directly.
    U32 width = image->comps[0].w;
    U32 height = image->comps[0].h;

    raw_image.resize(U16(width), U16(height), S8(channels));
    U8* rawp = raw_image.getData();
    if (!rawp)
    {
        base.setLastError("Memory error");
        base.decodeFailed();
        return true; // done
    }

    for (S32 comp = first_channel, dest = 0; comp < first_channel + channels; comp++, dest++)
    {
        grk_image_comp& c = image->comps[comp];
        if (!c.data)
        {
            LL_DEBUGS("Texture") << "ERROR -> decodeImpl: missing component data" << LL_ENDL;
            base.decodeFailed();
            continue;
        }

        U32 stride = c.stride;
        S32 offset = dest;

        // grk_get_data_type() explains that Grok stores decoded samples
        // as either 32-bit or 16-bit signed integers depending on the
        // codestream; read the component's own data_type rather than
        // assume one so both cases decode correctly.
        if (c.data_type == GRK_INT_16)
        {
            const int16_t* src = static_cast<const int16_t*>(c.data);
            for (S32 y = static_cast<S32>(height) - 1; y >= 0; y--)
            {
                for (U32 x = 0; x < width; x++)
                {
                    rawp[offset] = static_cast<U8>(src[static_cast<size_t>(y) * stride + x]);
                    offset += channels;
                }
            }
        }
        else
        {
            const int32_t* src = static_cast<const int32_t*>(c.data);
            for (S32 y = static_cast<S32>(height) - 1; y >= 0; y--)
            {
                for (U32 x = 0; x < width; x++)
                {
                    rawp[offset] = static_cast<U8>(src[static_cast<size_t>(y) * stride + x]);
                    offset += channels;
                }
            }
        }
    }

    // NOTE: Grok clamps the reduce factor it can honour to the smallest
    // number of decomposition levels present across all tiles, and does
    // not report the achieved factor back through this call path the
    // way OpenJPEG's image->comps[0].factor does. Reporting the
    // requested level here is correct for the overwhelming majority of
    // real assets, since SL textures carry enough decomposition levels
    // for the 0-5 discard range in normal use; this is a known
    // simplification worth revisiting if mismatched-discard artefacts
    // are ever observed in practice.
    base.setDiscardLevel(base.mDiscardLevel);

    return true; // done
}

// Estimates the number of quality layers necessary depending on the
// image surface (w x h). Ported unchanged from the OpenJPEG wrapper's
// estimate_num_layers, since the underlying heuristic is about image
// size rather than anything specific to either codec.
static U32 grok_estimate_num_layers(U32 surface)
{
    if      (surface <= 1024)    return 2;  // Tiny (<=32x32)
    else if (surface <= 16384)   return 3;  // Small (<=128x128)
    else if (surface <= 262144)  return 4;  // Medium (<=512x512)
    else if (surface <= 1048576) return 5;  // Up to ~1MP
    else                         return 6;  // Up to ~1.5-2MP
}

// Fills parameters.layer_rate according to the number of layers and a
// last-layer compression ratio, mirroring the OpenJPEG wrapper's
// set_tcp_rates. NOTE: this has not yet been verified empirically
// against a real round-trip encode/decode; the numeric convention for
// layer_rate is carried over from OpenJPEG's tcp_rates on the
// assumption the two remain compatible, since Grok is a fork of
// OpenJPEG's encoder, but this assumption should be confirmed with a
// dedicated encode test before being relied upon.
static void grok_set_layer_rates(grk_cparameters* parameters, U32 num_layers, F32 last_rate)
{
    parameters->numlayers = static_cast<uint16_t>(num_layers);
    parameters->allocation_by_rate_distortion = true;

    for (S32 i = static_cast<S32>(num_layers) - 1; i >= 0; i--)
    {
        parameters->layer_rate[num_layers - 1 - i] = last_rate * static_cast<F32>(1 << (i << 1));
    }
}

bool LLImageJ2CGrok::encodeImpl(LLImageJ2C &base, const LLImageRaw &raw_image, const char* comment_text, F32 encode_time, bool reversible)
{
    if (raw_image.isBufferInvalid())
    {
        base.setLastError("Invalid input, no buffer");
        return false;
    }

    LLImageDataSharedLock lockIn(&raw_image);
    LLImageDataLock lockOut(&base);

    S32 numcomps = raw_image.getComponents();
    S32 width    = raw_image.getWidth();
    S32 height   = raw_image.getHeight();

    grk_msg_handlers handlers = {};
    handlers.error_callback = grok_error_callback;
    handlers.warn_callback = grok_warning_callback;
    grk_set_msg_handlers(handlers);

    grk_cparameters parameters = {};
    grk_compress_set_default_params(&parameters);
    parameters.cod_format = GRK_FMT_J2K;
    parameters.prog_order = GRK_RLCP;
    parameters.irreversible = reversible ? 0 : 1;
    parameters.mct = (numcomps >= 3) ? 1 : 0;

    if (!reversible)
    {
        U32 surface = static_cast<U32>(width) * static_cast<U32>(height);
        U32 nb_layers = grok_estimate_num_layers(surface);
        grok_set_layer_rates(&parameters, nb_layers, 1.f / DEFAULT_COMPRESSION_RATE);
    }
    else
    {
        parameters.numlayers = 1;
        parameters.allocation_by_rate_distortion = true;
        parameters.layer_rate[0] = 0.0f;
    }

    // Same small-image resolution-count adjustment as the OpenJPEG
    // wrapper: images with either dimension under 64 need fewer DWT
    // resolution levels or Grok will refuse them, matching the identical
    // constraint in OpenJPEG since both implement the same standard.
    U32 width_tiles = static_cast<U32>(width) >> 6;
    U32 height_tiles = static_cast<U32>(height) >> 6;
    if (width_tiles == 0 && width >= MIN_IMAGE_SIZE) width_tiles = 1;
    if (height_tiles == 0 && height >= MIN_IMAGE_SIZE) height_tiles = 1;
    if (width_tiles == 1 || height_tiles == 1)
    {
        S32 min_dim = width < height ? width : height;
        parameters.numresolution = static_cast<uint8_t>(1 + static_cast<S32>(floor(log2(static_cast<double>(min_dim)))));
    }

    if (comment_text)
    {
        parameters.comment[0] = const_cast<char*>(comment_text);
        parameters.comment_len[0] = static_cast<uint16_t>(strlen(comment_text));
        parameters.is_binary_comment[0] = false;
        parameters.num_comments = 1;
    }

    std::vector<grk_image_comp> cmptparm(numcomps);
    for (S32 c = 0; c < numcomps; c++)
    {
        cmptparm[c].prec = 8;
        cmptparm[c].sgnd = false;
        cmptparm[c].dx = 1;
        cmptparm[c].dy = 1;
        cmptparm[c].w = static_cast<uint32_t>(width);
        cmptparm[c].h = static_cast<uint32_t>(height);
    }

    grk_image* image = grk_image_new(static_cast<uint16_t>(numcomps), cmptparm.data(), GRK_CLRSPC_SRGB, true);
    if (!image)
    {
        base.setLastError("Could not allocate Grok image");
        return false;
    }
    image->x1 = static_cast<uint32_t>(width);
    image->y1 = static_cast<uint32_t>(height);

    // comp.stride is the row pitch in samples and may exceed comp.w if
    // Grok pads rows for alignment, so the destination offset for row
    // dst_row, column x must be dst_row * stride + x rather than a flat
    // pixel counter divided by stride, which would silently misalign
    // every row once padding is present.
    const U8* src_datap = raw_image.getData();
    for (S32 c = 0; c < numcomps; c++)
    {
        grk_image_comp& comp = image->comps[c];
        U32 stride = comp.stride;
        S32 dst_row = 0;
        if (comp.data_type == GRK_INT_16)
        {
            int16_t* dst = static_cast<int16_t*>(comp.data);
            for (S32 y = height - 1; y >= 0; y--)
            {
                const U8* pixel = src_datap + (static_cast<size_t>(y) * width) * numcomps + c;
                for (S32 x = 0; x < width; x++)
                {
                    dst[static_cast<size_t>(dst_row) * stride + x] = static_cast<int16_t>(*pixel);
                    pixel += numcomps;
                }
                dst_row++;
            }
        }
        else
        {
            int32_t* dst = static_cast<int32_t*>(comp.data);
            for (S32 y = height - 1; y >= 0; y--)
            {
                const U8* pixel = src_datap + (static_cast<size_t>(y) * width) * numcomps + c;
                for (S32 x = 0; x < width; x++)
                {
                    dst[static_cast<size_t>(dst_row) * stride + x] = static_cast<int32_t>(*pixel);
                    pixel += numcomps;
                }
                dst_row++;
            }
        }
    }

    // Buffer-mode streaming needs a pre-sized destination, unlike the
    // growable-buffer callback approach OpenJPEG's wrapper needs; a
    // compressed J2K stream is essentially always smaller than the
    // uncompressed source, so allocating the raw size plus a fixed
    // safety margin is sufficient here.
    size_t out_capacity = static_cast<size_t>(width) * static_cast<size_t>(height) *
                          static_cast<size_t>(numcomps) + (64 * 1024);
    U8* out_buffer = static_cast<U8*>(ll_aligned_malloc_16(out_capacity));
    if (!out_buffer)
    {
        grk_object_unref(&image->obj);
        base.setLastError("Memory error");
        return false;
    }

    grk_stream_params out_stream = {};
    out_stream.buf = out_buffer;
    out_stream.buf_len = out_capacity;
    out_stream.is_read_stream = false;

    grk_object* encoder = grk_compress_init(&out_stream, &parameters, image);
    bool encoded = false;
    if (encoder)
    {
        uint64_t written = grk_compress(encoder, nullptr);
        if (written > 0)
        {
            base.allocateData(static_cast<S32>(written));
            memcpy(base.getData(), out_buffer, written);
            base.updateData();
            encoded = true;
        }
        grk_object_unref(encoder);
    }

    ll_aligned_free_16(out_buffer);
    grk_object_unref(&image->obj);

    if (!encoded)
    {
        LL_WARNS() << "Grok encoding was unsuccessful, returning false" << LL_ENDL;
    }
    return encoded;
}
