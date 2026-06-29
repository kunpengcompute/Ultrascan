# 开发指南

## 函数说明

>![](public_sys-resources/icon-notice.gif) **须知：** 
>KHSEL中的函数（KHSEL\_xxx）无需被显式调用，它们作为Ultrascan的内部接口使用。

KHSEL库已优化函数如[**表 1** KHSEL库已优化函数](#KHSEL库已优化函数)所示。

**表 1** KHSEL库已优化函数<a id="KHSEL库已优化函数"></a>

|名称|说明|
|--|--|
|KHSEL_BuildLily|Ultrascan新增单字节短规则匹配的编译函数。|
|KHSEL_LilyRunExec|Ultrascan新增单字节短规则匹配的运行函数。|
|KHSEL_BuildLilyForTeddy|Ultrascan新增2~4字节短规则匹配的编译函数。|
|KHSEL_LilyForTeddyRunExec|Ultrascan新增2~4字节短规则匹配的运行函数。|

通用字节码功能接口[**表 2** 通用字节码功能接口](#通用字节码功能接口)所示。  

**表 2** 通用字节码功能接口<a id="通用字节码功能接口"></a>

|名称|说明|
|--|--|
|fat_hs_compile|Ultrascan新增通用字节码编译函数，编译单个正则表达式。|
|fat_hs_compile_multi|Ultrascan新增通用字节码批量编译函数，编译多个正则表达式。|
|fat_hs_compile_ext_multi|Ultrascan新增通用字节码扩展批量编译函数，编译多个正则表达式。|
|fat_hs_compile_lit|Ultrascan新增通用字节码单字节字面量编译函数，编译单个单字节字面量正则表达式。|
|fat_hs_compile_lit_multi|Ultrascan新增通用字节码批量单字节字面量编译函数，编译多个单字节字面量正则表达式。|

## 使用说明

KHSEL函数源码已集成到Ultrascan仓库dev分支中，位于`src\kunpeng-enhanced`目录下，不再需要单独安装KHSEL相关软件包。HAO算法引擎是基于并行位提取的多模匹配加速算法，源码位于`src\fdr`目录下，作为FDR内部匹配器自动接入编译期和运行期流程，用户无需显式调用HAO内部接口。

## 函数定义

### KHSEL\_BuildLily

**函数功能**

基于规则集内的单字节短规则进行规则编译，输出编译后的mask。

**函数定义**

```c
std::vector<u8> KHSEL_BuildLily(std::map<char, lilyReport> &lily, std::vector<u32> &reportVec, std::vector<u32> &ekeyVec); 
```

**参数说明**

|参数名|描述|取值范围|输入/输出|
|--|--|--|--|
|lily|单字节短规则。|C++ map对象，无非空限制|输入|
|reportVec|单字节短规则对应的reportID。|C++ vector对象，无非空限制|输入|
|ekeyVec|单字节短规则对应的ekey。|C++ vector对象，无非空限制|输入|

**返回值**

规则编译后的输出。

### KHSEL\_LilyRunExec<a name="ZH-CN_TOPIC_0000002518245632"></a>

**函数功能**

基于编译输出的mask，进行运行期输入数据的匹配。

**函数定义**

```c
hs_error_t KHSEL_LilyRunExec(const struct RoseEngine *rose, hs_scratch_t *scratch); 
```

**参数说明**

|参数名|描述|取值范围|输入/输出|
|--|--|--|--|
|rose|RoseEngine对象，保存编译期输出结果，作为运行期输入。|非空|输入|
|scratch|输入数据所需的临时内存空间。|非空|输入|

**返回值**

匹配结果的错误码。

### KHSEL\_BuildLilyForTeddy

**函数功能**

基于规则集内的2~4字节短规则进行规则编译，输出编译后的mask。

**函数定义**

```c
ue2::bytecode_ptr<lilyTeddy> KHSEL_BuildLilyForTeddy(std::map<std::string, lilyReport> &lilyForTeddy,
                                        std::priority_queue<LilyForTeddyPair, std::vector<LilyForTeddyPair>, CompareStringLength> &lilyForTeddyPQ,
                                        std::vector<u32> &reportVec, std::vector<u32> &ekeyVec, std::vector<u32> &lenVec);
```

**参数说明**

|参数名| 描述                   | 取值范围                       |输入/输出|
|--|----------------------|----------------------------|--|
|lilyForTeddy| 2~4字节规则。             | C++ map对象，无非空限制            |输入|
|lilyForTeddyPQ| 2~4字节规则依规则长度排序的优先队列。 | C++ priority_queue对象，无非空限制 |输入|
|reportVec| 2~4字节规则对应的reportID。  | C++ vector对象，无非空限制         |输入|
|ekeyVec| 2~4字节规则对应的ekey。      | C++ vector对象，无非空限制         |输入|
|lenVec| 2~4字节规则对应的规则长度。      | C++ vector对象，无非空限制         |输入|

**返回值**

规则编译后的输出。

### KHSEL\_LilyForTeddyRunExec

**函数功能**

基于编译输出的mask，进行运行期输入数据的匹配。

**函数定义**

```c
hs_error_t KHSEL_LilyForTeddyRunExec(const struct RoseEngine *rose, hs_scratch_t *scratch);
```

**参数说明**

|参数名|描述|取值范围|输入/输出|
|--|--|--|--|
|rose|RoseEngine对象，保存编译期输出结果，作为运行期输入。|非空|输入|
|scratch|输入数据所需的临时内存空间。|非空|输入|

**返回值**

匹配结果的错误码。

### fat\_hs\_compile

**函数功能**

编译单个正则表达式，生成包含x86和ARM双架构字节码的通用数据库。

**函数定义**

```c
hs_error_t fat_hs_compile(const char *expression, unsigned int flags,
                          unsigned int mode,
                          const hs_platform_info_t *platform,
                          fat_hs_database_t **db,
                          hs_compile_error_t **error);
```

**参数说明**

|参数名|描述|取值范围|输入/输出|
|--|--|--|--|
|expression|正则表达式字符串，以NULL结尾。|非空字符串|输入|
|flags|表达式编译标志位，多个标志可通过OR运算组合。|HS_FLAG_CASELESS、HS_FLAG_DOTALL、HS_FLAG_MULTILINE等|输入|
|mode|编译模式，指定数据库类型。|HS_MODE_STREAM、HS_MODE_BLOCK、HS_MODE_VECTORED之一|输入|
|platform|目标平台信息，NULL表示当前主机平台。|可为NULL|输入|
|db|编译成功后返回的通用数据库指针。|非空指针|输出|
|error|编译失败时返回的错误信息。|非空指针|输出|

**返回值**

成功返回HS_SUCCESS，失败返回HS_COMPILER_ERROR并在error参数中提供错误详情。

### fat\_hs\_compile\_multi<a name="ZH-CN_TOPIC_fat_hs_compile_multi"></a>

**函数功能**

批量编译多个正则表达式，生成包含x86和ARM双架构字节码的通用数据库。

**函数定义**

```c
hs_error_t fat_hs_compile_multi(const char *const *expressions,
                                const unsigned int *flags,
                                const unsigned int *ids,
                                unsigned int elements, unsigned int mode,
                                const hs_platform_info_t *platform,
                                fat_hs_database_t **db,
                                hs_compile_error_t **error);
```

**参数说明**

|参数名|描述|取值范围|输入/输出|
|--|--|--|--|
|expressions|正则表达式字符串数组，每个元素以NULL结尾。|非空数组|输入|
|flags|每个表达式对应的编译标志位数组。|可为NULL，NULL时所有标志为0|输入|
|ids|每个表达式对应的ID数组。|可为NULL，NULL时所有ID为0|输入|
|elements|表达式数组元素个数。|大于0|输入|
|mode|编译模式，指定数据库类型。|HS_MODE_STREAM、HS_MODE_BLOCK、HS_MODE_VECTORED之一|输入|
|platform|目标平台信息，NULL表示当前主机平台。|可为NULL|输入|
|db|编译成功后返回的通用数据库指针。|非空指针|输出|
|error|编译失败时返回的错误信息。|非空指针|输出|

**返回值**

成功返回HS_SUCCESS，失败返回HS_COMPILER_ERROR并在error参数中提供错误详情。

### fat\_hs\_compile\_ext\_multi

**函数功能**

批量编译多个正则表达式，支持扩展参数，生成包含x86和ARM双架构字节码的通用数据库。

**函数定义**

```c
hs_error_t fat_hs_compile_ext_multi(const char *const *expressions,
                                    const unsigned int *flags,
                                    const unsigned int *ids,
                                    const hs_expr_ext_t *const *ext,
                                    unsigned int elements, unsigned int mode,
                                    const hs_platform_info_t *platform,
                                    fat_hs_database_t **db,
                                    hs_compile_error_t **error);
```

**参数说明**

|参数名|描述|取值范围|输入/输出|
|--|--|--|--|
|expressions|正则表达式字符串数组，每个元素以NULL结尾。|非空数组|输入|
|flags|每个表达式对应的编译标志位数组。|可为NULL，NULL时所有标志为0|输入|
|ids|每个表达式对应的ID数组。|可为NULL，NULL时所有ID为0|输入|
|ext|每个表达式对应的扩展参数结构体指针数组，用于设置最小/最大匹配长度等扩展属性。|可为NULL|输入|
|elements|表达式数组元素个数。|大于0|输入|
|mode|编译模式，指定数据库类型。|HS_MODE_STREAM、HS_MODE_BLOCK、HS_MODE_VECTORED之一|输入|
|platform|目标平台信息，NULL表示当前主机平台。|可为NULL|输入|
|db|编译成功后返回的通用数据库指针。|非空指针|输出|
|error|编译失败时返回的错误信息。|非空指针|输出|

**返回值**

成功返回HS_SUCCESS，失败返回HS_COMPILER_ERROR并在error参数中提供错误详情。

### fat\_hs\_compile\_lit

**函数功能**

编译单个纯字面量表达式（非正则表达式），生成包含x86和ARM双架构字节码的通用数据库。字面量表达式中的所有字符均按字面意义匹配，不解析正则语法。

**函数定义**

```c
hs_error_t fat_hs_compile_lit(const char *expression, unsigned int flags,
                              const size_t len, unsigned int mode,
                              const hs_platform_info_t *platform,
                              fat_hs_database_t **db,
                              hs_compile_error_t **error);
```

**参数说明**

|参数名|描述|取值范围|输入/输出|
|--|--|--|--|
|expression|纯字面量表达式字符串。|非空字符串|输入|
|flags|表达式编译标志位。|HS_FLAG_CASELESS、HS_FLAG_SINGLEMATCH、HS_FLAG_SOM_LEFTMOST|输入|
|len|字面量表达式的长度（字节数），允许表达式中包含`\0`字符。|大于0|输入|
|mode|编译模式，指定数据库类型。|HS_MODE_STREAM、HS_MODE_BLOCK、HS_MODE_VECTORED之一|输入|
|platform|目标平台信息，NULL表示当前主机平台。|可为NULL|输入|
|db|编译成功后返回的通用数据库指针。|非空指针|输出|
|error|编译失败时返回的错误信息。|非空指针|输出|

**返回值**

成功返回HS_SUCCESS，失败返回HS_COMPILER_ERROR并在error参数中提供错误详情。

### fat\_hs\_compile\_lit\_multi

**函数功能**

批量编译多个纯字面量表达式（非正则表达式），生成包含x86和ARM双架构字节码的通用数据库。字面量表达式中的所有字符均按字面意义匹配，不解析正则语法。

**函数定义**

```c
hs_error_t fat_hs_compile_lit_multi(const char *const *expressions,
                                    const unsigned int *flags,
                                    const unsigned int *ids,
                                    const size_t *lens,
                                    unsigned int elements, unsigned int mode,
                                    const hs_platform_info_t *platform,
                                    fat_hs_database_t **db,
                                    hs_compile_error_t **error);
```

**参数说明**

|参数名|描述|取值范围|输入/输出|
|--|--|--|--|
|expressions|纯字面量表达式字符串数组。|非空数组|输入|
|flags|每个表达式对应的编译标志位数组。|可为NULL，NULL时所有标志为0|输入|
|ids|每个表达式对应的ID数组。|可为NULL，NULL时所有ID为0|输入|
|lens|每个字面量表达式的长度数组（字节数），允许表达式中包含`\0`字符。|非空数组|输入|
|elements|表达式数组元素个数。|大于0|输入|
|mode|编译模式，指定数据库类型。|HS_MODE_STREAM、HS_MODE_BLOCK、HS_MODE_VECTORED之一|输入|
|platform|目标平台信息，NULL表示当前主机平台。|可为NULL|输入|
|db|编译成功后返回的通用数据库指针。|非空指针|输出|
|error|编译失败时返回的错误信息。|非空指针|输出|

**返回值**

成功返回HS_SUCCESS，失败返回HS_COMPILER_ERROR并在error参数中提供错误详情。

## 修订记录

|文档版本|发布日期|修改说明|
|--|--|--|
|02|2026-06-30|第二次正式发布，基于鲲鹏920新型号处理器新增通用字节码功能和HAO算法引擎。|
|01|2026-03-30|第一次正式发布，基于鲲鹏920新型号处理器优化Ultrascan 2~4字节短规则匹配算法，新增KHSEL_BuildLilyForTeddy、KHSEL_LilyForTeddyRunExec算法。|
