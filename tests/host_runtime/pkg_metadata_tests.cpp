// Android standalone regression for the installer (compile with pkg_crypto.cpp,
// pkg_type.cpp, aes.c, -llog and -lz). No JNI/RSA is needed for entry extraction.
#include "../../android/shadps4-app/core/runtime/src/main/cpp/pkg/pkg_extractor.cpp"
#include <cassert>
#include <iterator>

extern "C" int bachata_pkg_rsa_decrypt(const uint8_t*, uint8_t*, int) { return -1; }

template <class T>
void SetBig(be<T>& out, u64 value) {
    auto* bytes = reinterpret_cast<u8*>(&out);
    for (size_t i = 0; i < sizeof(T); ++i) bytes[sizeof(T) - i - 1] = value >> (i * 8);
}

int main() {
    using namespace bachata_pkg;
    char temp[] = "/data/local/tmp/pkg-metadata-XXXXXX";
    assert(mkdtemp(temp));
    const fs::path root{temp};
    const auto source = root / "fixture.pkg";
    const int fd = open(source.c_str(), O_CREAT | O_RDWR, 0600);
    assert(fd >= 0 && ftruncate(fd, 4096) == 0);
    ExtractState st;
    SetBig(st.hdr.pkg_table_entry_offset, 0x100);
    SetBig(st.hdr.pkg_table_entry_count, 2);
    SetBig(st.hdr.pkg_size, 4096);
    st.hasEntryKey3 = true;
    for (u8 i = 0; i < 32; ++i) st.entryKey3[i] = i;
    PKGEntry np{};
    SetBig(np.id, 0x403); SetBig(np.flags1, 0x80000000); SetBig(np.flags2, 0x3000);
    SetBig(np.offset, 0x200); SetBig(np.size, 33);
    PKGEntry trp{};
    SetBig(trp.id, 0x1400); SetBig(trp.offset, 0x300); SetBig(trp.size, 4);
    assert(pwrite(fd, &np, sizeof(np), 0x100) == sizeof(np));
    assert(pwrite(fd, &trp, sizeof(trp), 0x120) == sizeof(trp));
    // Independently generated SHA-256(entry || 00..1f) + AES-CBC fixture.
    // Plaintext is 00..20, followed by 15 padding bytes outside logical size.
    const u8 cipher[]{
        0x76,0xaf,0x5f,0x77,0xf7,0xf9,0x36,0xdc,0x79,0xab,0x62,0xb1,0x98,0xe5,0x21,0x9a,
        0x7b,0x9f,0xa7,0x66,0x81,0xef,0xe2,0x12,0xb7,0xb2,0xd9,0x74,0x78,0xd1,0x1c,0x0a,
        0x63,0x77,0x3e,0x4f,0x7d,0x95,0xd7,0x8e,0x10,0x7b,0x45,0x6f,0x63,0xdc,0x7d,0x55};
    const u8 trophy[]{0xdc,0xa2,0x4d,0};
    assert(pwrite(fd, cipher, sizeof(cipher), 0x200) == sizeof(cipher));
    assert(pwrite(fd, trophy, sizeof(trophy), 0x300) == sizeof(trophy));
    const auto read = [](const fs::path& p) {
        std::ifstream f(p, std::ios::binary);
        return std::vector<u8>(std::istreambuf_iterator<char>{f}, {});
    };
    std::string error;
    st.extract_root = root / "ok";
    assert(extract_sce_sys_entries(fd, st, error));
    const auto plain = read(st.extract_root / "sce_sys/npbind.dat");
    assert(plain.size() == 33);
    for (size_t i = 0; i < plain.size(); ++i) assert(plain[i] == i);
    assert(read(st.extract_root / "sce_sys/trophy/trophy00.trp") ==
           std::vector<u8>(std::begin(trophy), std::end(trophy)));

    st.extract_root = root / "missing-key";
    st.hasEntryKey3 = false;
    assert(!extract_sce_sys_entries(fd, st, error));
    assert(!fs::exists(st.extract_root / "sce_sys/npbind.dat"));
    st.hasEntryKey3 = true;
    st.extract_root = root / "blocked-directory";
    fs::create_directories(st.extract_root / "sce_sys");
    std::ofstream(st.extract_root / "sce_sys/trophy") << "keep";
    assert(!extract_sce_sys_entries(fd, st, error));
    st.extract_root = root / "blocked-file";
    fs::create_directories(st.extract_root / "sce_sys/npbind.dat");
    assert(!extract_sce_sys_entries(fd, st, error));
    st.extract_root = root / "short-ciphertext";
    SetBig(st.hdr.pkg_size, 0x200 + 33);
    assert(!extract_sce_sys_entries(fd, st, error));
    SetBig(st.hdr.pkg_size, 4096);
    SetBig(np.flags1, 0);
    assert(pwrite(fd, &np, sizeof(np), 0x100) == sizeof(np));
    st.extract_root = root / "unencrypted";
    assert(extract_sce_sys_entries(fd, st, error));
    assert(read(st.extract_root / "sce_sys/npbind.dat") ==
           std::vector<u8>(cipher, cipher + 33));
    close(fd);
    fs::remove_all(root);
    std::puts("PKG metadata: CBC partial tail, nested trophy, plaintext, missing key, range and write failures passed");
}
