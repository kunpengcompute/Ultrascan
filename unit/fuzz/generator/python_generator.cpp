#include "../fuzz_test.h"
#include "gtest/gtest.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <vector>

namespace {

bool fuzzVerbose() {
    const char *value = std::getenv("HS_FUZZ_VERBOSE");
    return value && value[0] != '\0' && value[0] != '0';
}

bool fileExists(const std::string &path) {
    std::ifstream file(path.c_str());
    return file.good();
}

// Generator paths are selected locally, but quoting them also makes build
// directories containing whitespace safe on POSIX shells.
std::string shellQuote(const std::string &value) {
    std::string quoted("'");
    for (char c : value) {
        if (c == '\'') {
            quoted += "'\\''";
        } else {
            quoted += c;
        }
    }
    quoted += '\'';
    return quoted;
}

} // namespace

class PythonGenerator : public Generator {
public:
    void configure(const std::string &p_type, int p_depth, int p_count,
                   bool p_fullCharset) override {
        generatorType = p_type;
        depth = p_depth;
        count = p_count;
        fullCharset = p_fullCharset;
    }

    std::vector<FuzzTestCase> generate() override {
        std::vector<FuzzTestCase> testCases;
        generateTo([&testCases](const FuzzTestCase &testCase) {
            testCases.push_back(testCase);
            return true;
        });
        return testCases;
    }

    size_t generateTo(
        const std::function<bool(const FuzzTestCase &)> &consumer) override {
        const std::string command = buildCommand();
        if (command.empty()) {
            ADD_FAILURE() << "unable to locate fuzz generator script for "
                          << generatorType;
            return 0;
        }

        FILE *pipe = popen(command.c_str(), "r");
        if (!pipe) {
            ADD_FAILURE() << "failed to start fuzz generator: " << command;
            return 0;
        }

        char buffer[1024];
        int lineCount = 0;
        size_t delivered = 0;
        while (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
            lineCount++;
            const FuzzTestCase testCase = parseGeneratorOutput(buffer);
            if (!testCase.pattern.empty()) {
                if (!consumer(testCase)) {
                    break;
                }
                delivered++;
            }
        }

        const int status = pclose(pipe);
        if (status != 0) {
            ADD_FAILURE() << "fuzz generator command failed with status "
                          << status << ": " << command;
        }
        if (fuzzVerbose()) {
            std::cout << "Command exit status: " << status << std::endl;
            std::cout << "Total lines read: " << lineCount << std::endl;
            std::cout << "Total test cases delivered: " << delivered
                      << std::endl;
        }
        return delivered;
    }

    FuzzTestCase parseGeneratorOutput(const std::string &line) const {
        FuzzTestCase testCase{};

        // Expected generator record format: id:/pattern/flags.
        const size_t colonPos = line.find(':');
        if (colonPos == std::string::npos) {
            return testCase;
        }

        try {
            testCase.id = std::stoi(line.substr(0, colonPos));
        } catch (...) {
            return testCase;
        }

        const size_t firstSlash = line.find('/', colonPos + 1);
        if (firstSlash == std::string::npos) {
            return testCase;
        }

        // Patterns may contain '/'; the final slash is the record delimiter.
        const size_t lastSlash = line.rfind('/');
        if (lastSlash == std::string::npos || lastSlash <= firstSlash) {
            return testCase;
        }

        testCase.pattern =
            line.substr(firstSlash + 1, lastSlash - firstSlash - 1);

        std::string flagsStr = line.substr(lastSlash + 1);
        flagsStr.erase(std::remove(flagsStr.begin(), flagsStr.end(), '\n'),
                       flagsStr.end());
        flagsStr.erase(std::remove(flagsStr.begin(), flagsStr.end(), '\r'),
                       flagsStr.end());
        testCase.flags = parseFlags(flagsStr);
        return testCase;
    }

    unsigned int parseFlags(const std::string &flagsStr) const {
        unsigned int flags = 0;

        // Keep this mapping in sync with util/ExpressionParser.rl.
        for (char c : flagsStr) {
            switch (c) {
            case 's':
                flags |= HS_FLAG_DOTALL;
                break;
            case 'm':
                flags |= HS_FLAG_MULTILINE;
                break;
            case 'i':
                flags |= HS_FLAG_CASELESS;
                break;
            case 'H':
                flags |= HS_FLAG_SINGLEMATCH;
                break;
            case 'V':
                flags |= HS_FLAG_ALLOWEMPTY;
                break;
            case 'W':
                flags |= HS_FLAG_UCP;
                break;
            case '8':
                flags |= HS_FLAG_UTF8;
                break;
            case 'P':
                flags |= HS_FLAG_PREFILTER;
                break;
            case 'L':
                flags |= HS_FLAG_SOM_LEFTMOST;
                break;
            case 'C':
                flags |= HS_FLAG_COMBINATION;
                break;
            case 'Q':
                flags |= HS_FLAG_QUIET;
                break;
            }
        }

        return flags;
    }

private:
    std::string findGeneratorScript() const {
        const std::string script = generatorType + ".py";
        const std::vector<std::string> candidates = {
            script,
            "./" + script,
            "../../tools/fuzz/" + script,
            "../../../tools/fuzz/" + script,
            "tools/fuzz/" + script,
            "../tools/fuzz/" + script};

        for (const auto &path : candidates) {
            if (fileExists(path)) {
                return path;
            }
        }

        std::cerr << "Failed to find fuzz generator script " << script
                  << ". Tried:";
        for (const auto &path : candidates) {
            std::cerr << " " << path;
        }
        std::cerr << std::endl;
        return std::string();
    }

    std::string pythonCommand() const {
        const char *python = std::getenv("HS_FUZZ_PYTHON");
        if (python && python[0] != '\0') {
            return python;
        }
        return "python";
    }

    std::string buildCommand() const {
        const std::string scriptPath = findGeneratorScript();
        if (scriptPath.empty()) {
            return std::string();
        }

        std::stringstream cmd;
        cmd << pythonCommand() << " " << shellQuote(scriptPath);
        cmd << " --depth " << depth;
        cmd << " --count " << count;
        if (fullCharset) {
            cmd << " --full";
        }
        return cmd.str();
    }

    std::string generatorType;
    int depth = 0;
    int count = 0;
    bool fullCharset = false;
};

TEST(PythonGeneratorParsing, PatternMayContainSlash) {
    PythonGenerator generator;
    const FuzzTestCase testCase =
        generator.parseGeneratorOutput("42:/left/Q/right/smH\n");
    const unsigned int expectedFlags =
        HS_FLAG_DOTALL | HS_FLAG_MULTILINE | HS_FLAG_SINGLEMATCH;

    EXPECT_EQ(42, testCase.id);
    EXPECT_EQ("left/Q/right", testCase.pattern);
    EXPECT_EQ(expectedFlags, testCase.flags);
    EXPECT_EQ(0U, testCase.flags & HS_FLAG_QUIET);
}

TEST(PythonGeneratorParsing, MapsAllExpressionParserFlags) {
    PythonGenerator generator;
    const FuzzTestCase testCase =
        generator.parseGeneratorOutput("7:/x/smiHVW8PLCQ\n");
    const unsigned int expectedFlags =
        HS_FLAG_DOTALL | HS_FLAG_MULTILINE | HS_FLAG_CASELESS |
        HS_FLAG_SINGLEMATCH | HS_FLAG_ALLOWEMPTY | HS_FLAG_UCP | HS_FLAG_UTF8 |
        HS_FLAG_PREFILTER | HS_FLAG_SOM_LEFTMOST | HS_FLAG_COMBINATION |
        HS_FLAG_QUIET;

    EXPECT_EQ(7, testCase.id);
    EXPECT_EQ("x", testCase.pattern);
    EXPECT_EQ(expectedFlags, testCase.flags);
}

std::unique_ptr<Generator> createGenerator() {
    return std::make_unique<PythonGenerator>();
}
