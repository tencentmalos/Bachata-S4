// SPDX-License-Identifier: GPL-2.0-or-later
#include <cstdio>
#include "shader_recompiler/experimental_features.h"

int main() {
    using Shader::IsTitleShaderExperimentEnabled;
    unsigned checks{}, failures{};
    const auto check = [&](bool value) {
        ++checks;
        failures += !value;
    };
    check(!IsTitleShaderExperimentEnabled("", ""));
    check(!IsTitleShaderExperimentEnabled("", "CUSA03023"));
    check(!IsTitleShaderExperimentEnabled("1", "CUSA03023"));
    check(!IsTitleShaderExperimentEnabled("0", "CUSA03023"));
    check(!IsTitleShaderExperimentEnabled("CUSA34119", "CUSA03023"));
    check(IsTitleShaderExperimentEnabled("CUSA34119", "CUSA34119"));
    check(IsTitleShaderExperimentEnabled("CUSA03023", "CUSA03023"));
    check(!IsTitleShaderExperimentEnabled("CUSA03023", ""));
    check(!IsTitleShaderExperimentEnabled("CUSAxxxxx", "CUSAxxxxx"));
    check(!IsTitleShaderExperimentEnabled("CUSA03023 ", "CUSA03023 "));
    check(!IsTitleShaderExperimentEnabled("*", "*"));
    printf("shader experiments: %u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
