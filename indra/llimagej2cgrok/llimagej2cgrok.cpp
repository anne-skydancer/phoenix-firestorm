/**
 * @file llimagej2cgrok.cpp
 * @brief JPEG 2000 encode/decode using Grok.
 */

#include "linden_common.h"

#include "llimagej2cgrok.h"

#include "grok.h"

#include <cerrno>
#include <cstdlib>
#include <limits>
#include <mutex>
#include <thread>
#include <vector>

namespace
{
constexpr U32 GROK_DEFAULT_WORKER_CAP = 6;

void grok_error_callback(const char* msg, void*)
{
    LL_WARNS("Texture") << "Grok: " << (msg ? msg : "unknown error") << LL_ENDL;
}

void grok_warning_callback(const char* msg, void*)
{
    // Progressive texture streams are routinely incomplete. Grok reports those
    // conditions more aggressively than OpenJPEG, so keep them off the hot log path.
    LL_DEBUGS("Texture") << "Grok: " << (msg ? msg : "unknown warning") << LL_ENDL;
}

U32 grok_worker_count()
{
    const U32 hardware = llmax(1u, std::thread::hardware_concurrency());
    U32 workers = llclamp(hardware / 2u, 1u, GROK_DEFAULT_WORKER_CAP);

    if (const char* value = std::getenv("FIRESTORM_GROK_THREADS"))
    {
        errno = 0;
        char* end = nullptr;
        const unsigned long requested = std::strtoul(value, &end, 10);
        if (!errno && end != value && *end == '\0' && requested >= 1 && requested <= 64)
        {
            workers = static_cast<U32>(requested);
        }
        else
        {
            LL_WARNS("Texture") << "Ignoring invalid FIRESTORM_GROK_THREADS='"
                                << value << "' (expected 1..64)" << LL_ENDL;
        }
    }
    return workers;
}

void ensure_grok_initialized()
{
    static std::once_flag once;
    std::call_once(once, []
    {
        grk_msg_handlers handlers = {};
        handlers.error_callback = grok_error_callback;
        handlers.warn_callback = grok_warning_callback;
        grk_set_msg_handlers(handlers);

        const U32 workers = grok_worker_count();
        grk_initialize(nullptr, workers, nullptr);
        LL_INFOS("Texture") << "Initialized Grok " << grk_version()
                            << " with " << workers << " workers" << LL_ENDL;
    });
}

bool valid_image_layout(U32 width, U32 height, U32 components)
{
    return width >= 1 && height >= 1 &&
           width <= static_cast<U32>(MAX_IMAGE_SIZE) &&
           height <= static_cast<U32>(MAX_IMAGE_SIZE) &&
           components >= 1 && components <= static_cast<U32>(MAX_IMAGE_COMPONENTS);
}

U8 sample_to_u8(S32 sample, const grk_image_comp& component)
{
    const U32 precision = llclamp<U32>(component.prec, 1u, 31u);
    S64 value = sample;
    if (component.sgnd)
    {
        value += (S64(1) << (precision - 1));
    }
    if (precision > 8)
    {
        value >>= (precision - 8);
    }
    else if (precision < 8)
    {
        value <<= (8 - precision);
    }
    return static_cast<U8>(llclamp<S64>(value, 0, 255));
}

class GrokDecode
{
public:
    GrokDecode(U8* data, U32 data_size, S32 discard_level)
    {
        mStream.buf = data;
        mStream.buf_len = data_size;
        mStream.is_read_stream = true;
        mParameters.core.reduce = static_cast<U8>(llclamp(discard_level, 0, S32(MAX_DISCARD_LEVEL)));
        // Parallelism belongs between viewer image jobs, not within one SL texture.
        mParameters.num_threads = 1;
        mCodec = grk_decompress_init(&mStream, &mParameters);
    }

    ~GrokDecode()
    {
        if (mCodec)
        {
            grk_object_unref(mCodec);
        }
    }

    bool readHeader()
    {
        return mCodec && grk_decompress_read_header(mCodec, &mHeader);
    }

    bool decode()
    {
        return mCodec && grk_decompress(mCodec, nullptr);
    }

    grk_image* image() const
    {
        return mCodec ? grk_decompress_get_image(mCodec) : nullptr;
    }

    grk_header_info mHeader = {};

private:
    grk_stream_params mStream = {};
    grk_decompress_parameters mParameters = {};
    grk_object* mCodec = nullptr;
};

U32 estimate_num_layers(U32 surface)
{
    if (surface <= 1024) return 2;
    if (surface <= 16384) return 3;
    if (surface <= 262144) return 4;
    if (surface <= 1048576) return 5;
    return 6;
}

void set_layer_rates(grk_cparameters& parameters, U32 num_layers, F32 last_rate)
{
    parameters.numlayers = static_cast<U16>(num_layers);
    parameters.allocation_by_rate_distortion = true;
    for (S32 i = static_cast<S32>(num_layers) - 1; i >= 0; --i)
    {
        parameters.layer_rate[num_layers - 1 - i] =
            last_rate * static_cast<F32>(1u << (i * 2));
    }
}
}

LLImageJ2CImpl* fallbackCreateLLImageJ2CImpl()
{
    return new LLImageJ2CGrok();
}

LLImageJ2CGrok::LLImageJ2CGrok()
{
    ensure_grok_initialized();
}

LLImageJ2CGrok::~LLImageJ2CGrok() = default;

std::string LLImageJ2CGrok::getEngineInfo() const
{
    return llformat("Grok: %s", grk_version());
}

bool LLImageJ2CGrok::initDecode(LLImageJ2C& base, LLImageRaw&, int discard_level, int*)
{
    base.mDiscardLevel = discard_level;
    return false;
}

bool LLImageJ2CGrok::initEncode(LLImageJ2C&, LLImageRaw&, int, int, int)
{
    return false;
}

bool LLImageJ2CGrok::getMetadata(LLImageJ2C& base)
{
    LLImageDataLock lock(&base);
    if (!base.getData() || base.getDataSize() <= 0)
    {
        base.setLastError("Invalid J2C input buffer");
        return false;
    }

    GrokDecode decoder(base.getData(), static_cast<U32>(base.getDataSize()), 0);
    if (!decoder.readHeader())
    {
        base.setLastError("Grok could not read the J2C header");
        return false;
    }

    const grk_image& image = decoder.mHeader.header_image;
    const U32 width = image.x1 >= image.x0 ? image.x1 - image.x0 : 0;
    const U32 height = image.y1 >= image.y0 ? image.y1 - image.y0 : 0;
    if (!valid_image_layout(width, height, image.numcomps))
    {
        base.setLastError("Invalid J2C image dimensions or component count");
        return false;
    }

    base.mDiscardLevel = 0;
    base.setSize(static_cast<S32>(width), static_cast<S32>(height),
                 static_cast<S32>(image.numcomps));
    return true;
}

bool LLImageJ2CGrok::decodeImpl(LLImageJ2C& base, LLImageRaw& raw_image,
                                F32, S32 first_channel, S32 max_channel_count)
{
    LLImageDataLock lock_in(&base);
    LLImageDataLock lock_out(&raw_image);
    LL_PROFILE_ZONE_SCOPED_CATEGORY_TEXTURE;

    const S32 data_size = base.getDataSize();
    const S32 max_bytes = base.getMaxBytes() > 0
                        ? llmin(base.getMaxBytes(), data_size)
                        : data_size;
    if (!base.getData() || max_bytes <= 0)
    {
        base.setLastError("Invalid J2C input buffer");
        base.decodeFailed();
        return true;
    }

    GrokDecode decoder(base.getData(), static_cast<U32>(max_bytes), base.mDiscardLevel);
    if (!decoder.readHeader())
    {
        base.setLastError("Grok could not read the J2C header");
        base.decodeFailed();
        return true;
    }

    const size_t comment_count = llmin(decoder.mHeader.num_comments,
                                       size_t(GRK_NUM_COMMENTS_SUPPORTED));
    for (size_t i = 0; i < comment_count; ++i)
    {
        if (!decoder.mHeader.is_binary_comment[i] && decoder.mHeader.comment[i])
        {
            raw_image.mComment.assign(decoder.mHeader.comment[i],
                                      decoder.mHeader.comment_len[i]);
            break;
        }
    }

    const S32 image_channels = static_cast<S32>(decoder.mHeader.header_image.numcomps);
    if (first_channel < 0 || first_channel >= image_channels || max_channel_count <= 0)
    {
        base.setLastError("Invalid J2C channel selection");
        base.decodeFailed();
        return true;
    }
    const S32 channels = llmin(image_channels - first_channel, max_channel_count);

    if (!decoder.decode())
    {
        if (raw_image.getComponents() != channels)
        {
            raw_image.resize(raw_image.getWidth(), raw_image.getHeight(), S8(channels));
        }
        base.setLastError("Grok failed to decode the J2C stream");
        base.decodeFailed();
        return true;
    }

    grk_image* image = decoder.image();
    if (!image || image->numcomps < first_channel + channels || !image->comps)
    {
        base.setLastError("Grok returned an invalid decoded image");
        base.decodeFailed();
        return true;
    }

    const U32 width = image->comps[0].w;
    const U32 height = image->comps[0].h;
    if (!valid_image_layout(width, height, channels))
    {
        base.setLastError("Grok returned invalid decoded dimensions");
        base.decodeFailed();
        return true;
    }

    for (S32 comp = first_channel; comp < first_channel + channels; ++comp)
    {
        const grk_image_comp& component = image->comps[comp];
        if (!component.data || component.w != width || component.h != height ||
            component.stride < width ||
            (component.data_type != GRK_INT_16 && component.data_type != GRK_INT_32))
        {
            base.setLastError("Unsupported Grok component layout");
            base.decodeFailed();
            return true;
        }
    }

    raw_image.resize(U16(width), U16(height), S8(channels));
    U8* output = raw_image.getData();
    if (!output)
    {
        base.setLastError("Memory error");
        base.decodeFailed();
        return true;
    }

    for (S32 comp = first_channel, dest = 0;
         comp < first_channel + channels; ++comp, ++dest)
    {
        const grk_image_comp& component = image->comps[comp];
        size_t output_offset = static_cast<size_t>(dest);
        for (S32 y = static_cast<S32>(height) - 1; y >= 0; --y)
        {
            const size_t row = static_cast<size_t>(y) * component.stride;
            for (U32 x = 0; x < width; ++x)
            {
                const S32 sample = component.data_type == GRK_INT_16
                    ? static_cast<const int16_t*>(component.data)[row + x]
                    : static_cast<const int32_t*>(component.data)[row + x];
                output[output_offset] = sample_to_u8(sample, component);
                output_offset += static_cast<size_t>(channels);
            }
        }
    }

    base.setDiscardLevel(llclamp(base.mDiscardLevel, 0, S32(MAX_DISCARD_LEVEL)));
    return true;
}

bool LLImageJ2CGrok::encodeImpl(LLImageJ2C& base, const LLImageRaw& raw_image,
                                const char* comment_text, F32, bool reversible)
{
    if (raw_image.isBufferInvalid())
    {
        base.setLastError("Invalid input, no buffer");
        return false;
    }

    LLImageDataSharedLock lock_in(&raw_image);
    LLImageDataLock lock_out(&base);

    const S32 components = raw_image.getComponents();
    const S32 width = raw_image.getWidth();
    const S32 height = raw_image.getHeight();
    if (!valid_image_layout(width, height, components))
    {
        base.setLastError("Invalid raw image dimensions or component count");
        return false;
    }

    grk_cparameters parameters = {};
    grk_compress_set_default_params(&parameters);
    parameters.cod_format = GRK_FMT_J2K;
    parameters.prog_order = GRK_RLCP;
    parameters.irreversible = !reversible;
    parameters.mct = components >= 3;

    if (reversible)
    {
        parameters.numlayers = 1;
        parameters.allocation_by_rate_distortion = true;
        parameters.layer_rate[0] = 0.0;
    }
    else
    {
        const U32 surface = static_cast<U32>(width) * static_cast<U32>(height);
        set_layer_rates(parameters, estimate_num_layers(surface),
                        1.f / DEFAULT_COMPRESSION_RATE);
    }

    const S32 min_dimension = llmin(width, height);
    if (min_dimension < 64)
    {
        parameters.numresolution = static_cast<U8>(1 +
            static_cast<S32>(floor(log2(static_cast<double>(min_dimension)))));
    }

    if (comment_text && *comment_text)
    {
        const size_t length = llmin(strlen(comment_text), size_t(std::numeric_limits<U16>::max()));
        parameters.comment[0] = const_cast<char*>(comment_text);
        parameters.comment_len[0] = static_cast<U16>(length);
        parameters.is_binary_comment[0] = false;
        parameters.num_comments = 1;
    }

    std::vector<grk_image_comp> component_parameters(static_cast<size_t>(components));
    for (grk_image_comp& component : component_parameters)
    {
        component.prec = 8;
        component.sgnd = false;
        component.dx = 1;
        component.dy = 1;
        component.w = static_cast<U32>(width);
        component.h = static_cast<U32>(height);
    }

    const GRK_COLOR_SPACE color_space = components >= 3 ? GRK_CLRSPC_SRGB : GRK_CLRSPC_GRAY;
    grk_image* image = grk_image_new(static_cast<U16>(components),
                                     component_parameters.data(), color_space, true);
    if (!image)
    {
        base.setLastError("Could not allocate Grok image");
        return false;
    }
    image->x1 = static_cast<U32>(width);
    image->y1 = static_cast<U32>(height);

    const U8* source = raw_image.getData();
    bool valid_components = true;
    for (S32 channel = 0; channel < components && valid_components; ++channel)
    {
        grk_image_comp& component = image->comps[channel];
        if (!component.data || component.stride < static_cast<U32>(width) ||
            (component.data_type != GRK_INT_16 && component.data_type != GRK_INT_32))
        {
            valid_components = false;
            break;
        }

        U32 destination_row = 0;
        for (S32 y = height - 1; y >= 0; --y, ++destination_row)
        {
            const U8* pixel = source +
                (static_cast<size_t>(y) * width * components) + channel;
            for (S32 x = 0; x < width; ++x, pixel += components)
            {
                const size_t offset = static_cast<size_t>(destination_row) * component.stride + x;
                if (component.data_type == GRK_INT_16)
                {
                    static_cast<int16_t*>(component.data)[offset] = *pixel;
                }
                else
                {
                    static_cast<int32_t*>(component.data)[offset] = *pixel;
                }
            }
        }
    }

    if (!valid_components)
    {
        grk_object_unref(&image->obj);
        base.setLastError("Unsupported Grok encoder component layout");
        return false;
    }

    const size_t raw_size = static_cast<size_t>(width) * height * components;
    if (raw_size > static_cast<size_t>(std::numeric_limits<S32>::max()) - 65536)
    {
        grk_object_unref(&image->obj);
        base.setLastError("Encoded image is too large");
        return false;
    }
    const size_t capacity = raw_size + 65536;
    U8* buffer = static_cast<U8*>(ll_aligned_malloc_16(capacity));
    if (!buffer)
    {
        grk_object_unref(&image->obj);
        base.setLastError("Memory error");
        return false;
    }

    grk_stream_params stream = {};
    stream.buf = buffer;
    stream.buf_len = capacity;
    stream.is_read_stream = false;
    grk_object* encoder = grk_compress_init(&stream, &parameters, image);
    bool encoded = false;
    if (encoder)
    {
        const uint64_t written = grk_compress(encoder, nullptr);
        if (written > 0 && written <= capacity &&
            written <= static_cast<uint64_t>(std::numeric_limits<S32>::max()))
        {
            U8* destination = base.allocateData(static_cast<S32>(written));
            if (destination)
            {
                memcpy(destination, buffer, static_cast<size_t>(written));
                base.updateData();
                encoded = true;
            }
        }
        grk_object_unref(encoder);
    }

    ll_aligned_free_16(buffer);
    grk_object_unref(&image->obj);
    if (!encoded)
    {
        base.setLastError("Grok encoding was unsuccessful");
    }
    return encoded;
}
