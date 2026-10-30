// SPDX-License-Identifier: GPL-2.0-or-later
#include "pinyin_composer.h"

#include <algorithm>
#include <mutex>

#include "pinyinime.h"

namespace Libraries::Ime {
namespace {

std::recursive_mutex gDictionaryMutex;
std::weak_ptr<void> gDecoderOwner;
std::string gSystemDictionary;
std::string gUserDictionary;

constexpr size_t kMaxCandidate = 64;
// Bound preedit work to the decoder's supported spelling and phrase lengths.
constexpr size_t kMaxSpelling = 40;
constexpr size_t kMaxChinese = 20;

std::string Utf8(const ime_pinyin::char16 *text, size_t length) {
    std::string out;
    for (size_t i = 0; i < length && text[i] != 0; ++i) {
        const unsigned c = text[i];
        if (c < 0x80) {
            out += static_cast<char>(c);
        } else if (c < 0x800) {
            out += static_cast<char>(0xC0 | (c >> 6));
            out += static_cast<char>(0x80 | (c & 0x3F));
        } else {
            out += static_cast<char>(0xE0 | (c >> 12));
            out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (c & 0x3F));
        }
    }
    return out;
}

std::u16string Candidate(size_t index) {
    ime_pinyin::char16 buffer[kMaxCandidate + 1] = {};
    if (ime_pinyin::im_get_candidate(index, buffer, kMaxCandidate) == nullptr) return {};
    std::u16string text;
    for (size_t i = 0; i < kMaxCandidate && buffer[i] != 0; ++i) text += static_cast<char16_t>(buffer[i]);
    return text;
}

std::string Utf8(const std::u16string &text) {
    return Utf8(reinterpret_cast<const ime_pinyin::char16 *>(text.c_str()), text.size());
}

}  // namespace

void SetPinyinDictionary(const std::string &systemDictionary, const std::string &userDictionary) {
    std::lock_guard<std::recursive_mutex> lock(gDictionaryMutex);
    gSystemDictionary = systemDictionary;
    gUserDictionary = userDictionary;
}

bool PinyinComposer::available() {
    std::lock_guard<std::recursive_mutex> engineLock(gDictionaryMutex);
    if (lease_) return true;
    if (!gDecoderOwner.expired()) return false;
    if (tried_) return false;
    std::string system, user;
    {
        std::lock_guard<std::recursive_mutex> lock(gDictionaryMutex);
        system = gSystemDictionary;
        user = gUserDictionary;
    }
    if (system.empty()) return false;
    tried_ = true;
    open_ = ime_pinyin::im_open_decoder(system.c_str(), user.empty() ? nullptr : user.c_str());
    if (open_) {
        ime_pinyin::im_set_max_lens(kMaxSpelling, kMaxChinese);
        ime_pinyin::im_reset_search();
        lease_ = std::shared_ptr<void>(new int(0), [](void* token) {
            std::lock_guard<std::recursive_mutex> lock(gDictionaryMutex);
            ime_pinyin::im_flush_cache();
            ime_pinyin::im_close_decoder();
            delete static_cast<int*>(token);
        });
        gDecoderOwner = lease_;
    } else {
        // The panel exposes a disabled language switch when the dictionary is unavailable.
    }
    return open_;
}

void PinyinComposer::refresh(size_t count) {
    std::lock_guard<std::recursive_mutex> lock(gDictionaryMutex);
    count_ = count;
    candidates_.clear();
    if (spelling_.empty() || count == 0) {
        preedit_ = spelling_;
        return;
    }
    // The fixed part of the sentence candidate, then the letters from the first unfixed spelling.
    const std::u16string sentence = Candidate(0);
    const size_t fixed = ime_pinyin::im_get_fixed_len();
    const ime_pinyin::uint16 *starts = nullptr;
    const size_t spellings = ime_pinyin::im_get_spl_start_pos(starts);
    size_t decoded = 0;
    const char *letters = ime_pinyin::im_get_sps_str(&decoded);
    std::string rest = letters != nullptr ? letters : spelling_;
    if (starts != nullptr && fixed <= spellings && starts[fixed] <= rest.size()) rest = rest.substr(starts[fixed]);
    preedit_ = Utf8(sentence.substr(0, std::min(fixed, sentence.size()))) + rest;
}

void PinyinComposer::addLetter(char letter) {
    std::lock_guard<std::recursive_mutex> lock(gDictionaryMutex);
    if (!lease_ || letter < 'a' || letter > 'z' || spelling_.size() >= kMaxSpelling) return;
    spelling_ += letter;
    // im_add_letter is a stub in this engine (it returns 0); a search of the whole spelling
    // continues from the previous one's common prefix and keeps the choices fixed in it.
    refresh(ime_pinyin::im_search(spelling_.c_str(), spelling_.size()));
}

bool PinyinComposer::backspace() {
    std::lock_guard<std::recursive_mutex> lock(gDictionaryMutex);
    if (!lease_ || spelling_.empty()) return false;
    if (ime_pinyin::im_get_fixed_len() > 0) {
        refresh(ime_pinyin::im_cancel_last_choice());
        return true;
    }
    spelling_.pop_back();
    if (spelling_.empty()) {
        reset();
        return true;
    }
    ime_pinyin::im_reset_search();
    refresh(ime_pinyin::im_search(spelling_.c_str(), spelling_.size()));
    return true;
}

std::string PinyinComposer::choose(size_t index) {
    std::lock_guard<std::recursive_mutex> lock(gDictionaryMutex);
    if (!lease_ || spelling_.empty() || index >= count_) return {};
    const size_t count = ime_pinyin::im_choose(index);
    const ime_pinyin::uint16 *starts = nullptr;
    const size_t spellings = ime_pinyin::im_get_spl_start_pos(starts);
    if (ime_pinyin::im_get_fixed_len() >= spellings) {
        // The whole spelling is fixed: the only candidate left is the sentence.
        const std::string sentence = Utf8(Candidate(0));
        reset();
        return sentence;
    }
    refresh(count);
    return {};
}

std::string PinyinComposer::commitLetters() {
    std::lock_guard<std::recursive_mutex> lock(gDictionaryMutex);
    std::string letters;
    letters.swap(spelling_);
    reset();
    return letters;
}

void PinyinComposer::reset() {
    std::lock_guard<std::recursive_mutex> lock(gDictionaryMutex);
    spelling_.clear();
    preedit_.clear();
    candidates_.clear();
    count_ = 0;
    if (lease_) ime_pinyin::im_reset_search();
}

void PinyinComposer::flush() {
    std::lock_guard<std::recursive_mutex> lock(gDictionaryMutex);
    if (lease_) ime_pinyin::im_flush_cache();
}

const std::string &PinyinComposer::candidate(size_t index) {
    std::lock_guard<std::recursive_mutex> lock(gDictionaryMutex);
    static const std::string kEmpty;
    if (!lease_ || index >= count_) return kEmpty;
    while (candidates_.size() <= index) candidates_.push_back(Utf8(Candidate(candidates_.size())));
    return candidates_[index];
}

}  // namespace Libraries::Ime
