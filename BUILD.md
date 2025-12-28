# Building ACP

This project uses an autoconf-style configure script to set up the build environment.

## Prerequisites

### Required
- `g++` or compatible C++ compiler with C++20 support
- `libaio` development files
  - Ubuntu/Debian: `sudo apt-get install libaio-dev`
  - CentOS/RHEL: `sudo yum install libaio-devel`
  - macOS: `brew install libaio`
- `spdlog` development files (header-only logging library)
  - Ubuntu/Debian: `sudo apt-get install libspdlog-dev`
  - CentOS/RHEL: `sudo yum install spdlog-devel`
  - macOS: `brew install spdlog`

### Optional
- `liburing` development files (for advanced asynchronous I/O support)
  - Ubuntu/Debian: `sudo apt-get install liburing-dev`
  - CentOS/RHEL: `sudo yum install liburing-devel`

## Build Instructions

### 1. Configure the project

Basic configuration (liburing disabled by default):
```bash
./configure
```

To enable liburing support:
```bash
./configure --enable-uring
```

To explicitly disable liburing:
```bash
./configure --disable-uring
```

To use a specific C++ compiler:
```bash
CXX=clang++ ./configure
```

### 2. Build the project

```bash
make
```

The executable will be created as `acp` in the project root.

### 3. Clean build artifacts

```bash
make clean
```

To also remove configuration files:
```bash
make distclean
```

## Configuration Details

The configure script performs the following checks:

1. **Required Libraries**:
   - Checks for `libaio` header and linking capability
   - Checks for `spdlog` header availability
   - Fails if either library is not found

2. **Optional Libraries**:
   - Checks if `liburing` is available on the system
   - Reports availability but doesn't fail if missing
   - Can be enabled with `--enable-uring` flag

3. **Generated Files**:
   - `config.h`: Header file with feature definitions
   - `config.mk`: Makefile fragment with compiler settings and flags

## Environment Variables

You can customize the build environment:

```bash
CXX=g++        # C++ compiler executable
CXXFLAGS=...   # Additional compiler flags
```

Example:
```bash
CXX=clang++ CXXFLAGS="-O3 -march=native" ./configure
make
```
