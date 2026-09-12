// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
#include <cstdio>
#include <cstring>
#include <exception>
#include <vector>
#include <SPIRV/GlslangToSpv.h>
#include <Zydis/Zydis.h>
#include <date/tz.h>
#include <glslang/Public/ResourceLimits.h>
#include <glslang/Public/ShaderLang.h>
#include <miniz.h>
#include <openssl/sha.h>
#include <openssl/ssl.h>

extern "C" int shadps4_host_dependencies_check() {
    int checks = 0, failures = 0;
    auto check = [&](bool ok, const char* name) {
        ++checks;
        failures += !ok;
        std::printf("%s %s\n", ok ? "PASS" : "FAIL", name);
    };
    try {
        const auto* zone = date::current_zone();
        check(zone && !zone->name().empty(), "current_timezone");
        if (zone)
            std::printf("timezone=%s\n", zone->name().c_str());
        check(date::locate_zone("UTC") != nullptr, "tzdata_lookup");
    } catch (const std::exception& error) {
        check(false, "timezone_exception");
        std::printf("%s\n", error.what());
    }
    unsigned char digest[SHA256_DIGEST_LENGTH]{};
    const unsigned char expected[] = {0xba, 0x78, 0x16, 0xbf, 0x8f, 0x01, 0xcf, 0xea,
                                      0x41, 0x41, 0x40, 0xde, 0x5d, 0xae, 0x22, 0x23,
                                      0xb0, 0x03, 0x61, 0xa3, 0x96, 0x17, 0x7a, 0x9c,
                                      0xb4, 0x10, 0xff, 0x61, 0xf2, 0x00, 0x15, 0xad};
    SHA256(reinterpret_cast<const unsigned char*>("abc"), 3, digest);
    check(std::memcmp(digest, expected, sizeof(expected)) == 0, "libressl_sha256");
    SSL_CTX* ssl = SSL_CTX_new(TLS_method());
    check(ssl != nullptr, "libressl_tls_context");
    SSL_CTX_free(ssl);
    check(mz_crc32(0, reinterpret_cast<const unsigned char*>("123456789"), 9) == 0xcbf43926,
          "miniz_crc32");
    ZydisDecoder decoder{};
    ZydisDecodedInstruction instruction{};
    ZydisDecodedOperand operands[ZYDIS_MAX_OPERAND_COUNT]{};
    const unsigned char bytes[] = {0x48, 0x89, 0xd8};
    const bool decoded = ZYAN_SUCCESS(ZydisDecoderInit(&decoder, ZYDIS_MACHINE_MODE_LONG_64,
                                                       ZYDIS_STACK_WIDTH_64)) &&
                         ZYAN_SUCCESS(ZydisDecoderDecodeFull(&decoder, bytes, sizeof(bytes),
                                                             &instruction, operands));
    check(decoded && instruction.mnemonic == ZYDIS_MNEMONIC_MOV && instruction.length == 3,
          "zydis_x86_decode_on_arm64");
    const bool initialized = glslang::InitializeProcess();
    check(initialized, "glslang_initialize");
    if (initialized) {
        // Destroy shader/program before FinalizeProcess.
        {
            glslang::TShader shader(EShLangVertex);
            const char* source = "#version 450\nvoid main(){gl_Position=vec4(0,0,0,1);}";
            shader.setStrings(&source, 1);
            shader.setEnvInput(glslang::EShSourceGlsl, EShLangVertex, glslang::EShClientVulkan,
                               100);
            shader.setEnvClient(glslang::EShClientVulkan, glslang::EShTargetVulkan_1_0);
            shader.setEnvTarget(glslang::EShTargetSpv, glslang::EShTargetSpv_1_0);
            const auto messages = static_cast<EShMessages>(EShMsgSpvRules | EShMsgVulkanRules);
            const bool parsed = shader.parse(GetDefaultResources(), 450, false, messages);
            glslang::TProgram program;
            program.addShader(&shader);
            const bool linked = parsed && program.link(messages);
            std::vector<unsigned> spirv;
            if (linked)
                glslang::GlslangToSpv(*program.getIntermediate(EShLangVertex), spirv);
            check(linked && !spirv.empty() && spirv.front() == 0x07230203, "glsl_to_spirv");
            if (!linked)
                std::printf("%s\n%s\n", shader.getInfoLog(), program.getInfoLog());
        }
        glslang::FinalizeProcess();
    }
    std::printf("checks=%d failures=%d\n", checks, failures);
    return failures ? 1 : 0;
}
