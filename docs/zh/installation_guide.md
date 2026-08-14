# 安装指南

## 简介

本文基于鲲鹏920新型号处理器和openEuler操作系统，提供Ultrascan的安装和编译指导。

Ultrascan是一款高性能的正则表达式匹配库，它是以PCRE（Perl-compatible regular expression）为原型开发，并以BSD（Berkeley Software Distribution）许可证开源，遵循libpcre库通用的正则表达式语法，拥有独立的C语言接口。在Ultrascan正式发布版本的基础上，参考鲲鹏微架构特征，重新设计核心接口的实现机制，并完成了开发和性能优化，推出适合鲲鹏计算平台的软件包。使用鲲鹏计算平台的用户可以根据自己业务需求下载本软件包，提升业务在鲲鹏平台上的稳定性和性能。

Ultrascan鲲鹏计算平台软件版本主要增加了以下功能：

- 增加鲲鹏计算平台分支，且完全兼容ARMv8-A，同时确保x86平台使用不受影响。
- 通过使用NEON指令、内联汇编、数据对齐、指令对齐、内存数据预取、静态分支预测、代码结构优化等方法，实现在鲲鹏计算平台的性能提升。
- 发布KHSEL（Kunpeng Hyperscan Enhanced Library）软件增强包，包括短规则旁路混合模型和假阳性阻断模型。
    - KHSEL优化了大规模规则集匹配算法FDR，小规模快速匹配算法Shufti，增强了Ultrascan处理snort_literal，snort_pcre等数据集的scan性能，并且针对长规则校验的场景进行了优化。
    - 提供短规则旁路混合模型，对包含短规则的规则集合能大幅提高匹配性能。
    - 提供假阳性阻断模型，对包含坏字符串片段的规则集合能大幅提高匹配性能。其中的“坏字符串”指少量含特殊片段的规则，其导致多模匹配算法报告的假阳性过多，触发大量的解释器调用和低效的长规则校验，而最终无一真实匹配。大量无谓的解释器调用成为热点，多模匹配失去了应有的预过滤能力。
- 增加通用字节码功能，用户可以通过hsdump工具将规则集编译为通用字节码，该字节码支持在x86和鲲鹏计算平台运行。
- 增加正则匹配反馈优化技术，在AArch64平台根据真实扫描语料采集反馈，并使用反馈重新编译规则数据库。

更多关于Ultrascan的信息，请参考[鲲鹏Gitcode代码仓](https://gitcode.com/boostkit/Ultrascan)。

## 环境要求

### 已验证环境

Ultrascan当前适配鲲鹏920新型号处理器，操作系统为openEuler 22.03 LTS SP4/openEuler 24.03 LTS SP3。若您在使用过程中遇到问题，请先检查使用的环境是否在已验证的环境范围内。

### 软件要求

安装编译前参见本章节提供的相关链接获取对应的软件包

软件要求如[**表 1** 软件要求](#软件要求)所示。

**表 1** 软件要求<a id="软件要求"></a>

|软件名称|版本|说明|获取方式|
|--|--|--|--|
|Git|系统软件源提供的版本|必选：获取Ultrascan源码。|使用Yum工具安装。|
|GCC|10.3及以上|必选。|-|
|CMake|2.8.11及以上|必选。|-|
|Ragel|6.9及以上|必选：编译依赖Ragel。|[获取链接](http://www.colm.net/files/ragel/ragel-6.10.tar.gz)|
|Boost|1.57及以上|必选：编译依赖Boost头文件。|[获取链接](https://archives.boost.io/release/1.87.0/source/boost_1_87_0.tar.gz)|
|PCRE|8.41及以上|可选：Ultrascan tools正确性验证工具hscollider编译依赖PCRE的8.41及以上版本。|[获取链接](https://sourceforge.net/projects/pcre/files/pcre/8.43/pcre-8.43.tar.gz)|
|SQLite|SQLite 3|可选：Ultrascan tools测试工具hsbench和hspgo编译依赖SQLite 3。|使用Yum工具安装。|
|Ultrascan|master|必选：待编译软件。|[获取链接](https://gitcode.com/boostkit/Ultrascan/tree/master)|

## 配置编译环境

### 获取项目代码

1. 安装Git。系统已安装Git时，可跳过此步骤。

    ```bash
    yum install -y git
    ```

2. 从GitCode代码仓克隆Ultrascan源码到`/opt/Ultrascan`。

    ```bash
    cd /opt
    git clone https://gitcode.com/boostkit/Ultrascan.git /opt/Ultrascan
    ```

    >![](public_sys-resources/icon-note.gif) **说明：**
    >执行克隆前，请确保`/opt`所在文件系统具有数GB可用空间，并且目标路径`/opt/Ultrascan`不存在或为空。如果该路径中已经存在完整的Ultrascan Git仓库，请不要重复克隆，可直接执行下一步切换分支。

3. 测试阶段切换到`dev_26_930`分支。

    ```bash
    git -C /opt/Ultrascan checkout dev_26_930
    ```

4. 确认当前分支。

    ```bash
    git -C /opt/Ultrascan branch --show-current
    ```

    正确输出：

    ```text
    dev_26_930
    ```

### 配置工作目录

本文统一将Ultrascan源码及相关文件放置在`/opt/Ultrascan`下，目录布局如下：

```text
/opt/Ultrascan/
├── build/           # 默认静态库构建目录
├── build-debug/     # Debug构建目录
├── build-shared/    # 动态库构建目录
├── build-feedback/  # 反馈优化构建目录
└── deps/            # 第三方依赖
```

首次使用时创建默认构建目录和依赖目录：

```bash
mkdir -p /opt/Ultrascan/build \
         /opt/Ultrascan/deps
```

`/opt`目录通常需要管理员权限创建。请确保执行下载、编译和测试的用户对上述目录具有读写权限。

执行编译前，使用以下命令确认`/opt/Ultrascan`所在文件系统具有足够的可用空间：

```bash
df -h /opt/Ultrascan
```

完整源码、第三方依赖及多个构建配置会占用数GB空间。若空间不足，请先为`/opt/Ultrascan`配置容量足够的文件系统，再继续后续步骤。

### （可选）配置本地源

>![](public_sys-resources/icon-note.gif) **说明：** 离线环境配置本地源，在线环境可以跳过这一步。

正确配置Yum源，以便于后续能够正常安装所需依赖包和软件。

1. 挂载系统镜像，本文以openEuler为例。

    ```bash
    mount YOUR_OS.iso /mnt -o loop
    ```

    >![](public_sys-resources/icon-note.gif) **说明：** 
    >**YOUR\_OS.iso**表示读者当前环境操作系统的镜像文件。

2. 配置Yum本地源。
    1. 创建并打开“/etc/yum.repos.d/openEuler.repo“文件。

        ```bash
        vim /etc/yum.repos.d/openEuler.repo
        ```

        >![](public_sys-resources/icon-note.gif) **说明：** 
        >openEuler.repo文件需要自行创建，建议备份原有repo文件。

    2. 按“i“键进入编辑模式，在openEuler.repo文件中添加如下内容：

        ```bash
        [openEuler]
        name=openEuler
        baseurl=file:///mnt
        enabled=1
        gpgcheck=0
        ```

    3. 按“Esc“键，输入`:wq!`并按“Enter“键保存并退出编辑。

3. 使Yum源配置生效。

    ```bash
    yum clean all
    yum makecache
    ```

### 安装Ragel

由于Ultrascan的编译依赖于Ragel，本文中编译环境采用Ragel 6.10版本。

1. 获取Ragel 6.10源码包。

    ```bash
    wget -P /opt/Ultrascan/deps \
        http://www.colm.net/files/ragel/ragel-6.10.tar.gz
    ```

    >![](public_sys-resources/icon-note.gif) **说明：** 
    >对于服务器无法连接外网的情况，可以将软件包下载到本地再上传到服务器。软件包下载地址请参见[软件要求](#软件要求)。

2. 解压源码包。

    ```bash
    tar -C /opt/Ultrascan/deps -xzf \
        /opt/Ultrascan/deps/ragel-6.10.tar.gz
    ```

3. 进入Ragel源码目录。

    ```bash
    cd /opt/Ultrascan/deps/ragel-6.10
    ```

4. 编译并安装Ragel。

    ```bash
    ./configure
    make
    make install
    ```

5. 通过检查Ragel版本验证Ragel是否安装成功。

    ```bash
    ragel -v
    ```

    显示“Ragel State Machine Compiler version 6.10 March 2017”则安装成功。

### 配置Boost

编译Ultrascan依赖于1.57及以上版本的Boost，本文编译环境采用的是Boost 1.87版本。提供两种配置Boost的方法，请根据实际情况选择。

- 第一种：下载软件包并建立软链接，使用该方法直接创建Boost软链接即可，不需要执行安装Boost命令。
- 第二种：下载软件包并安装，将Boost软件包安装到服务器中，不需要每次下载源码软链接Boost头文件，该安装步骤不需要建立软链接。

以下是两种配置方法的具体步骤：

**方法一：下载软件包并建立软链接**

1. 获取Boost 1.87源码包。

    ```bash
    wget -P /opt/Ultrascan/deps \
        https://archives.boost.io/release/1.87.0/source/boost_1_87_0.tar.gz
    ```

2. 解压源码。

    ```bash
    tar -C /opt/Ultrascan/deps -zxf \
        /opt/Ultrascan/deps/boost_1_87_0.tar.gz
    ```

3. 建立软链接。
    
    ```bash
    ln -s /opt/Ultrascan/deps/boost_1_87_0/boost \
        /opt/Ultrascan/include/boost
    ```

    >![](public_sys-resources/icon-note.gif) **说明：** 
    >编译依赖Boost头文件。以上命令将解压目录中的`boost`头文件目录链接到Ultrascan源码树。

**方法二：下载软件包并安装**

1. 获取软件包。

    ```bash
    wget -P /opt/Ultrascan/deps \
        https://archives.boost.io/release/1.87.0/source/boost_1_87_0.tar.gz
    ```

2. 解压软件包。

    ```bash
    tar -C /opt/Ultrascan/deps -zxf \
        /opt/Ultrascan/deps/boost_1_87_0.tar.gz
    ```

3. 进入解压缩后的目录。

    ```bash
    cd /opt/Ultrascan/deps/boost_1_87_0
    ```

4. 运行bootstrap.sh脚本并设置相关参数。

    ```bash
    ./bootstrap.sh --with-libraries=all --with-toolset=gcc
    ```

5. 执行编译。

    ```bash
    ./b2 toolset=gcc
    ```

6. 安装Boost。

    ```bash
    ./b2 install --prefix=/usr
    ```

    安装完成后，有类似如下提示则表示安装成功。

    ![](figures/zh-cn_image_0000002518785776.png)

7. 更新系统的动态链接库。

    ```bash
    ldconfig
    ```

### 下载PCRE

Ultrascan tools工具hscollider的编译依赖PCRE 8.41及以上版本，本文采用PCRE 8.43版本。

1. 获取PCRE 8.43源码包。

    ```bash
    wget -O /opt/Ultrascan/deps/pcre-8.43.tar.gz \
        https://sourceforge.net/projects/pcre/files/pcre/8.43/pcre-8.43.tar.gz/download
    ```

    >![](public_sys-resources/icon-note.gif) **说明：**
    >对于服务器无法连接外网的情况，可以通过[软件要求](#软件要求)中的链接下载源码包，再将其上传到`/opt/Ultrascan/deps/pcre-8.43.tar.gz`。

2. 解压源码。

    ```bash
    tar -C /opt/Ultrascan/deps -zxf \
        /opt/Ultrascan/deps/pcre-8.43.tar.gz
    ```

### 安装SQLite

Ultrascan tools工具hsbench和hspgo编译依赖SQLite 3，使用Yum命令安装SQLite及SQLite开发套件，安装完成后进行版本验证。

1. 安装SQLite及开发套件。

    ```bash
    yum install -y sqlite sqlite-devel
    ```

2. 安装完成后使用以下命令验证开发套件是否配置成功。

    ```bash
    pkg-config --libs sqlite3
    ```

    - 如果执行成功，则回显内容如下所示。

        ```bash
        -lsqlite3
        ```

    - 如果执行失败，则需要按以下步骤操作。
        1. 打开“/usr/lib64/pkgconfig/sqlite3.pc“文件。

            ```bash
            vim /usr/lib64/pkgconfig/sqlite3.pc
            ```

        2. 按“i“键进入编辑模式，在“/usr/lib64/pkgconfig/sqlite3.pc“文件中添加如下内容。

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

            >![](public_sys-resources/icon-note.gif) **说明：** 
            >其中libdir、includedir等以实际安装路径进行设置。

        3. 按“Esc“键，输入`:wq!`并按“Enter“键保存并退出编辑。

## 编译Ultrascan

在Ultrascan源码目录下添加PCRE依赖库，最后进行源码静态库、动态库或Debug模式的编译。

1. 确认Ultrascan源码已放置在`/opt/Ultrascan`。

2. 添加PCRE依赖库。

    源码hscollider工具编译依赖于PCRE工具。

    1. 切换到pcre-8.43下载目录，将解压后的pcre-8.43文件夹拷贝到Ultrascan源码目录下并重命名为pcre文件夹。

        ```bash
        cp -rf /opt/Ultrascan/deps/pcre-8.43 \
            /opt/Ultrascan/pcre
        ```

    2. 打开“pcre/CMakeLists.txt“文件。

        ```bash
        vim /opt/Ultrascan/pcre/CMakeLists.txt
        ```

    3. 按“i“键进入编辑模式，在拷贝后的“pcre/CMakeLists.txt“文件中找到`CMAKE_POLICY(SET CMP0026 OLD)`并将其注释掉，如下所示。

        ```bash
        CMAKE_MINIMUM_REQUIRED(VERSION 2.8.0)
        #CMAKE_POLICY(SET CMP0026 OLD)
        ```

        新版本CMake已移除`CMP0026`策略的`OLD`行为，因此需要将该命令注释掉；它不影响Ultrascan所需的PCRE功能。

    4. 按“Esc“键，输入`:wq!`并按“Enter“键保存并退出编辑。

3. 编译源码。

    编译支持Release、Debug、动态库和反馈优化等配置。以下方式按需选择；需要同时保留多种构建结果时，使用各自独立的构建目录，避免CMake缓存中的选项相互影响。

    - 编译默认Release静态库。

        ```bash
        mkdir -p /opt/Ultrascan/build
        cd /opt/Ultrascan/build
        cmake ..
        make -j
        ```

        默认Release配置不生成`hsdump`。如需按照[快速入门](./quick_start.md)生成通用字节码，请使用下面的Debug模式完成构建。

    - （可选）编译Debug模式。

        在鲲鹏计算平台执行：

        ```bash
        mkdir -p /opt/Ultrascan/build-debug
        cd /opt/Ultrascan/build-debug
        cmake .. -DCMAKE_BUILD_TYPE=DEBUG
        make -j
        ```

        Debug模式会生成`/opt/Ultrascan/build-debug/bin/hsdump`。

        在x86平台执行：

        ```bash
        mkdir -p /opt/Ultrascan/build-debug
        cd /opt/Ultrascan/build-debug
        cmake .. -DCMAKE_BUILD_TYPE=DEBUG \
            -DCMAKE_C_FLAGS="-D__X86_64__" \
            -DCMAKE_CXX_FLAGS="-D__X86_64__"
        make -j
        ```

        编译完成后，默认生成Ultrascan的静态库和测试程序：

        ![](figures/zh-cn_image_0000002550305603.png)

        编译完成生成的测试程序：

        ![](figures/1.png)

        如果开启debug模式，则产生的测试程序如下：

        ![](figures/6.png)

        生成的静态库：

        ![](figures/2.png)

    - 编译动态库。

        ```bash
        mkdir -p /opt/Ultrascan/build-shared
        cd /opt/Ultrascan/build-shared
        cmake .. -DBUILD_SHARED_LIBS=ON
        make -j
        ```

        生成的动态库：

        ![](figures/3.png)

    - （可选）编译正则匹配反馈优化技术。该技术当前仅支持AArch64且默认关闭：

        ```bash
        mkdir -p /opt/Ultrascan/build-feedback
        cd /opt/Ultrascan/build-feedback
        cmake .. -DHS_ENABLE_FP_FEEDBACK=ON
        make -j
        ```

        该选项可与`CMAKE_BUILD_TYPE`、`BUILD_SHARED_LIBS`等现有选项组合使用。使用时注意：

        - 在x86等非AArch64平台设置`HS_ENABLE_FP_FEEDBACK=ON`会在CMake配置阶段报错。
        - 不设置该选项时，库仍导出反馈相关公共符号，但功能调用返回`HS_ARCH_ERROR`。
        - `hspgo`依赖SQLite 3。已安装SQLite且反馈能力开启时，会生成`/opt/Ultrascan/build-feedback/bin/hspgo`；缺少SQLite时库仍可构建，但不会生成该工具。
        - 应用通过API集成时，请参考[正则匹配反馈优化技术API](./api_reference.md#4-正则匹配反馈优化技术api)。
