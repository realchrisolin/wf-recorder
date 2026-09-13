/*
 * Smoke test for FFmpeg 7.1+ dual-path pixfmt lookup
 * (avcodec_get_supported_config vs legacy AVCodec.pix_fmts).
 */

#include <iostream>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/pixfmt.h>
}

#define HAVE_AVCODEC_GET_SUPPORTED_CONFIG \
    (LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(61, 13, 100))

static const AVPixelFormat *get_codec_pix_fmts(const AVCodec *codec)
{
#if HAVE_AVCODEC_GET_SUPPORTED_CONFIG
    const AVPixelFormat *fmts = nullptr;
    avcodec_get_supported_config(nullptr, codec, AV_CODEC_CONFIG_PIX_FORMAT, 0,
        (const void **)&fmts, nullptr);
    return fmts;
#else
    return codec->pix_fmts;
#endif
}

int main()
{
    const char *candidates[] = {
        "libx264",
        "libx265",
        "mpeg4",
        "libvpx",
        "h264_vaapi",
        nullptr,
    };

    const AVCodec *codec = nullptr;
    const char *used = nullptr;
    for (int i = 0; candidates[i]; ++i) {
        codec = avcodec_find_encoder_by_name(candidates[i]);
        if (codec) {
            used = candidates[i];
            break;
        }
    }

    if (!codec) {
        std::cerr << "ffmpeg-pixfmt-test: skip (no common encoder found)\n";
        return 77; /* meson skip */
    }

    const AVPixelFormat *fmts = get_codec_pix_fmts(codec);
    if (!fmts) {
        std::cerr << "ffmpeg-pixfmt-test: FAIL no pixfmts for " << used
                  << " (HAVE_AVCODEC_GET_SUPPORTED_CONFIG="
                  << HAVE_AVCODEC_GET_SUPPORTED_CONFIG << ")\n";
        return 1;
    }

    int count = 0;
    for (const AVPixelFormat *p = fmts; *p != AV_PIX_FMT_NONE; ++p) {
        ++count;
    }
    if (count <= 0) {
        std::cerr << "ffmpeg-pixfmt-test: FAIL empty pixfmt list for " << used << "\n";
        return 1;
    }

    std::cout << "ffmpeg-pixfmt-test: ok encoder=" << used
              << " pixfmts=" << count
              << " api="
              << (HAVE_AVCODEC_GET_SUPPORTED_CONFIG ? "get_supported_config" : "legacy")
              << "\n";
    return 0;
}
