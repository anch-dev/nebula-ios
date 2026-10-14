// SPDX-License-Identifier: GPL-3.0-only
// Every presentation post-processing pass (FXAA, FSR 1, SMAA, resample)
// translates to MSL and, where Metal is available, compiles.

#include "galaxy/gx/shader_translate.h"
#include "post_shader_list.h"
#include "shader_msl_check.h"

#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>

int main() {
    namespace xlate = galaxy::gx::xlate;
    int failures = 0;
    for (const PostShaderSource& shader : kPostShaders) {
        std::ifstream in(shader.path, std::ios::binary);
        const std::string source{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
        if (source.empty()) {
            std::printf("FAILED: %s: cannot read %s\n", shader.name, shader.path);
            ++failures;
            continue;
        }
        xlate::Options options;
        options.entry_point = shader.entry;
        options.label = shader.name;
        const xlate::Result result = xlate::hlsl_to_msl(
            source, shader.stage == 'v' ? xlate::Stage::Vertex : xlate::Stage::Pixel, options);
        std::string error;
        if (!result.ok) {
            std::printf("FAILED: %s (-> MSL)\n%s\n", shader.name, result.log.c_str());
            ++failures;
        } else if (!galaxy_test::msl_compiles(result.msl, shader.name, error)) {
            std::printf("FAILED: %s (Metal)\n%s\n", shader.name, error.c_str());
            ++failures;
        } else {
            std::printf("ok: %s (%s)\n", shader.name, result.entry_point.c_str());
        }
    }
    std::printf("%d post-process shader failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
