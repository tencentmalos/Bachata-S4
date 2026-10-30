// SPDX-License-Identifier: GPL-2.0-or-later
#include "pinyin_composer.h"
#include <cstdio>
#include <filesystem>
#include <string>
#include <utility>
using namespace Libraries::Ime;
static int checks, failures;
#define CHECK(x) do { ++checks; if (!(x)) { ++failures; std::printf("FAIL %d: %s\n",__LINE__,#x); } } while(0)
static void type(PinyinComposer& c, const char* text) { std::printf("compose: %s\n", text); for (;*text;++text)c.addLetter(*text); }
int main(int argc, char** argv) {
    setbuf(stdout, nullptr);
    if(argc!=3)return 2;
    SetPinyinDictionary(argv[1],argv[2]);
    {
        PinyinComposer a;
        CHECK(a.available());
        type(a,"nihao");
        CHECK(a.composing());
        CHECK(a.candidateCount()>0);
        CHECK(a.candidate(0)=="你好");
        CHECK(a.choose(0)=="你好");
        CHECK(!a.composing());
        type(a,"zhongguo");
        CHECK(a.candidate(0)=="中国");
        CHECK(a.choose(0)=="中国");
        type(a,"nihaox");
        CHECK(a.backspace());
        CHECK(a.candidate(0)=="你好");
        PinyinComposer second;
        CHECK(!second.available()); // do not reset another visible dialog's decoder
        PinyinComposer moved(std::move(a));
        CHECK(moved.available());
        CHECK(moved.choose(0)=="你好");
        type(moved,"xyz");
        CHECK(moved.commitLetters()=="xyz");
        CHECK(!moved.composing());
        CHECK(!moved.backspace());
        type(moved,"nihao"); moved.reset();
        CHECK(moved.candidateCount()==0);
        CHECK(moved.choose(999).empty());
        moved.flush();
    }
    {
        PinyinComposer reopened;
        CHECK(reopened.available());
        type(reopened,"zhongwen");
        CHECK(reopened.candidate(0)=="中文");
        CHECK(reopened.choose(0)=="中文");
        type(reopened,std::string(80,'a').c_str());
        CHECK(reopened.commitLetters().size()==40);
        CHECK(reopened.candidate(999).empty());
    }
    std::printf("PINYIN checks=%d failures=%d\n",checks,failures);
    return failures?1:0;
}
