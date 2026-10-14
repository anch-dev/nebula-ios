// SPDX-License-Identifier: GPL-3.0-only
// shader_msl_check.h with Metal: each MSL source is compiled by the system
// Metal compiler, through a device when one exists (CI virtual machines may
// have none) or else the offline compiler (xcrun metal).

#include "shader_msl_check.h"

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <unistd.h>

namespace galaxy_test {
namespace {

id<MTLDevice> device() {
    static id<MTLDevice> instance = [] {
        id<MTLDevice> created = MTLCreateSystemDefaultDevice();
        std::printf("note: Metal device: %s\n",
                    created != nil ? [[created name] UTF8String] : "none (using xcrun metal)");
        return created;
    }();
    return instance;
}

bool offline_compile(const std::string& msl, std::string& error) {
    char directory[] = "/tmp/galaxy_msl_XXXXXX";
    if (mkdtemp(directory) == nullptr) {
        error = "mkdtemp failed";
        return false;
    }
    const std::string source = std::string(directory) + "/shader.metal";
    const std::string log = std::string(directory) + "/log.txt";
    {
        std::ofstream out(source);
        out << msl;
    }
    const std::string command = "xcrun -sdk macosx metal -std=metal2.3 -c '" + source +
                                "' -o /dev/null > '" + log + "' 2>&1";
    const int status = std::system(command.c_str());
    std::ifstream in(log);
    error.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    std::remove(source.c_str());
    std::remove(log.c_str());
    rmdir(directory);
    return status == 0;
}

}  // namespace

bool msl_compiles(const std::string& msl, const char*, std::string& error) {
    @autoreleasepool {
        id<MTLDevice> metal = device();
        if (metal == nil) return offline_compile(msl, error);
        MTLCompileOptions* options = [[MTLCompileOptions alloc] init];
        options.languageVersion = MTLLanguageVersion2_3;
        NSError* failure = nil;
        NSString* source = [NSString stringWithUTF8String:msl.c_str()];
        id<MTLLibrary> library = [metal newLibraryWithSource:source options:options error:&failure];
        if (library == nil) {
            error = failure != nil ? [[failure localizedDescription] UTF8String] : "unknown error";
            return false;
        }
        return true;
    }
}

}  // namespace galaxy_test
