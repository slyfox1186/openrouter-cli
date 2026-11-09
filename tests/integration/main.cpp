#define DOCTEST_CONFIG_IMPLEMENT
#include <doctest/doctest.h>

#include <iostream>

int main(int argc, char** argv) {
    // CTest captures stdout through a pipe; completed cases must be visible before exit.
    std::cout << std::unitbuf;
    return doctest::Context(argc, argv).run();
}
