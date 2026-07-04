# Developer Guide

## Function Description

>![](public_sys-resources/icon-notice.gif) **NOTICE:**
>Functions (KHSEL_*xxx*) in KHSEL do not need to be explicitly called. They are used as internal interfaces of Ultrascan.

[**Table 1**](#optimized-functions-in-khsel) lists the optimized functions in KHSEL.

**Table 1** Optimized functions in KHSEL<a id="optimized-functions-in-khsel"></a>

|Function|Description|
|--|--|
|KHSEL_BuildLily|A new Ultrascan function that compiles short-rule (single byte) matching.|
|KHSEL_LilyRunExec|A new Ultrascan function that executes short-rule (single byte) matching.|
|KHSEL_BuildLilyForTeddy|A new Ultrascan function that compiles short-rule (2–4 bytes) matching.|
|KHSEL_LilyForTeddyRunExec|A new Ultrascan function that executes short-rule (2–4 bytes) matching.|

[**Table 2**](#universal-bytecode-function-apis) lists the universal bytecode function APIs. 

**Table 2** Universal bytecode function APIs<a id="universal-bytecode-function-apis"></a>

|Function|Description|
|--|--|
|fat_hs_compile|A new Ultrascan function that compiles a single regular expression into universal bytecode.|
|fat_hs_compile_multi|A new Ultrascan function that batch compiles multiple regular expressions into universal bytecode.|
|fat_hs_compile_ext_multi|A new extended Ultrascan function that batch compiles multiple regular expressions into universal bytecode.|
|fat_hs_compile_lit|A new Ultrascan function that compiles the expression represented by a single-byte literal into universal bytecode.|
|fat_hs_compile_lit_multi|A new Ultrascan function that batch compiles multiple expressions represented by single-byte literals into universal bytecode.|

## Usage Description

The KHSEL function source code has been integrated into the `dev` branch of the Ultrascan repository and is stored in the `src\kunpeng-enhanced` directory. Therefore, you do not need to install the KHSEL software packages separately.

## Function Definition

### KHSEL\_BuildLily

**Function Usage**

Compiles single-byte rules in the rule set and outputs a compiled mask.

**Function Syntax**

```c
std::vector<u8> KHSEL_BuildLily(std::map<char, lilyReport> &lily, std::vector<u32> &reportVec, std::vector<u32> &ekeyVec); 
```

**Parameters**

|Parameter|Description|Value Range|Input/Output|
|--|--|--|--|
|lily|Single-byte rule.|C++ map object, which can be empty.|Input|
|reportVec|Report ID corresponding to the single-byte rule.|C++ vector object, which can be empty.|Input|
|ekeyVec|ekey corresponding to the single-byte rule.|C++ vector object, which can be empty.|Input|

**Return Value**

Output mask after rule compilation.

### KHSEL\_LilyRunExec<a name="EN-US_TOPIC_0000002518245632"></a>

**Function Usage**

Performs input data matching at runtime based on the output mask.

**Function Syntax**

```c
hs_error_t KHSEL_LilyRunExec(const struct RoseEngine *rose, hs_scratch_t *scratch); 
```

**Parameters**

|Parameter|Description|Value Range|Input/Output|
|--|--|--|--|
|rose|RoseEngine object, which stores the output at compile time and is used as the input at runtime.|Not empty|Input|
|scratch|Temporary memory space required for input data.|Not empty|Input|

**Return Value**

Error code of the matching result.

### KHSEL\_BuildLilyForTeddy

**Function Usage**

Compiles 2-to-4-byte rules in the rule set and outputs a compiled mask.

**Function Syntax**

```c
ue2::bytecode_ptr<lilyTeddy> KHSEL_BuildLilyForTeddy(std::map<std::string, lilyReport> &lilyForTeddy,
                                        std::priority_queue<LilyForTeddyPair, std::vector<LilyForTeddyPair>, CompareStringLength> &lilyForTeddyPQ,
                                        std::vector<u32> &reportVec, std::vector<u32> &ekeyVec, std::vector<u32> &lenVec);
```

**Parameters**

|Parameter| Description                  | Value Range                      |Input/Output|
|--|----------------------|----------------------------|--|
|lilyForTeddy| 2-to-4-byte rule.            | C++ map object, which can be empty.           |Input|
|lilyForTeddyPQ| Priority queue of the 2-to-4-byte rules sorted by rule length.| C++ priority_queue object, which can be empty.|Input|
|reportVec| Report ID corresponding to the 2-to-4-byte rule. | C++ vector object, which can be empty.        |Input|
|ekeyVec| ekey corresponding to the 2-to-4-byte rule.     | C++ vector object, which can be empty.        |Input|
|lenVec| Length of the 2-to-4-byte rule.     | C++ vector object, which can be empty.        |Input|

**Return Value**

Output mask after rule compilation.

### KHSEL\_LilyForTeddyRunExec

**Function Usage**

Performs input data matching at runtime based on the output mask.

**Function Syntax**

```c
hs_error_t KHSEL_LilyForTeddyRunExec(const struct RoseEngine *rose, hs_scratch_t *scratch);
```

**Parameters**

|Parameter|Description|Value Range|Input/Output|
|--|--|--|--|
|rose|RoseEngine object, which stores the output at compile time and is used as the input at runtime.|Not empty|Input|
|scratch|Temporary memory space required for input data.|Not empty|Input|

**Return Value**

Error code of the matching result.

### fat\_hs\_compile

**Function Usage**

Compiles a single regular expression to generate a database that contains x86 and Arm bytecode.

**Function Syntax**

```c
hs_error_t fat_hs_compile(const char *expression, unsigned int flags,
                          unsigned int mode,
                          const hs_platform_info_t *platform,
                          fat_hs_database_t **db,
                          hs_compile_error_t **error);
```

**Parameters**

|Parameter|Description|Value Range|Input/Output|
|--|--|--|--|
|expression|Regular expression string, ending with null.|Non-empty character string|Input|
|flags|Expression compilation flag. Multiple flags can be combined using the OR operation.|<code>HS_FLAG_CASELESS</code>, <code>HS_FLAG_DOTALL</code>, <code>HS_FLAG_MULTILINE</code>, etc.|Input|
|mode|Compilation mode, which specifies the database type.|<code>HS_MODE_STREAM</code>, <code>HS_MODE_BLOCK</code>, or <code>HS_MODE_VECTORED</code>|Input|
|platform|Target platform information. The null value indicates the current host platform.|The value can be null.|Input|
|db|Returned pointer to the general database after successful compilation.|Non-null pointer|Output|
|error|Error information returned when the compilation fails.|Non-null pointer|Output|

**Return Value**

If the operation is successful, `HS_SUCCESS` is returned. If the operation fails, `HS_COMPILER_ERROR` is returned and the error details are provided in the `error` parameter.

### fat\_hs\_compile\_multi<a name="EN-US_TOPIC_fat_hs_compile_multi"></a>

**Function Usage**

Batch compiles multiple regular expressions to generate a database that contains x86 and Arm bytecode.

**Function Syntax**

```c
hs_error_t fat_hs_compile_multi(const char *const *expressions,
                                const unsigned int *flags,
                                const unsigned int *ids,
                                unsigned int elements, unsigned int mode,
                                const hs_platform_info_t *platform,
                                fat_hs_database_t **db,
                                hs_compile_error_t **error);
```

**Parameters**

|Parameter|Description|Value Range|Input/Output|
|--|--|--|--|
|expressions|Regular expression string array, with each element ending with null.|Non-empty array|Input|
|flags|Array of compilation flags corresponding to each expression.|The value can be null. If the value is null, all flags are <code>0</code>.|Input|
|ids|Array of IDs corresponding to each expression.|The value can be null. If the value is null, all IDs are <code>0</code>.|Input|
|elements|Number of elements in the expression array.|> 0|Input|
|mode|Compilation mode, which specifies the database type.|<code>HS_MODE_STREAM</code>, <code>HS_MODE_BLOCK</code>, or <code>HS_MODE_VECTORED</code>|Input|
|platform|Target platform information. The null value indicates the current host platform.|The value can be null.|Input|
|db|Returned pointer to the general database after successful compilation.|Non-null pointer|Output|
|error|Error information returned when the compilation fails.|Non-null pointer|Output|

**Return Value**

If the operation is successful, `HS_SUCCESS` is returned. If the operation fails, `HS_COMPILER_ERROR` is returned and the error details are provided in the `error` parameter.

### fat\_hs\_compile\_ext\_multi

**Function Usage**

With support for extended parameters, batch compiles multiple regular expressions to generate a database that contains x86 and Arm bytecode.

**Function Syntax**

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

**Parameters**

|Parameter|Description|Value Range|Input/Output|
|--|--|--|--|
|expressions|Regular expression string array, with each element ending with null.|Non-empty array|Input|
|flags|Array of compilation flags corresponding to each expression.|The value can be null. If the value is null, all flags are <code>0</code>.|Input|
|ids|Array of IDs corresponding to each expression.|The value can be null. If the value is null, all IDs are <code>0</code>.|Input|
|ext|Array of pointers to the extended parameter structures corresponding to each expression, which is used to set extended attributes such as the minimum and maximum matching lengths.|The value can be null.|Input|
|elements|Number of elements in the expression array.|> 0|Input|
|mode|Compilation mode, which specifies the database type.|<code>HS_MODE_STREAM</code>, <code>HS_MODE_BLOCK</code>, or <code>HS_MODE_VECTORED</code>|Input|
|platform|Target platform information. The null value indicates the current host platform.|The value can be null.|Input|
|db|Returned pointer to the general database after successful compilation.|Non-null pointer|Output|
|error|Error information returned when the compilation fails.|Non-null pointer|Output|

**Return Value**

If the operation is successful, `HS_SUCCESS` is returned. If the operation fails, `HS_COMPILER_ERROR` is returned and the error details are provided in the `error` parameter.

### fat\_hs\_compile\_lit

**Function Usage**

Compiles a single literal expression (not a regular expression) to generate a database that contains x86 and Arm bytecode. All characters in the literal expression are matched literally and are not parsed by the regular expression syntax.

**Function Syntax**

```c
hs_error_t fat_hs_compile_lit(const char *expression, unsigned int flags,
                              const size_t len, unsigned int mode,
                              const hs_platform_info_t *platform,
                              fat_hs_database_t **db,
                              hs_compile_error_t **error);
```

**Parameters**

|Parameter|Description|Value Range|Input/Output|
|--|--|--|--|
|expression|Literal expression string.|Non-empty character string|Input|
|flags|Expression compilation flag.|<code>HS_FLAG_CASELESS</code>, <code>HS_FLAG_SINGLEMATCH</code>, or <code>HS_FLAG_SOM_LEFTMOST</code>|Input|
|len|Length (in bytes) of the literal expression. The expression can contain <code>\0</code>.|> 0|Input|
|mode|Compilation mode, which specifies the database type.|<code>HS_MODE_STREAM</code>, <code>HS_MODE_BLOCK</code>, or <code>HS_MODE_VECTORED</code>|Input|
|platform|Target platform information. The null value indicates the current host platform.|The value can be null.|Input|
|db|Returned pointer to the general database after successful compilation.|Non-null pointer|Output|
|error|Error information returned when the compilation fails.|Non-null pointer|Output|

**Return Value**

If the operation is successful, `HS_SUCCESS` is returned. If the operation fails, `HS_COMPILER_ERROR` is returned and the error details are provided in the `error` parameter.

### fat\_hs\_compile\_lit\_multi

**Function Usage**

Batch compiles multiple literal expressions (not regular expressions) to generate a database that contains x86 and Arm bytecode. All characters in the literal expressions are matched literally and are not parsed by the regular expression syntax.

**Function Syntax**

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

**Parameters**

|Parameter|Description|Value Range|Input/Output|
|--|--|--|--|
|expressions|Array of literal expression strings.|Non-empty array|Input|
|flags|Array of compilation flags corresponding to each expression.|The value can be null. If the value is null, all flags are <code>0</code>.|Input|
|ids|Array of IDs corresponding to each expression.|The value can be null. If the value is null, all IDs are <code>0</code>.|Input|
|lens|Array of lengths (in bytes) for each literal expression. The expressions can contain <code>\0</code>.|Non-empty array|Input|
|elements|Number of elements in the expression array.|> 0|Input|
|mode|Compilation mode, which specifies the database type.|<code>HS_MODE_STREAM</code>, <code>HS_MODE_BLOCK</code>, or <code>HS_MODE_VECTORED</code>|Input|
|platform|Target platform information. The null value indicates the current host platform.|The value can be null.|Input|
|db|Returned pointer to the general database after successful compilation.|Non-null pointer|Output|
|error|Error information returned when the compilation fails.|Non-null pointer|Output|

**Return Value**

If the operation is successful, `HS_SUCCESS` is returned. If the operation fails, `HS_COMPILER_ERROR` is returned and the error details are provided in the `error` parameter.

## Change History

|Issue|Date|Description|
|--|--|--|
|02|2026-06-30|This issue is the second official release. Added the universal bytecode function based on the new Kunpeng 920 processor model.|
|01|2026-03-30|This issue is the first official release. Optimized the Ultrascan short-byte (2–4 bytes) rule matching algorithm based on the new Kunpeng 920 processor model, and added the KHSEL_BuildLilyForTeddy and KHSEL_LilyForTeddyRunExec algorithms.|
