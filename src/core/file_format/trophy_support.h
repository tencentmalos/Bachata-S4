// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <map>
#include <string>
#include <string_view>
#include "common/types.h"
namespace Core {
std::map<s32,std::string> ExtractTrophies(std::string_view npbind_guest,std::string_view trophy_dir_guest);
}
