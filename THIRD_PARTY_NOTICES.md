# Third-party notices

The application is MIT licensed; dependencies retain their own terms. Release archives are
pinned by SHA-256 in `CMakeLists.txt`. They are downloaded during configuration and are not
vendored in this repository.

| Component | Version | Terms | Use |
| --- | --- | --- | --- |
| [nlohmann/json](https://github.com/nlohmann/json) | 3.12.0 | MIT | Compiled into the executable |
| [CLI11](https://github.com/CLIUtils/CLI11) | 2.7.2 | BSD-3-Clause | Compiled into the executable |
| [libcurl](https://curl.se/docs/copyright.html) | 8.23.0 | curl license | Static library |
| [OpenSSL](https://openssl-library.org/source/license/) | System 3.x | Apache-2.0 | Shared system library |
| [libstdc++ and libgcc](https://gcc.gnu.org/onlinedocs/libstdc++/manual/license.html) | Selected toolchain | GPLv3 with GCC Runtime Library Exception 3.1 | Static runtime libraries |
| [glibc](https://www.gnu.org/software/libc/) | System version | LGPL-2.1-or-later, with component-specific notices | Shared system runtime |
| [doctest](https://github.com/doctest/doctest) | 2.5.3 | MIT | Tests only |
| [cpp-httplib](https://github.com/yhirose/cpp-httplib) | 0.63.0 | MIT | Tests only |

The notices below cover libraries incorporated into the application. When distributing a
binary, include this file and `LICENSE`, and retain the selected toolchain's licensing notices
and [GCC Runtime Library Exception](https://www.gnu.org/licenses/gcc-exception-3.1.html).
If distributing system libraries or test executables too, include their complete licenses
and satisfy their source-availability obligations as applicable. This source repository does
not distribute OpenSSL, glibc or test executables.

## nlohmann/json

MIT License

Copyright (c) 2013-2025 Niels Lohmann

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.

## CLI11

CLI11 2.7.2 Copyright (c) 2017-2026 University of Cincinnati, developed by Henry
Schreiner under NSF AWARD 1414736. All rights reserved.

Redistribution and use in source and binary forms of CLI11, with or without
modification, are permitted provided that the following conditions are met:

1. Redistributions of source code must retain the above copyright notice, this
   list of conditions and the following disclaimer.
2. Redistributions in binary form must reproduce the above copyright notice,
   this list of conditions and the following disclaimer in the documentation
   and/or other materials provided with the distribution.
3. Neither the name of the copyright holder nor the names of its contributors
   may be used to endorse or promote products derived from this software without
   specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR
ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
(INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON
ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

## libcurl

COPYRIGHT AND PERMISSION NOTICE

Copyright (c) 1996 - 2026, Daniel Stenberg, <daniel@haxx.se>, and many
contributors, see the THANKS file.

All rights reserved.

Permission to use, copy, modify, and distribute this software for any purpose
with or without fee is hereby granted, provided that the above copyright
notice and this permission notice appear in all copies.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT OF THIRD PARTY RIGHTS. IN
NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM,
DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE
OR OTHER DEALINGS IN THE SOFTWARE.

Except as contained in this notice, the name of a copyright holder shall not
be used in advertising or otherwise to promote the sale, use or other dealings
in this Software without prior written authorization of the copyright holder.
