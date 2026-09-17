// Copyright (c) 2026 aopenfx contributors.
//
// The build tag, checked against the translation unit that wrote it.
//
// Two jobs. Run on its own, it checks that the tag names this compiler's
// standard-library ABI switch and that the sizes it quotes are this build's
// sizes -- a tag that lied about what it measured would be worse than none.
// Run with --print, it writes the tag and nothing else, so a CTest script can
// build it twice with a different ABI and require the two tags to differ.
#include <cstdio>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

#include "aofx/Version.h"

namespace {

int failures = 0;

void expect(bool ok, const char* what) {
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n  tag: %s\n", what, aofx::buildTag());
        ++failures;
    } else {
        std::printf("ok  : %s\n", what);
    }
}

bool contains(const char* text, const std::string& needle) {
    return std::strstr(text, needle.c_str()) != nullptr;
}

}   // namespace

int main(int argc, char** argv) {
    const char* tag = aofx::buildTag();
    if (argc > 1 && std::strcmp(argv[1], "--print") == 0) {
        std::printf("%s\n", tag);
        return 0;
    }

    expect(tag != nullptr && tag[0] != '\0', "the tag is not empty");
    expect(aofx::buildTag() == tag, "the pointer is stable across calls");
    expect(std::strlen(tag) + 1 < sizeof(aofx::detail::BuildTagText{}.text),
           "the tag was not truncated");

    expect(contains(tag, "string=" + std::to_string(sizeof(std::string))),
           "the tag quotes this build's sizeof(std::string)");
    expect(contains(tag, "vector=" + std::to_string(sizeof(std::vector<int>))),
           "the tag quotes this build's sizeof(std::vector<int>)");
    expect(contains(tag, "function=" + std::to_string(sizeof(std::function<void()>))),
           "the tag quotes this build's sizeof(std::function<void()>)");

#if defined(__GLIBCXX__) && defined(_GLIBCXX_USE_CXX11_ABI)
    expect(contains(tag, "cxx11abi=" + std::to_string(_GLIBCXX_USE_CXX11_ABI)),
           "libstdc++: the tag names _GLIBCXX_USE_CXX11_ABI");
#elif defined(_LIBCPP_VERSION) && defined(_LIBCPP_ABI_VERSION)
    expect(contains(tag, "abi=" + std::to_string(_LIBCPP_ABI_VERSION)),
           "libc++: the tag names _LIBCPP_ABI_VERSION");
#elif defined(_MSC_VER) && defined(_ITERATOR_DEBUG_LEVEL)
    expect(contains(tag, "idl=" + std::to_string(_ITERATOR_DEBUG_LEVEL)),
           "MSVC: the tag names _ITERATOR_DEBUG_LEVEL");
#endif

    std::printf("%s\n", failures == 0 ? "all checks passed" : "FAILED");
    return failures == 0 ? 0 : 1;
}
