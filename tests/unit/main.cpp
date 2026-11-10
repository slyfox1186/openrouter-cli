#define DOCTEST_CONFIG_IMPLEMENT
#include <doctest/doctest.h>

#include "http.hpp"

#include <cstdio>

int main(int argc, char** argv) {
    const orcli::CurlGlobal curl;
    if (!curl.ok()) {
        std::fputs("libcurl initialization failed\n", stderr);
        return 1;
    }
    return doctest::Context(argc, argv).run();
}
