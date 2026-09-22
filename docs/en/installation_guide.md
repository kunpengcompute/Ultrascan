# Installation Guide

<!-- md-trans-meta sourceCommit=fd215461e79615cfea45605856a99c32a14a7e4b translatedAt=2026-08-27T01:40:06.974Z pushedAt=2026-09-09T07:42:21.598Z -->

## Introduction

This document describes how to install and compile Ultrascan based on new Kunpeng 920 processor models and openEuler.

Ultrascan is a high-performance regular expression matching library. It is developed based on Perl Compatible Regular Expressions (PCRE) and is open-source under the Berkeley Software Distribution (BSD) license. It follows the regular expression syntax of the libpcre library but has its own C interfaces. Based on the official Ultrascan release and Kunpeng microarchitecture, the implementation mechanism of core interfaces is redesigned, the development and performance are optimized, and the software package suitable for the Kunpeng platform is released. Users of the Kunpeng platform can download this software package based on their service requirements to improve the stability and performance of services on the Kunpeng platform.

The following functions are added to the Ultrascan version dedicated for the Kunpeng platform:

- The Kunpeng platform branch fully compatible with Armv8-A is added. In addition, its use on the x86 platform is not affected.
- NEON instructions, inline assembly, data alignment, instruction alignment, memory prefetch, static branch prediction, and code restructuring are used to improve the performance on the Kunpeng platform.
- The Kunpeng Hyperscan Enhanced Library (KHSEL) is released. It includes a hybrid model with a short-rule bypass and a false-positive blocking model.
    - KHSEL optimizes the large-pattern matching algorithm FDR, small-pattern quick matching algorithm Shufti, and long-rule verification, and enhances the scan performance of Ultrascan for processing datasets such as snort_literal and snort_pcre.
    - The hybrid model with a short-rule bypass significantly improves the matching performance for rule sets containing short rules.
    - The false-positive blocking model greatly improves the matching performance for rule sets containing bad string fragments. Bad strings refer to a small number of rules with special fragments, causing excessive false positives in multi-pattern matching. This triggers a large number of interpreter calls and inefficient long-rule verification, but yields zero true matches. These unnecessary interpreter calls become computing hotspots, undermining the pre-filtering capability of multi-pattern matching.
- The universal bytecode function is added. Users can use the hsdump tool to compile rule sets into universal bytecode that can run on both x86 and Kunpeng computing platforms.
- Feedback-driven optimization for regular expression matching is added. On the AArch64 platform, feedback is collected based on the real scanning data, and the rule database is recompiled using the feedback.

For more information about Ultrascan, visit the [Kunpeng repository on GitCode](https://gitcode.com/boostkit/Ultrascan).

## Environment Requirements

### Verified Environments

Ultrascan can function properly on new Kunpeng 920 processor models running openEuler 22.03 LTS SP4 or openEuler 24.03 LTS SP3. If you encounter any problem with Ultrascan, confirm that your environment is a verified environment.

### Software Requirements

Before installing and compiling software, obtain the software packages by referring to the links provided in this section.

[**Table 1**](#software-requirements-table) describes the software requirements.

**Table 1** Software requirements<a id="software-requirements-table"></a>

|Software Name|Version|Description|How to Obtain|
|--|--|--|--|
|Git|Provided by the system software repository|Mandatory for obtaining the Ultrascan source code.|Install it using Yum.|
|GCC|10.3 or later|Mandatory.|-|
|CMake|2.8.11 or later|Mandatory.|-|
|Ragel|6.9 or later|Mandatory. The compilation depends on Ragel.|[Link](http://www.colm.net/files/ragel/ragel-6.10.tar.gz)|
|Boost|1.57 or later|Mandatory. The compilation depends on the Boost header file.|[Link](https://archives.boost.io/release/1.87.0/source/boost_1_87_0.tar.gz)|
|PCRE|8.41 or later|Optional. The compilation of the Ultrascan verification tool hscollider depends on PCRE 8.41 or later.|[Link](https://sourceforge.net/projects/pcre/files/pcre/8.43/pcre-8.43.tar.gz)|
|SQLite|SQLite 3|Optional. The compilation of the Ultrascan test tools hsbench and hspgo depends on SQLite 3.|Install it using Yum.|
|Ultrascan|master|Mandatory. Software to be compiled.|[Link](https://gitcode.com/boostkit/Ultrascan/tree/master)|

## Configuring the Compilation Environment

### Obtaining Project Code

1. Install Git. If Git is already installed, skip this step.

    ```bash
    yum install -y git
    ```

2. Clone the Ultrascan source code from the GitCode repository to `/opt/Ultrascan`.

    ```bash
    cd /opt
    git clone https://gitcode.com/boostkit/Ultrascan.git /opt/Ultrascan
    ```

    >![](public_sys-resources/icon-note.gif) **NOTE:**
    >Before cloning, ensure that the file system where `/opt` is located has several GB of free space, and that the target path `/opt/Ultrascan` does not exist or is empty. If a complete Ultrascan Git repository already exists in this path, do not clone it again. In this case, you can skip this step directly.

### Configuring the Working Directory

This document places the Ultrascan source code and related files under `/opt/Ultrascan`. The directory layout is as follows:

```text
/opt/Ultrascan/
├── build/           # Default static library build directory
├── build-debug/     # Debug build directory
├── build-shared/    # Dynamic library build directory
├── build-feedback/  # Feedback-driven optimization build directory
└── deps/            # Third-party dependencies
```

Create the default build directory and dependency directory upon the first use.

```bash
mkdir -p /opt/Ultrascan/build \
         /opt/Ultrascan/deps
```

The `/opt` directory usually requires administrator permissions for creation. Ensure that the user who performs download, compilation, and testing has read and write permissions on the above directories.

Before compilation, run the following command to confirm that the file system where `/opt/Ultrascan` is located has sufficient available space:

```bash
df -h /opt/Ultrascan
```

The complete source code, third-party dependencies, and multiple build configurations occupy several GB of space. If the space is insufficient, configure a file system with sufficient capacity for `/opt/Ultrascan` before proceeding to the next step.

### (Optional) Configuring the Local Repository

>![](public_sys-resources/icon-note.gif) **NOTE:** For an offline environment, configure the local repository. For an online environment, skip this step.

Configure a Yum repository properly for subsequent installation of the required software and dependencies.

1. Mount the system image. This document uses openEuler as an example.

    ```bash
    mount YOUR_OS.iso /mnt -o loop
    ```

    >![](public_sys-resources/icon-note.gif) **NOTE:**
    >`YOUR_OS.iso` indicates the image file of the OS in your environment.

2. Configure a local Yum repository.
    1. Create and open the `/etc/yum.repos.d/openEuler.repo` file.

        ```bash
        vim /etc/yum.repos.d/openEuler.repo
        ```

        >![](public_sys-resources/icon-note.gif) **NOTE:**
        >The `openEuler.repo` file needs to be manually created. You are advised to back up original `.repo` files.

    2. Press `i` to enter the insert mode and add the following content to the `openEuler.repo` file:

        ```bash
        [openEuler]
        name=openEuler
        baseurl=file:///mnt
        enabled=1
        gpgcheck=0
        ```

    3. Press `Esc`, type `:wq!`, and press `Enter` to save the file and exit.

3. Make the Yum repository configuration take effect.

    ```bash
    yum clean all
    yum makecache
    ```

### Installing Ragel

Ultrascan compilation depends on Ragel. In this document, Ragel 6.10 is used in the compilation environment.

1. Obtain the Ragel 6.10 source package.

    ```bash
    wget -P /opt/Ultrascan/deps \
        http://www.colm.net/files/ragel/ragel-6.10.tar.gz
    ```

    >![](public_sys-resources/icon-note.gif) **NOTE:**
    >If the server cannot connect to the Internet, you can download the software package to the local PC and then upload it to the server. For details about the software package download address, see [**Table 1**](#software-requirements-table).

2. Decompress the source package.

    ```bash
    tar -C /opt/Ultrascan/deps -xzf \
        /opt/Ultrascan/deps/ragel-6.10.tar.gz
    ```

3. Access the Ragel source code directory.

    ```bash
    cd /opt/Ultrascan/deps/ragel-6.10
    ```

4. Compile and install Ragel.

    ```bash
    ./configure
    make
    make install
    ```

5. Check the Ragel version to verify whether the Ragel is successfully installed.

    ```bash
    ragel -v
    ```

    If `Ragel State Machine Compiler version 6.10 March 2017` is displayed, the installation is successful.

### Configuring Boost

Ultrascan compilation requires Boost 1.57 or later. In this document, Boost 1.87 is used. The following are two methods for configuring Boost. Select one as required.

- Method 1: Download the Boost software package and create a symbolic link. This method does not require Boost installation.
- Method 2: Download the Boost software package and install it on the server. This method does not require symbolic link creation.

The detailed configuration steps are as follows.

**(Method 1) Downloading the Software Package and Creating a Symbolic Link**

1. Obtain the Boost 1.87 source package.

    ```bash
    wget -P /opt/Ultrascan/deps \
        https://archives.boost.io/release/1.87.0/source/boost_1_87_0.tar.gz
    ```

2. Decompress the source code.

    ```bash
    tar -C /opt/Ultrascan/deps -zxf \
        /opt/Ultrascan/deps/boost_1_87_0.tar.gz
    ```

3. Create a symbolic link.

    ```bash
    ln -s /opt/Ultrascan/deps/boost_1_87_0/boost \
        /opt/Ultrascan/include/boost
    ```

    >![](public_sys-resources/icon-note.gif) **NOTE:**
    >The build depends on the Boost header files. The above command links the `boost` header file directory in the extracted directory to the Ultrascan source tree.

**(Method 2) Downloading and Installing the Software Package**

1. Obtain the software package.

    ```bash
    wget -P /opt/Ultrascan/deps \
        https://archives.boost.io/release/1.87.0/source/boost_1_87_0.tar.gz
    ```

2. Decompress the software package.

    ```bash
    tar -C /opt/Ultrascan/deps -zxf \
        /opt/Ultrascan/deps/boost_1_87_0.tar.gz
    ```

3. Go to the directory generated after the decompression.

    ```bash
    cd /opt/Ultrascan/deps/boost_1_87_0
    ```

4. Run the `bootstrap.sh` script and set related parameters.

    ```bash
    ./bootstrap.sh --with-libraries=all --with-toolset=gcc
    ```

5. Perform compilation.

    ```bash
    ./b2 toolset=gcc
    ```

6. Install Boost.

    ```bash
    ./b2 install --prefix=/usr
    ```

    If information similar to the following is displayed, the installation is successful.

    ![](figures/en-us_image_0000002518785776.png)

7. Update the dynamic link libraries in the system.

    ```bash
    ldconfig
    ```

### Downloading PCRE

The compilation of the Ultrascan tool hscollider depends on PCRE 8.41 or later. This document uses PCRE 8.43 as an example.

1. Obtain the PCRE 8.43 source package.

    ```bash
    wget -O /opt/Ultrascan/deps/pcre-8.43.tar.gz \
        https://sourceforge.net/projects/pcre/files/pcre/8.43/pcre-8.43.tar.gz/download
    ```

    >![](public_sys-resources/icon-note.gif) **NOTE:**
    >If the server cannot connect to the Internet, download the source package from the address in [**Table 1**](#software-requirements-table), and then upload it to `/opt/Ultrascan/deps/pcre-8.43.tar.gz`.

2. Decompress the source code.

    ```bash
    tar -C /opt/Ultrascan/deps -zxf \
        /opt/Ultrascan/deps/pcre-8.43.tar.gz
    ```

### Installing SQLite

The compilation of the Ultrascan tools hsbench and hspgo depends on SQLite 3. Run the `yum` command to install SQLite and the SQLite development suite. After the installation is complete, check the SQLite version.

1. Install SQLite and the SQLite development suite.

    ```bash
    yum install -y sqlite sqlite-devel
    ```

2. After the installation is complete, run the following command to check whether the development suite is properly configured:

    ```bash
    pkg-config --libs sqlite3
    ```

    - If yes, the following information is displayed.

        ```bash
        -lsqlite3
        ```

    - If not, perform the following steps:
        1. Open the `/usr/lib64/pkgconfig/sqlite3.pc` file.

            ```bash
            vim /usr/lib64/pkgconfig/sqlite3.pc
            ```

        2. Press `i` to enter the insert mode and add the following content to the `/usr/lib64/pkgconfig/sqlite3.pc` file:

            ```bash
            # Package Information for pkg-config
            
            prefix=/usr
            exec_prefix=/usr
            libdir=/usr/lib64
            includedir=/usr/include
            
            Name: SQLite
            Description: SQL database engine
            Version: 3.37.2
            Libs: -L${libdir} -lsqlite3
            Libs.private: -lm -lz
            Cflags: -I${includedir}
            ```

            >![](public_sys-resources/icon-note.gif) **NOTE:**
            >Replace `libdir` and `includedir` with the actual installation paths.

        3. Press `Esc`, type `:wq!`, and press `Enter` to save the file and exit.

## Compiling Ultrascan

In the Ultrascan source code directory, add the PCRE dependency library, and compile the source code in static library, dynamic library, or debug mode.

1. Confirm that the Ultrascan source code has been placed in `/opt/Ultrascan`.

2. Add the PCRE dependency library.

    The compilation of the hscollider tool source code depends on the PCRE tool.

    1. Go to the PCRE 8.43 download directory, extract and copy the `pcre-8.43` folder to the Ultrascan source code directory, and rename the folder as `pcre`.

        ```bash
        cp -rf /opt/Ultrascan/deps/pcre-8.43 \
            /opt/Ultrascan/pcre
        ```

    2. Open the `pcre/CMakeLists.txt` file.

        ```bash
        vim /opt/Ultrascan/pcre/CMakeLists.txt
        ```

    3. Press `i` to enter the insert mode and comment out `CMAKE_POLICY(SET CMP0026 OLD)` in the copied "pcre/CMakeLists.txt" file as follows:

        ```bash
        CMAKE_MINIMUM_REQUIRED(VERSION 2.8.0)
        #CMAKE_POLICY(SET CMP0026 OLD)
        ```

        The new version of CMake has removed the `OLD` behavior of the `CMP0026` policy, so this command needs to be commented out. This does not affect the PCRE function required by Ultrascan.

    4. Press `Esc`, type `:wq!`, and press `Enter` to save the file and exit.

3. Compile the source code.

    The compilation supports release-mode, debug-mode, dynamic library, and feedback-driven optimization configurations. Select them as needed. If multiple build results need to be retained simultaneously, use separate build directories to avoid options in the CMake cache affecting each other.

    - Compile the static libraries in release mode (without adding compilation options).

        ```bash
        mkdir -p /opt/Ultrascan/build
        cd /opt/Ultrascan/build
        cmake ..
        make -j
        ```

        The release-mode configuration (without adding compilation options) does not generate `hsdump`. To generate universal bytecode as described in [Quick Start](./quick_start.md), use the debug mode below to complete the build.

    - (Optional) Compile the static libraries in debug mode.

        Run the following commands on the Kunpeng computing platform:

        ```bash
        mkdir -p /opt/Ultrascan/build-debug
        cd /opt/Ultrascan/build-debug
        cmake .. -DCMAKE_BUILD_TYPE=DEBUG
        make -j
        ```

        The debug mode generates `/opt/Ultrascan/build-debug/bin/hsdump`.

        Run the following commands on the x86 platform:

        ```bash
        mkdir -p /opt/Ultrascan/build-debug
        cd /opt/Ultrascan/build-debug
        cmake .. -DCMAKE_BUILD_TYPE=DEBUG \
            -DCMAKE_C_FLAGS="-D__X86_64__" \
            -DCMAKE_CXX_FLAGS="-D__X86_64__"
        make -j
        ```

        After the compilation, the following static libraries and test programs of Ultrascan are generated by default:

        ![](figures/en-us_image_0000002550305603.png)

        Generated test programs:

        ![](figures/1.png)

        If the debug mode is enabled, the following test programs are generated:

        ![](figures/6.png)

        Generated static libraries:

        ![](figures/2.png)

    - Compile dynamic libraries.

        ```bash
        mkdir -p /opt/Ultrascan/build-shared
        cd /opt/Ultrascan/build-shared
        cmake .. -DBUILD_SHARED_LIBS=ON
        make -j
        ```

        Generated dynamic libraries:

        ![](figures/3.png)

    - (Optional) Use the compilation option for feedback-driven optimization for regular expression matching. This technology currently supports only AArch64 and is disabled by default.

        ```bash
        mkdir -p /opt/Ultrascan/build-feedback
        cd /opt/Ultrascan/build-feedback
        cmake .. -DHS_ENABLE_FP_FEEDBACK=ON
        make -j
        ```

        This option can be used together with existing options such as `CMAKE_BUILD_TYPE` and `BUILD_SHARED_LIBS`. Note the following when using it:

        - Setting `HS_ENABLE_FP_FEEDBACK=ON` on non-AArch64 platforms such as x86 causes an error during the CMake configuration phase.
        - When this option is not set, the libraries still export feedback-related public symbols, but function calls return `HS_ARCH_ERROR`.
        - `hspgo` depends on SQLite 3. When SQLite is installed and the feedback capability is enabled, `/opt/Ultrascan/build-feedback/bin/hspgo` is generated; when SQLite is missing, the libraries can still be built, but this tool is not generated.
        - When applications integrate this technology via APIs, refer to [Feedback-driven Optimization APIs for Regular Expression Matching](./api_reference.md#4-feedback-driven-optimization-apis-for-regular-expression-matching).

    - (Optional) Select an AArch64 compilation target.

        When the CMake target processor is `aarch64` or `AARCH64`, `HS_ARM_MARCH` can be used to control the instruction set baseline of the AArch64 compilation target. The following values are supported:

        - `AUTO` (default): uses `-march=native -mtune=native` for native builds, and automatically falls back to `PORTABLE` for cross-compilation, or when the C/C++ compiler does not support this flag combination. This parameter applies to scenarios where the build machine and the deployment machine have the same instruction set baseline.
        - `PORTABLE`: always uses `-march=armv8-a+crc`. The build result requires the target machine to support Armv8-A and the CRC32 extension. This option applies to build result distribution across different machine models that meet this minimum baseline, and is also the recommended baseline for performance comparison tests.
        - `native`: explicitly uses `-march=native -mtune=native`, equivalent to the successful path of `AUTO`, but without probe-based fallback.
        - Explicit architecture values (such as `armv8.2-a+crc+sve`, `armv9-a+sve2`, and `armv8.6-a+crc+sve2+sve2-bitperm`): used when the target machine model is unified and the instruction set baseline is known. This option is passed with the architecture value, without adding the `-march=` prefix.

        Usage example:

        ```bash
        mkdir -p /opt/Ultrascan/build-portable
        cd /opt/Ultrascan/build-portable
        cmake .. -DHS_ARM_MARCH=PORTABLE
        make -j
        ```

        Notes on use:

        - The `AUTO` or `native` build result can only be deployed on machines that support all instruction set extensions selected by the compiler; deployment on machines with a lower instruction set baseline may trigger invalid instruction errors. For cross-model distribution, use `PORTABLE`, or select an explicit value based on the target model with the lowest baseline.
        - `PORTABLE` does not use SVE/SVE2 as the build target baseline. When SVE or SVE2 instruction set capability is required, explicitly select an architecture value that includes the corresponding extensions, and ensure that all deployment machines support that value.
        - In cross-compilation, the tool chain file should set `CMAKE_SYSTEM_PROCESSOR` to `aarch64`; otherwise, this option will not take effect.
        - During the configuration phase, `AARCH64 single-ISA build mode/flags` and `crc/sve/sve2/sve2-bitperm` are output, which can be used to verify the actual compilation flags in effect.