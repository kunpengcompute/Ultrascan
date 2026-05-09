# Developer Guide<a name="EN-US_TOPIC_0000002549765393"></a>

## Function Description<a name="EN-US_TOPIC_0000002549885353"></a>

>![](public_sys-resources/icon-notice.gif) **NOTICE:** 
>Functions \(KHSEL\__xxx_\) in the KHSEL\_core library do not need to be explicitly called. They are used as internal interfaces of Hyperscan.

[Table 1](#table_1)  lists the optimized function in the KHSEL\_ops library.  [Table 2](#table_2)  lists the optimized functions in the KHSEL\_core library.

**Table  1**  Optimized function in KHSEL\_ops<a id="table_1"></a>

|Function|Description|
|--|--|
|ReplaceAllAcc|Performs string match and Replace All operations according to the fixed regular expression rule "[^A-Za-z0-9_/.]+".|


**Table  2**  Optimized functions in KHSEL\_core<a id="table_2"></a>

|Function|Description|
|--|--|
|KHSEL_BuildLily|Compiles short-byte rule matching.|
|KHSEL_LilyRunExec|Executes short-byte rule matching.|



## Usage Description<a name="EN-US_TOPIC_0000002518405558"></a>

**KHSEL\_ops<a name="section157951035849"></a>**

Link  **libKHSEL\_ops.a**  using  **-L**  and  **-l**  during g++ compilation.

```
g++ test.cpp -o test -I/usr/local/ksl/include -L/usr/local/ksl/lib -lKHSEL_ops
```

**KHSEL\_core<a name="section121472055548"></a>**

The KHSEL\_core library functions need to be compiled with Hyperscan. For details, see \[compile\_guide.md\]\(./docs/en/compile\_guide.md\).


## Function Syntax<a name="EN-US_TOPIC_0000002549885351"></a>

### ReplaceAllAcc<a name="EN-US_TOPIC_0000002518245588"></a>

**Function Usage<a name="section95941732195012"></a>**

Performs string match and Replace All operations according to the fixed regular expression rule "\[^A-Za-z0-9\_/.\]+" based on the shufti algorithm.

**Function Syntax<a name="section1183110404506"></a>**

```
std::string ReplaceAllAcc(const std::string& input, const std::string& replacement) 
```

**Parameters<a name="section1192224915509"></a>**

|Parameter|Description|Value Range|Input/Output|
|--|--|--|--|
|input|Input string (string to be replaced)|C++ string object, which can be null.|Input|
|replacement|Replacement string|C++ string object, which can be null.|Input|


**Return Value<a name="section15720131195318"></a>**

String after the Replace All operation is performed.

>![](public_sys-resources/icon-notice.gif) **NOTICE:** 
>The input string is not modified.

**Example<a name="section518716218534"></a>**

1. Create a  **testReplaceAll.cpp**  file.
2. Press  **i**  to enter the insert mode and add the following content to the file:

    ```
    #include "khsel_ops.h"
    #include <iostream>
    
    int main() {
        std::string input = "Hello*#$&@Hello12345!()";
        std::string replacement = "hi";
        std::cout << ReplaceAllAcc(input, replacement) << std::endl;
        return 0;
    }
    ```

3. Press  **Esc**, type  **:wq!**, and press  **Enter**  to save the file and exit.
4. Compile the  **testReplaceAll.cpp**  file and specify the name of the output executable file as  **testReplaceAll**.

    ```
    g++ testReplaceAll.cpp -o testReplaceAll -I /usr/local/ksl/include -L /usr/local/ksl/lib -lKHSEL_ops
    ```

5. Run the  **testReplaceAll**  executable file.

    ```
    ./testReplaceAll
    ```

    The execution result is as follows:

    ```
    HellohiHello12345hi
    ```


### KHSEL\_BuildLily<a name="EN-US_TOPIC_0000002518245592"></a>

**Function Usage<a name="section95941732195012"></a>**

Performs rule compilation based on short-byte rules in the rule set and outputs a compiled mask.

**Function Syntax<a name="section1183110404506"></a>**

```
std::vector<u8> KHSEL_BuildLily(std::map<char, lilyReport> &lily, std::vector<u32> &reportVec, std::vector<u32> &ekeyVec); 
```

**Parameters<a name="section1192224915509"></a>**

|Parameter|Description|Value Range|Input/Output|
|--|--|--|--|
|lily|Short-byte rule.|C++ map object, which can be null.|Input|
|reportVec|Report ID corresponding to the short-byte rule.|C++ vector object, which can be null.|Input|
|ekeyVec|ekey corresponding to the short-byte rule.|C++ vector object, which can be null.|Input|


**Return Value<a name="section13615359181110"></a>**

Output mask after rule compilation.


### KHSEL\_LilyRunExec<a name="EN-US_TOPIC_0000002518245632"></a>

**Function Usage<a name="section95941732195012"></a>**

Performs input data matching at runtime based on the output mask.

**Function Syntax<a name="section1183110404506"></a>**

```
hs_error_t KHSEL_LilyRunExec(const struct RoseEngine *rose, hs_scratch_t *scratch); 
```

**Parameters<a name="section1192224915509"></a>**

|Parameter|Description|Value Range|Input/Output|
|--|--|--|--|
|rose|RoseEngine object, which stores the output at compile time and is used as the input at runtime.|Not null|Input|
|scratch|Temporary memory space required for input data.|Not null|Input|


**Return Value<a name="section13615359181110"></a>**

Error code of the matching result.



