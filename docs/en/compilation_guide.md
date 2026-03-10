# Compilation Guide<a name="EN-US_TOPIC_0000002520718454"></a>

## Introduction<a name="EN-US_TOPIC_0000002550305593"></a>

This document describes how to install and compile Hyperscan based on the Kunpeng 920 processor and openEuler.

Hyperscan  is a high-performance regular expression matching library. It is developed based on Perl Compatible Regular Expressions \(PCRE\) and is open-source under the Berkeley Software Distribution \(BSD\) license. It follows the regular expression syntax of the commonly used libpcre library but has its own C interfaces. Based on the official Hyperscan release and Kunpeng microarchitecture, the implementation mechanism of core interfaces is redesigned, the development and performance are optimized, and the software package suitable for the Kunpeng platform is released. Users of the Kunpeng platform can download this software package based on their service requirements to improve the stability and performance of services on the Kunpeng platform.

The following functions are added to the Hyperscan version dedicated for the Kunpeng platform:

- The Kunpeng platform branch fully compatible with Armv8-A is added. In addition, its use on the x86 platform is not affected.
- NEON instruction, inline assembly, data alignment, instruction alignment, memory prefetch, static branch prediction, and adjusted code structure are used to improve the performance on the Kunpeng platform.
- The Kunpeng Hyperscan Enhanced Library \(KHSEL\) is released. It includes the KHSEL\_ops and KHSEL\_core sub-libraries, a hybrid model with a short-rule bypass, and a false-positive blocking model.
    - KHSEL\_ops provides the ReplaceAllAcc function, which accelerates the ReplaceAll function of the C++ standard library in a fixed rule. The optimization effect can be obtained on Kunpeng 920 series processors.
    - KHSEL\_core optimizes the large-pattern matching algorithm FDR, small-pattern quick matching algorithm Shufti, and long-rule verification, and enhances the scan performance of Hyperscan for processing datasets such as snort\_literal and snort\_pcre.
    - The hybrid model with a short-rule bypass significantly improves the matching performance for rule sets containing short rules.
    - The false-positive blocking model greatly improves the matching performance for rule sets that contain bad string fragments. "Bad strings" refer to a small number of rules with special fragments, causing excessive false positives in multi-pattern matching. This triggers a large number of interpreter calls and inefficient long-rule verification, but yields zero true matches. These unnecessary interpreter calls become computing hotspots, undermining the pre-filtering capability of multi-pattern matching.

For more information about Hyperscan, visit the  [Kunpeng repository on GitCode](https://gitcode.com/boostkit/hyperscan).

This document describes how to install and compile Hyperscan based on the Kunpeng 920 processor and openEuler.
## Environment Requirements<a name="EN-US_TOPIC_0000002550305599"></a>

### Verified Environments<a name="EN-US_TOPIC_0000002550345615"></a>

Before installing Hyperscan, prepare the hardware and software environments to facilitate subsequent installation operations.

Hyperscan can function properly on Kunpeng 920 series processors running openEuler 22.03 LTS SP3 or openEuler 24.03 LTS SP4. If you encounter any problem when using this feature, check that your environment is a verified environment.

Before installing Hyperscan, prepare the hardware and software environments to facilitate subsequent installation operations.
### Software Requirements<a name="EN-US_TOPIC_0000002550345611"></a>

Before installing and compiling software, obtain the software packages by referring to the links provided in this section.

**Software Requirements<a name="section157921256102515"></a>**

[Table 1](#table334392814610)  lists the software requirements.

**Table  1**  Software requirements

|Software|Version|Description|How to Obtain|
|--|--|--|--|
|GCC|10.3 or later|Required.|-|
|CMake|2.8.11 or later|Required.|-|
|Ragel|6.9 or later|Required. The compilation depends on Ragel.|Link|
|Boost|1.57 or later|Required. The compilation depends on the Boost header file.|Link|
|PCRE|8.41 or later|Optional. The compilation of the Hyperscan verification tool hscollider depends on PCRE 8.41 or later.|Link|
|SQLite|SQLite 3|Optional. The compilation of the Hyperscan test tool hsbench depends on SQLite 3.|Use Yum to install it.|
|Hyperscan|dev_neo|Required. Software to be compiled.|Link|
|KSL|2.5.3|Required. Software package with KHSEL enhancements. The string matching algorithms FDR and shufti are optimized and integrated in the **BoostKit-ksl_2.5.3.zip** software package.|Contact Huawei technical support.|


>![](public_sys-resources/icon-note.gif) **NOTE:** 
>Before using the software package, read and agree to  [Kunpeng BoostKit User License Agreement 2.0](https://www.hikunpeng.com/en/legal/developer/boostkit/software/protocol).

**Verifying Software Package Integrity<a name="section1764281413264"></a>**

After downloading a software package from the Kunpeng community, verify the software package to ensure that it is consistent with the original one on the website.

1. Obtain software packages and corresponding digital certificates according to  [Table 1](#table334392814610).
2. Obtain the verification tool and guide from the  [Huawei enterprise website](https://support.huawei.com/enterprise/en/tool/pgp-verify-TL1000000054).
3. Verify the package integrity by following instructions in the  _OpenPGP Signature Verification Guide_.

Before installing and compiling software, obtain the software packages by referring to the links provided in this section.


## Configuring the Compilation Environment<a name="EN-US_TOPIC_0000002550305597"></a>

### Configuring a Local Source<a name="EN-US_TOPIC_0000002550305595"></a>

Configure the Yum source properly to install the required software and dependencies.

1. Mount the system image. This document uses openEuler as an example.

    ```
    mount YOUR_OS.iso /mnt -o loop
    ```

    >![](public_sys-resources/icon-note.gif) **NOTE:** 
    >_**YOUR\_OS.iso**_  indicates the image file of the OS in your environment.

2. Configure the local Yum source.
    1. Create and open the  **/etc/yum.repos.d/openEuler.repo**  file.

        ```
        vim /etc/yum.repos.d/openEuler.repo
        ```

        >![](public_sys-resources/icon-note.gif) **NOTE:** 
        >The  **openEuler.repo**  file needs to be manually created. You are advised to back up original .repo files.

    2. Press  **i**  to enter the insert mode and add the following content to the  **openEuler.repo**  file:

        ```
        [openEuler]
        name=openEuler
        baseurl=file:///mnt
        enabled=1
        gpgcheck=0
        ```

    3. Press  **Esc**, type  **:wq!**, and press  **Enter**  to save the file and exit.

3. Make the Yum source configuration take effect.

    ```
    yum clean all
    yum makecache
    ```

Configure the Yum source properly to install the required software and dependencies.
### Installing Ragel<a name="EN-US_TOPIC_0000002518785772"></a>

Hyperscan compilation depends on Ragel. In this document, Ragel 6.10 is used in the compilation environment.

1. Obtain the Ragel 6.10 source package.

    ```
    wget http://www.colm.net/files/ragel/ragel-6.10.tar.gz
    ```

    >![](public_sys-resources/icon-note.gif) **NOTE:** 
    >If the server cannot connect to the Internet, you can download the software package to the local PC and then upload it to the server. For the download address, see  [Environment Requirements](environment-requirements.md).

2. Decompress the source package.

    ```
    tar -xzf ragel-6.10.tar.gz
    ```

3. Access the Ragel source code directory.

    ```
    cd ./ragel-6.10
    ```

4. Compile and install Ragel.

    ```
    ./configure
    make
    make install
    ```

5. Check the Ragel version to verify that the Ragel is successfully installed.

    ```
    ragel -v
    ```

    If "Ragel State Machine Compiler version 6.10 March 2017" is displayed, the installation is successful.

Hyperscan compilation depends on Ragel. In this document, Ragel 6.10 is used in the compilation environment.
### Configuring Boost<a name="EN-US_TOPIC_0000002518785768"></a>

Hyperscan compilation requires Boost 1.57 or later. In this document, Boost 1.87 is used. The following are two methods for configuring Boost. Select one as required.

- Method 1: Download the Boost software package and create a symbolic link. This method does not require Boost installation.
- Method 2: Download the Boost software package and install it on the server. This method does not require symbolic link creation.

The detailed configuration steps are as follows.

**\(Method 1\) Downloading the Software Package and Creating a Symbolic Link<a name="section194461025186"></a>**

1. Obtain the Boost 1.87 source package.

    ```
    wget https://archives.boost.io/release/1.87.0/source/boost_1_87_0.tar.gz
    ```

2. Decompress the source package.

    ```
    tar -zxf boost_1_87_0.tar.gz
    ```

3. For details about how to create a symbolic link, see  [2](compiling-hyperscan.md#li209412339197).

**\(Method 2\) Downloading and Installing the Software Package<a name="section1857344614147"></a>**

1. Obtain the software package.

    ```
    wget https://archives.boost.io/release/1.87.0/source/boost_1_87_0.tar.gz
    ```

2. Decompress the software package.

    ```
    tar -zxf boost_1_87_0.tar.gz
    ```

3. Go to the directory generated after the decompression.

    ```
    cd boost_1_87_0
    ```

4. Run the** bootstrap.sh**  script and set related parameters.

    ```
    ./bootstrap.sh --with-libraries=all --with-toolset=gcc
    ```

5. Perform the compilation.

    ```
    ./b2 toolset=gcc
    ```

6. Install Boost.

    ```
    ./b2 install --prefix=/usr
    ```

    If information similar to the following is displayed, the installation is successful.

    ![](figures/en-us_image_0000002518785776.png)

7. Update the dynamic link libraries in the system.

    ```
    ldconfig
    ```

Hyperscan compilation requires Boost 1.57 or later. In this document, Boost 1.87 is used. The following are two methods for configuring Boost. Select one as required.
### Downloading PCRE<a name="EN-US_TOPIC_0000002518625864"></a>

The compilation of the Hyperscan tool hscollider depends on PCRE 8.41 or later. This document uses PCRE 8.43 as an example.

1. Obtain the PCRE 8.43 source package as instructed in  [Table 1](software-requirements.md#table334392814610).
2. Decompress the source package.

    ```
    tar -zxf pcre-8.43.tar.gz
    ```

The compilation of the Hyperscan tool hscollider depends on PCRE 8.41 or later. This document uses PCRE 8.43 as an example.
### Installing SQLite<a name="EN-US_TOPIC_0000002518785774"></a>

The compilation of the Hyperscan tool hsbench depends on SQLite 3. Run the Yum command to install SQLite and the SQLite development suite. After the installation is complete, check the SQLite version.

1. Install SQLite and the SQLite development suite.

    ```
    yum install sqlite sqlite-devel
    ```

2. After the installation is complete, run the following command to check whether the development suite is properly configured:

    ```
    pkg-config --libs sqlite3
    ```

    - If yes, the following information is displayed.

        ```
        -lsqlite3
        ```

    - If not, perform the following steps:
        1. Open the  **/usr/lib64/pkgconfig/sqlite3.pc**  file.

            ```
            vim /usr/lib64/pkgconfig/sqlite3.pc
            ```

        2. Press  **i**  to enter the insert mode and add the following content to the  **/usr/lib64/pkgconfig/sqlite3.pc**  file:

            ```
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
            >Replace  **libdir**  and  **includedir**  with the actual installation paths.

        3. Press  **Esc**, type  **:wq!**, and press  **Enter**  to save the file and exit.

The compilation of the Hyperscan tool hsbench depends on SQLite 3. Run the Yum command to install SQLite and the SQLite development suite. After the installation is complete, check the SQLite version.
### Installing KHSEL<a name="EN-US_TOPIC_0000002550345613"></a>

KHSEL is a Hyperscan enhancement package used to improve the scan performance of Hyperscan.

1. Obtain the KHSEL-related package  **BoostKit-ksl\_2.5.3.zip**  based on  [Software Requirements](software-requirements.md).
2. Decompress  **BoostKit-ksl\_2.5.3.zip**  to obtain the binary Kunpeng System Library \(KSL\) RPM package.
3. Install KSL.

    ```
    rpm -ivh boostkit-ksl-xxxx.aarch64.rpm
    ```

    After the installation, the system automatically adds the path to the  **lib**  folder, that is,  **/usr/local/ksl/lib**, to the environment variable  **LD\_LIBRARY\_PATH**.

    In the preceding command,  **_xxxx_**  indicates the version number.

4. Obtain the Hyperscan source code from GitCode by pulling the  **dev\_neo**  branch.

    ```
    git clone https://gitcode.com/boostkit/hyperscan.git -b dev_neo
    ```

5. Go to the  **hyperscan**  directory.

    ```
    cd hyperscan
    ```

6. Compile Hyperscan based on instructions from step 2 in  [Compiling Hyperscan](compiling-hyperscan.md).

KHSEL is a Hyperscan enhancement package used to improve the scan performance of Hyperscan.


## Compiling Hyperscan<a name="EN-US_TOPIC_0000002518625866"></a>

In the Hyperscan source code directory, add the Boost header file and PCRE dependency library, and compile the source code in static library, dynamic library, or debug mode.

1. Go to the Hyperscan source code directory.

    ```
    cd hyperscan
    ```

2. Add the Boost header file.

    ```
    ln -s {boost_path}/boost include/boost
    ```

    >![](public_sys-resources/icon-note.gif) **NOTE:** 
    >The compilation depends on the Boost header file.  _**\{boost\_path\}**_  indicates the full path after  **boost\_1\_87\_0.tar.gz**  is decompressed. It is recommended that  _**boost\_path**_  be set to an absolute path.

3. Add the PCRE dependency library.

    The compilation of the hscollider tool source code depends on the PCRE tool.

    1. Go to the PCRE 8.43 download directory, extract and copy the  **pcre-8.43**  folder to the Hyperscan source code directory, and rename the folder as  **pcre**.

        ```
        cp -rf ./pcre-8.43 hyperscan/pcre
        ```

    2. Open  **pcre/CMakeLists.txt**.

        ```
        vim hyperscan/pcre/CMakeLists.txt
        ```

    3. Press  **i**  to enter the insert mode and comment out line 77 in the copied  **pcre/CMakeLists.txt**  file as follows:

        ```
        CMAKE_MINIMUM_REQUIRED(VERSION 2.8.0)
        #CMAKE_POLICY(SET CMP0026 OLD)
        ```

        In the  **CMakeLists.txt**  file, the command in line 77 cannot be identified in CMake versions earlier than 2.8.1 and does not affect functionality. The command needs to be commented out. You can also upgrade CMake to 3.0 or later to solve the problem that the  **CMAKE\_POLICY**  command cannot be identified.

    4. Press  **Esc**, type  **:wq!**, and press  **Enter**  to save the file and exit.

4. Compile the source code.
    1. Go to the Hyperscan source code directory.

        ```
        cd hyperscan
        ```

    2. Create a  **build**  directory.

        ```
        mkdir -p build
        ```

    3. Go to the  **build**  directory.

        ```
        cd build
        ```

    4. Perform the compilation.

        The compilation mode can be release or debug, and the compilation results can be static or dynamic libraries. By default, the release mode is used and static libraries are generated. If dynamic libraries are required, add the corresponding compilation option.

        - Compile static libraries from the source code. By default, static libraries are generated in release mode.

            ```
            cmake ..
            make -j
            ```

            After the compilation, the following static libraries and test programs of Hyperscan are generated by default:

            ![](figures/en-us_image_0000002550305603.png)

            Generated test programs:

            ![](figures/1.png)

            Generated static libraries:

            ![](figures/2.png)

        - Compile dynamic libraries from the source code. Add the  **-DBUILD\_SHARED\_LIBS=ON**  option to the compile command.

            ```
            cmake .. -DBUILD_SHARED_LIBS=ON
            ```

            Generated dynamic libraries:

            ![](figures/3.png)

        - Compile the source code in debug mode. Add the  **-DCMAKE\_BUILD\_TYPE=DEBUG**  option to the compile command.

            ```
            cmake .. -DCMAKE_BUILD_TYPE=DEBUG
            ```

In the Hyperscan source code directory, add the Boost header file and PCRE dependency library, and compile the source code in static library, dynamic library, or debug mode.

