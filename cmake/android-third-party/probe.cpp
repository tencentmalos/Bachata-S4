// SPDX-License-Identifier: GPL-2.0-or-later
#include <cstdio>
#include <cstring>
#include <fmt/format.h>
#include <ft2build.h>
#include <png.h>
#include <spdlog/logger.h>
#include <zlib.h>
#include FT_FREETYPE_H
#include <AL/al.h>
#include <AL/alc.h>
#include <SDL3/SDL.h>
#include <aacdecoder_lib.h>
#include <google/protobuf/any.pb.h>
#include <httplib.h>
#include <hwinfo/os.h>
#include <libusb.h>
#include <sirit/sirit.h>
#include <zstd.h>
extern "C" int third_party_check() {
    int checks = 0, failures = 0;
    auto check = [&](bool ok, const char* name) {
        ++checks;
        failures += !ok;
        std::printf("%s %s\n", ok ? "PASS" : "FAIL", name);
    };
    check(fmt::format("{}", 42) == "42", "fmt");
    spdlog::logger logger("probe");
    check(logger.name() == "probe", "spdlog");
    const unsigned char input[] = "bionic dependency probe";
    unsigned char compressed[128]{}, output[128]{};
    uLongf size = sizeof(compressed), length = sizeof(output);
    check(compress(compressed, &size, input, sizeof(input)) == Z_OK &&
              uncompress(output, &length, compressed, size) == Z_OK && length == sizeof(input) &&
              std::memcmp(input, output, length) == 0,
          "zlib_roundtrip");
    check(png_access_version_number() == PNG_LIBPNG_VER, "png_version");
    FT_Library freetype = nullptr;
    check(FT_Init_FreeType(&freetype) == 0, "freetype_init");
    if (freetype)
        FT_Done_FreeType(freetype);
    check(SDL_GetVersion() == SDL_VERSION, "sdl3_version");
    Sirit::Module module;
    module.AddCapability(spv::Capability::Shader);
    const auto words = module.Assemble();
    check(words.size() >= 5 && words[0] == spv::MagicNumber, "sirit_assemble");
    // Default device must actually open. The runner selects drivers=opensl;
    // a null backend is not accepted as audio-device evidence.
    ALCdevice* audio = alcOpenDevice(nullptr);
    ALCcontext* context = audio ? alcCreateContext(audio, nullptr) : nullptr;
    check(context && alcMakeContextCurrent(context), "openal_device_context");
    alcMakeContextCurrent(nullptr);
    if (context)
        alcDestroyContext(context);
    if (audio)
        alcCloseDevice(audio);
    check(libusb_get_version()->major == 1, "libusb_version");
    const auto decoder = aacDecoder_Open(TT_MP4_ADTS, 1);
    check(decoder != nullptr, "fdk_aac_open");
    if (decoder)
        aacDecoder_Close(decoder);
    const auto encoded = ZSTD_compress(compressed, sizeof(compressed), input, sizeof(input), 1);
    const auto decoded =
        ZSTD_isError(encoded) ? 0 : ZSTD_decompress(output, sizeof(output), compressed, encoded);
    check(decoded == sizeof(input) && std::memcmp(input, output, sizeof(input)) == 0,
          "zstd_roundtrip");
    google::protobuf::Any message, copy;
    message.set_type_url("bionic-probe");
    message.set_value("payload");
    check(copy.ParseFromString(message.SerializeAsString()) && copy.value() == "payload",
          "protobuf_roundtrip");
    const hwinfo::OS os;
    std::printf("hwinfo os=%s kernel=%s\n", os.name().c_str(), os.kernel().c_str());
    check(os.is64bit() && os.isLittleEndian(), "hwinfo_arch");
    httplib::SSLClient client("localhost", 443);
    check(client.is_valid(), "httplib_tls_context");
    std::printf("checks=%d failures=%d\n", checks, failures);
    return failures ? 1 : 0;
}
