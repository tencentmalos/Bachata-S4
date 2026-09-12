// SPDX-License-Identifier: GPL-2.0-or-later
extern "C" int ffmpeg_media_check(const char* path);
int main(int argc, char** argv) {
    return argc == 2 ? ffmpeg_media_check(argv[1]) : 2;
}
