// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
// Read-only validation utility: inspect an FPKG, or compare positioned reads
// against an independently produced JSON manifest of SHA-256 expectations.
#include "core/file_format/pkg_reader.h"
#include <array>
#include <chrono>
#include <fstream>
#include <iostream>
#include <nlohmann/json.hpp>
#include <openssl/evp.h>

using Core::FileFormat::PkgReader;
using nlohmann::json;

static std::string Hash(PkgReader &reader, size_t id, u64 offset, u64 size) {
  std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> ctx(EVP_MD_CTX_new(),
                                                              EVP_MD_CTX_free);
  if (!ctx || EVP_DigestInit_ex(ctx.get(), EVP_sha256(), nullptr) != 1)
    throw std::runtime_error("SHA-256 initialization failed");
  std::vector<u8> buffer(1024 * 1024);
  for (u64 done = 0; done < size;) {
    const auto take = std::min<u64>(buffer.size(), size - done);
    const auto got =
        reader.Read(id, offset + done, std::span(buffer).first(take));
    if (got != take)
      throw std::runtime_error("short positioned read");
    if (EVP_DigestUpdate(ctx.get(), buffer.data(), got) != 1)
      throw std::runtime_error("SHA-256 update failed");
    done += got;
  }
  std::array<u8, 32> digest{};
  unsigned int length = 0;
  if (EVP_DigestFinal_ex(ctx.get(), digest.data(), &length) != 1 ||
      length != 32)
    throw std::runtime_error("SHA-256 finalization failed");
  constexpr char hex[] = "0123456789abcdef";
  std::string out;
  for (const auto byte : digest) {
    out += hex[byte >> 4];
    out += hex[byte & 15];
  }
  return out;
}
int main(int argc, char **argv) {
  try {
    if (argc < 3 || (std::string_view(argv[1]) != "inspect" &&
                     std::string_view(argv[1]) != "verify")) {
      std::cerr << "Usage: shadps4_pkg_read inspect FILE.pkg | verify FILE.pkg "
                   "manifest.json\n";
      return 2;
    }
    const auto begin = std::chrono::steady_clock::now();
    const auto reader = PkgReader::Open(argv[2]);
    const auto opened = reader->GetStatistics();
    json output;
    output["package"] = std::filesystem::path(argv[2]).filename().string();
    output["open_source_bytes"] = opened.source_bytes;
    if (std::string_view(argv[1]) == "inspect") {
      output["entries"] = json::array();
      for (const auto &entry : reader->Entries())
        output["entries"].push_back({{"path", entry.path},
                                     {"size", entry.size},
                                     {"directory", entry.directory}});
    } else {
      if (argc != 4)
        throw std::runtime_error("verification requires a manifest");
      std::ifstream stream(argv[3]);
      const auto expected = json::parse(stream);
      size_t checks = 0;
      for (const auto &entry : expected.at("entries")) {
        const auto name = entry.at("path").get<std::string>();
        const auto id = reader->Find(name);
        if (id == PkgReader::Missing ||
            reader->Entries()[id].size != entry.at("size").get<u64>() ||
            reader->Entries()[id].directory !=
                entry.at("directory").get<bool>())
          throw std::runtime_error("entry mismatch: " + name);
        ++checks;
      }
      for (const auto &test : expected.at("reads")) {
        const auto name = test.at("path").get<std::string>();
        const auto id = reader->Find(name);
        if (id == PkgReader::Missing ||
            Hash(*reader, id, test.at("offset"), test.at("size")) !=
                test.at("sha256").get<std::string>())
          throw std::runtime_error("read mismatch: " + name + " at " +
                                   test.at("offset").dump());
        ++checks;
      }
      output["checks"] = checks;
      output["status"] = "PASS";
    }
    const auto stats = reader->GetStatistics();
    output["source_bytes"] = stats.source_bytes;
    output["decoded_blocks"] = stats.inflated_blocks;
    output["cache_hits"] = stats.cache_hits;
    output["seconds"] =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - begin)
            .count();
    std::cout << output.dump(2) << '\n';
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
