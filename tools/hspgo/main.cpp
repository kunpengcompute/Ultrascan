/*
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *  * Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 *  * Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *  * Neither the name of Huawei Corporation nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "config.h"

#include "ExpressionParser.h"
#include "data_corpus.h"
#include "database.h"
#include "expressions.h"
#include "fp_collector.h"
#include "heapstats.h"
#include "hs.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <climits>
#include <clocale>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <limits>
#include <memory>
#include <sstream>
#include <stdint.h>
#include <string>
#include <thread>
#include <vector>

#include <getopt.h>
#include <sys/stat.h>
#include <sys/types.h>

#if defined(_WIN32)
#include <direct.h>
#endif

#if defined(__linux__)
#include <pthread.h>
#include <sched.h>
#endif

using std::cerr;
using std::cout;
using std::dec;
using std::hex;
using std::left;
using std::move;
using std::right;
using std::setfill;
using std::setw;
using std::string;
using std::vector;

namespace {

const size_t MAX_HSPGO_WORKERS = 1024;
const int OPT_ECHO_MATCHES = 1000;
const char *HSPGO_REPORT_CSV_NAME = "report.csv";
const char *HSPGO_FEEDBACK_CSV_NAME = "feedback.csv";
const char *HSPGO_FEEDBACK_BIN_NAME = "feedback.bin";
const unsigned HSPGO_RATE_SCALE_DECIMALS = 12;

enum class ScanMode {
    STREAMING,
    BLOCK,
    VECTORED,
};

struct Options {
    string exprPath;
    string corpusPath;
    string reportCsvPath;
    string feedbackBinPath;
    string feedbackImportPath;
    string threadSpec;
    string greyOverrides;
    vector<unsigned> cpuList;
    unsigned baselineRounds = 1;
    unsigned collectRounds = 1;
    unsigned measureRounds = 5;
    unsigned threadCount = 1;
    unsigned top = 0;
    ScanMode scanMode = ScanMode::STREAMING;
    hs_fp_feedback_params_t feedbackParams = {};
    bool showSummaries = false;
    bool showDiagnostics = false;
    bool echoMatches = false;
};

struct PatternSet {
    vector<string> exprs;
    vector<unsigned int> flags;
    vector<unsigned int> ids;
    vector<hs_expr_ext> ext;
};

struct RunStats {
    unsigned long long scanCalls = 0;
    unsigned long long bytes = 0;
    unsigned long long matches = 0;
};

struct MatchContext {
    RunStats *stats = nullptr;
    unsigned int blockId = 0;
    unsigned int streamId = 0;
    bool echoMatches = false;
    bool streaming = false;
};

struct ParallelRunResult {
    RunStats stats;
    double seconds = 0.0;
    double fastestWorkerSeconds = 0.0;
};

struct ThreadScanResult {
    RunStats stats;
    string error;
    double seconds = 0.0;
    bool ok = true;
};

struct DatabaseStats {
    string signatures;
    string info;
    size_t expressionCount = 0;
    size_t bytecodeSize = 0;
    uint32_t crc32 = 0;
    size_t scratchSize = 0;
    double compileSeconds = 0.0;
    size_t peakHeap = 0;
};

struct DatabaseDeleter {
    void operator()(hs_database_t *db) const { hs_free_database(db); }
};

struct ScratchDeleter {
    void operator()(hs_scratch_t *scratch) const { hs_free_scratch(scratch); }
};

struct CollectorDeleter {
    void operator()(hs_fp_collector_t *collector) const {
        hs_fp_collector_free(collector);
    }
};

struct FeedbackDeleter {
    void operator()(hs_fp_feedback_t *feedback) const {
        hs_fp_feedback_free(feedback);
    }
};

struct CompileContextDeleter {
    void operator()(hs_compile_context_t *ctx) const {
        hs_compile_context_free(ctx);
    }
};

using DatabasePtr = std::unique_ptr<hs_database_t, DatabaseDeleter>;
using ScratchPtr = std::unique_ptr<hs_scratch_t, ScratchDeleter>;
using CollectorPtr = std::unique_ptr<hs_fp_collector_t, CollectorDeleter>;
using FeedbackPtr = std::unique_ptr<hs_fp_feedback_t, FeedbackDeleter>;
using CompileContextPtr =
    std::unique_ptr<hs_compile_context_t, CompileContextDeleter>;

struct OwnedFragment {
    hs_fp_fragment_info_t info = {};
    vector<u8> bytes;
    vector<u8> mask;
    vector<u8> cmp;
};

struct DumpData {
    hs_fp_feedback_dump_summary_t summary = {};
    vector<OwnedFragment> reportFragments;
    vector<OwnedFragment> feedbackFragments;
};

void usage(const char *error) {
    cout
        << "Usage: hspgo [OPTIONS...]\n\n"
        << "Options:\n\n"
        << "  -h, --help              Display help and exit.\n"
        << "  -G OVERRIDES            Overrides for the grey box.\n"
        << "  -e PATH                 Load hsbench expression file/directory.\n"
        << "  -c FILE                 Load hsbench sqlite corpus.\n"
        << "  -b N                    Run N normal hs_scan() baseline rounds "
           "(default 1).\n"
        << "  -n N                    Run N measurement rounds after DB switch "
           "(default 5).\n"
        << "  -N                      Run in block mode (default: streaming).\n"
        << "  -V                      Run in vectored mode (default: "
           "streaming).\n"
        << "  -m N                    Minimum trigger count for feedback "
           "(default 1000).\n"
        << "  -p N                    Minimum false-positive trigger count "
           "(default 1000).\n"
        << "  -q RATIO                Minimum false-positive rate ratio, 0..1 "
           "(default 0.99).\n"
        << "  -s RATIO                Minimum false-positive share ratio, 0..1 "
           "(default 0.05).\n"
        << "  -k N                    Maximum bad fragments kept for feedback "
           "(default all).\n"
        << "  -T CPU[,CPU|CPU-CPU]...\n"
        << "                          Run one worker per CPU and bind "
           "affinity.\n"
        << "  -v                      Verbose feedback view with summaries, "
           "diagnostics and top 10 fragments.\n"
        << "  --echo-matches          Display optimized measurement matches.\n"
        << "  -o DIR                  Dump report/feedback CSV files into "
           "DIR.\n"
        << "  -O DIR                  Dump reusable feedback binary into DIR.\n"
        << "  -I DIR                  Load reusable feedback binary from DIR "
           "and skip collection.\n\n"
        << "Optimized throughput is measured only after the feedback-compiled "
           "DB is active.\n";

    if (error) {
        cerr << "Error: " << error << "\n";
    }
}

void usage(const string &error) { usage(error.c_str()); }

bool isPathSeparator(char c) { return c == '/' || c == '\\'; }

string trimTrailingSeparators(const string &path) {
    if (path.empty()) {
        return path;
    }

    size_t end = path.size();
    while (end > 1 && isPathSeparator(path[end - 1])) {
        if (end == 3 && path[1] == ':') {
            break;
        }
        end--;
    }
    return path.substr(0, end);
}

string parentDirectory(const string &path) {
    const string trimmed = trimTrailingSeparators(path);
    const size_t pos = trimmed.find_last_of("/\\");
    if (pos == string::npos) {
        return "";
    }
    if (pos == 0) {
        return trimmed.substr(0, 1);
    }
    if (pos == 2 && trimmed[1] == ':') {
        return trimmed.substr(0, 3);
    }
    return trimmed.substr(0, pos);
}

bool directoryExists(const string &path) {
    struct stat st = {};
    if (stat(path.c_str(), &st) != 0) {
        return false;
    }
#if defined(_WIN32)
    return (st.st_mode & _S_IFDIR) != 0;
#else
    return S_ISDIR(st.st_mode);
#endif
}

bool pathExists(const string &path) {
    struct stat st = {};
    return stat(path.c_str(), &st) == 0;
}

bool makeDirectory(const string &path) {
#if defined(_WIN32)
    return _mkdir(path.c_str()) == 0 || errno == EEXIST;
#else
    return mkdir(path.c_str(), 0777) == 0 || errno == EEXIST;
#endif
}

bool ensureDirectory(const string &path, const char *purpose) {
    const string dir = trimTrailingSeparators(path);
    if (dir.empty()) {
        cerr << purpose << " directory path must not be empty\n";
        return false;
    }
    if (directoryExists(dir)) {
        return true;
    }
    if (pathExists(dir)) {
        cerr << purpose << " path exists but is not a directory: " << dir
             << "\n";
        return false;
    }

    const string parent = parentDirectory(dir);
    if (!parent.empty() && parent != dir && !directoryExists(parent) &&
        !ensureDirectory(parent, purpose)) {
        return false;
    }

    if (!makeDirectory(dir) || !directoryExists(dir)) {
        cerr << "Unable to create " << purpose << " directory: " << dir << "\n";
        return false;
    }
    return true;
}

string joinPath(const string &dir, const char *name) {
    string out = trimTrailingSeparators(dir);
    if (!out.empty() && !isPathSeparator(out.back())) {
        out.push_back('/');
    }
    out += name;
    return out;
}

string optionName(int c) {
    if (c > 0 && std::isprint(c)) {
        string name("-");
        name.push_back(static_cast<char>(c));
        return name;
    }
    return "option";
}

bool parsePositiveUnsigned(const char *text, unsigned *out) {
    if (!text || !*text || !out || text[0] == '+' || text[0] == '-') {
        return false;
    }

    errno = 0;
    char *end = nullptr;
    unsigned long value = std::strtoul(text, &end, 10);
    if (errno || !end || *end != '\0' || value == 0 ||
        value > std::numeric_limits<unsigned>::max()) {
        return false;
    }

    *out = static_cast<unsigned>(value);
    return true;
}

bool parseU64(const char *text, unsigned long long *out) {
    if (!text || !*text || !out || text[0] == '+' || text[0] == '-') {
        return false;
    }

    errno = 0;
    char *end = nullptr;
    unsigned long long value = std::strtoull(text, &end, 10);
    if (errno || !end || *end != '\0') {
        return false;
    }

    *out = value;
    return true;
}

bool parseUnsigned(const char *text, unsigned *out) {
    if (!text || !*text || !out || text[0] == '+' || text[0] == '-') {
        return false;
    }

    errno = 0;
    char *end = nullptr;
    unsigned long value = std::strtoul(text, &end, 10);
    if (errno || !end || *end != '\0' ||
        value > std::numeric_limits<unsigned>::max()) {
        return false;
    }

    *out = static_cast<unsigned>(value);
    return true;
}

bool parseRatioScaled(const char *text, unsigned long long *out) {
    if (!text || !*text || !out || text[0] == '+' || text[0] == '-') {
        return false;
    }

    const string value(text);
    const size_t dot = value.find('.');
    if (dot != string::npos && value.find('.', dot + 1) != string::npos) {
        return false;
    }

    const string whole = dot == string::npos ? value : value.substr(0, dot);
    const string frac = dot == string::npos ? string() : value.substr(dot + 1);
    if (whole.empty() && frac.empty()) {
        return false;
    }

    unsigned wholeValue = 0;
    for (char c : whole) {
        if (!std::isdigit(static_cast<unsigned char>(c))) {
            return false;
        }
        wholeValue = wholeValue * 10U + static_cast<unsigned>(c - '0');
        if (wholeValue > 1U) {
            return false;
        }
    }

    if (wholeValue == 1U) {
        for (char c : frac) {
            if (!std::isdigit(static_cast<unsigned char>(c)) || c != '0') {
                return false;
            }
        }
        *out = HS_FP_FEEDBACK_RATE_SCALE;
        return true;
    }

    unsigned long long scaled = 0;
    unsigned digits = 0;
    bool ceilTail = false;
    for (char c : frac) {
        if (!std::isdigit(static_cast<unsigned char>(c))) {
            return false;
        }
        const unsigned digit = static_cast<unsigned>(c - '0');
        if (digits < HSPGO_RATE_SCALE_DECIMALS) {
            scaled = scaled * 10ULL + digit;
            digits++;
        } else if (digit) {
            ceilTail = true;
        }
    }

    while (digits < HSPGO_RATE_SCALE_DECIMALS) {
        scaled *= 10ULL;
        digits++;
    }

    if (ceilTail && scaled < HS_FP_FEEDBACK_RATE_SCALE) {
        scaled++;
    }
    *out = scaled;
    return true;
}

bool greyOverridesHaveNegativeValue(const char *text) {
    if (!text) {
        return false;
    }

    const char *p = text;
    while (*p) {
        const char *colon = std::strchr(p, ':');
        if (!colon) {
            return false;
        }

        const char *value = colon + 1;
        while (*value == ' ' || *value == '\t') {
            value++;
        }
        if (*value == '-') {
            return true;
        }

        const char *semi = std::strchr(value, ';');
        if (!semi) {
            return false;
        }
        p = semi + 1;
    }

    return false;
}

bool appendCpu(unsigned cpu, vector<unsigned> *out, string *error) {
    if (out->size() >= MAX_HSPGO_WORKERS) {
        if (error) {
            *error = "thread CPU list has too many entries";
        }
        return false;
    }

    for (unsigned existing : *out) {
        if (existing == cpu) {
            if (error) {
                std::ostringstream oss;
                oss << "CPU " << cpu << " appears more than once";
                *error = oss.str();
            }
            return false;
        }
    }

#if defined(__linux__)
    if (cpu >= CPU_SETSIZE) {
        if (error) {
            std::ostringstream oss;
            oss << "CPU " << cpu << " is outside CPU_SETSIZE";
            *error = oss.str();
        }
        return false;
    }
#endif

    out->push_back(cpu);
    return true;
}

bool parseCpuList(const char *text, vector<unsigned> *out, string *error) {
    if (!text || !*text || !out) {
        if (error) {
            *error = "thread CPU list must not be empty";
        }
        return false;
    }

    out->clear();
    string spec(text);
    size_t pos = 0;
    while (pos < spec.size()) {
        const size_t comma = spec.find(',', pos);
        const size_t end = comma == string::npos ? spec.size() : comma;
        if (end == pos) {
            if (error) {
                *error = "thread CPU list contains an empty item";
            }
            return false;
        }

        string token = spec.substr(pos, end - pos);
        const size_t dash = token.find('-');
        if (dash == string::npos) {
            unsigned cpu = 0;
            if (!parseUnsigned(token.c_str(), &cpu)) {
                if (error) {
                    *error = "thread CPU list contains an invalid CPU id";
                }
                return false;
            }
            if (!appendCpu(cpu, out, error)) {
                return false;
            }
        } else {
            if (dash == 0 || dash + 1 >= token.size() ||
                token.find('-', dash + 1) != string::npos) {
                if (error) {
                    *error = "thread CPU range must be FIRST-LAST";
                }
                return false;
            }
            unsigned first = 0;
            unsigned last = 0;
            if (!parseUnsigned(token.substr(0, dash).c_str(), &first) ||
                !parseUnsigned(token.substr(dash + 1).c_str(), &last) ||
                first > last) {
                if (error) {
                    *error = "thread CPU range is invalid";
                }
                return false;
            }
            const unsigned long long rangeCount =
                static_cast<unsigned long long>(last) -
                static_cast<unsigned long long>(first) + 1;
            if (rangeCount > MAX_HSPGO_WORKERS ||
                out->size() > MAX_HSPGO_WORKERS - rangeCount) {
                if (error) {
                    *error = "thread CPU range has too many entries";
                }
                return false;
            }
            for (unsigned cpu = first;; cpu++) {
                if (!appendCpu(cpu, out, error)) {
                    return false;
                }
                if (cpu == last) {
                    break;
                }
            }
        }

        if (out->empty() || out->size() > MAX_HSPGO_WORKERS) {
            if (error && error->empty()) {
                *error = "thread CPU list must not be empty";
            }
            return false;
        }

        if (comma == string::npos) {
            break;
        }
        pos = comma + 1;
    }

    return !out->empty();
}

bool processArgs(int argc, char **argv, Options *opts) {
    static const struct option longopts[] = {
        {"help", no_argument, nullptr, 'h'},
        {"echo-matches", no_argument, nullptr, OPT_ECHO_MATCHES},
        {nullptr, 0, nullptr, 0}};

    opterr = 0;
    int optionIndex = 0;
    for (;;) {
        int c = getopt_long(argc, argv, ":b:c:e:G:hI:k:m:Nn:o:O:p:q:s:T:vV",
                            longopts, &optionIndex);
        if (c < 0) {
            break;
        }

        unsigned value = 0;
        unsigned long long value64 = 0;
        switch (c) {
        case 'h':
            usage(nullptr);
            std::exit(0);
        case 'G':
            if (greyOverridesHaveNegativeValue(optarg)) {
                usage("grey override values must be non-negative");
                return false;
            }
            opts->greyOverrides.assign(optarg);
            if (hs_set_grey_overrides(optarg) != HS_SUCCESS) {
                usage("Invalid grey overrides");
                return false;
            }
            break;
        case 'b':
            if (!parseUnsigned(optarg, &value)) {
                usage("baseline rounds must be a non-negative integer");
                return false;
            }
            opts->baselineRounds = value;
            break;
        case 'c':
            if (!optarg || !*optarg) {
                usage("corpus path must not be empty");
                return false;
            }
            opts->corpusPath.assign(optarg);
            break;
        case 'e':
            if (!optarg || !*optarg) {
                usage("expression path must not be empty");
                return false;
            }
            opts->exprPath.assign(optarg);
            break;
        case 'p':
            if (!parseU64(optarg, &value64)) {
                usage("minimum false-positive count must be a non-negative "
                      "integer");
                return false;
            }
            opts->feedbackParams.flags |=
                HS_FP_FEEDBACK_PARAM_MIN_FALSE_POSITIVE_COUNT;
            opts->feedbackParams.min_false_positive_count = value64;
            break;
        case 'k':
            if (!parsePositiveUnsigned(optarg, &value)) {
                usage("feedback TopK must be a positive integer");
                return false;
            }
            opts->feedbackParams.flags |=
                HS_FP_FEEDBACK_PARAM_MAX_BAD_FRAGMENTS;
            opts->feedbackParams.max_bad_fragments = value;
            break;
        case 'I':
            if (!optarg || !*optarg) {
                usage("feedback binary input directory must not be empty");
                return false;
            }
            opts->feedbackImportPath.assign(optarg);
            break;
        case 'm':
            if (!parseU64(optarg, &value64)) {
                usage("minimum trigger count must be a non-negative integer");
                return false;
            }
            opts->feedbackParams.flags |=
                HS_FP_FEEDBACK_PARAM_MIN_TRIGGER_COUNT;
            opts->feedbackParams.min_trigger_count = value64;
            break;
        case 'n':
            if (!parsePositiveUnsigned(optarg, &value)) {
                usage("measure rounds must be a positive integer");
                return false;
            }
            opts->measureRounds = value;
            break;
        case 'N':
            opts->scanMode = ScanMode::BLOCK;
            break;
        case 'o':
            if (!optarg || !*optarg) {
                usage("CSV output directory must not be empty");
                return false;
            }
            opts->reportCsvPath.assign(optarg);
            break;
        case 'O':
            if (!optarg || !*optarg) {
                usage("feedback binary output directory must not be empty");
                return false;
            }
            opts->feedbackBinPath.assign(optarg);
            break;
        case 'q':
            if (!parseRatioScaled(optarg, &value64)) {
                usage("minimum false-positive rate must be a ratio from 0 to "
                      "1");
                return false;
            }
            opts->feedbackParams.flags |=
                HS_FP_FEEDBACK_PARAM_MIN_FALSE_POSITIVE_RATE;
            opts->feedbackParams.min_false_positive_rate = value64;
            break;
        case 'T': {
            if (!optarg || !*optarg) {
                usage("thread list must not be empty");
                return false;
            }
            opts->threadSpec.assign(optarg);
            string cpuError;
            if (!parseCpuList(optarg, &opts->cpuList, &cpuError)) {
                if (cpuError.empty()) {
                    cpuError = "thread CPU list must be CPU[,CPU|CPU-CPU]...";
                }
                usage(cpuError);
                return false;
            }
            opts->threadCount = static_cast<unsigned>(opts->cpuList.size());
            break;
        }
        case 'v':
            opts->showSummaries = true;
            opts->showDiagnostics = true;
            if (!opts->top) {
                opts->top = 10;
            }
            break;
        case 'V':
            opts->scanMode = ScanMode::VECTORED;
            break;
        case 's':
            if (!parseRatioScaled(optarg, &value64)) {
                usage(
                    "minimum false-positive share must be a ratio from 0 to 1");
                return false;
            }
            opts->feedbackParams.flags |= HS_FP_FEEDBACK_PARAM_MIN_WASTE_SHARE;
            opts->feedbackParams.min_waste_share = value64;
            break;
        case OPT_ECHO_MATCHES:
            opts->echoMatches = true;
            break;
        case ':':
            usage(optionName(optopt) + " requires an argument");
            return false;
        case '?':
            if (optopt) {
                usage("unknown option " + optionName(optopt));
            } else if (optind > 0 && optind <= argc) {
                usage(string("unknown option ") + argv[optind - 1]);
            } else {
                usage("unknown option");
            }
            return false;
        default:
            usage("unknown argument");
            return false;
        }
    }

    if (optind < argc) {
        usage("unexpected positional argument");
        return false;
    }
    if (opts->exprPath.empty()) {
        usage("provide -e");
        return false;
    }
    if (opts->corpusPath.empty()) {
        usage("provide -c");
        return false;
    }
    if (!opts->feedbackImportPath.empty()) {
        opts->collectRounds = 0;
    }

    return true;
}

bool looksLikeDelimitedPcre(const string &pattern) {
    return pattern.size() >= 2 && pattern[0] == '/';
}

bool addExpression(unsigned int id, const string &input, bool allowPlain,
                   PatternSet *out) {
    string expr;
    unsigned int flags = 0;
    hs_expr_ext ext = {};

    if (looksLikeDelimitedPcre(input)) {
        if (!readExpression(input, expr, &flags, &ext)) {
            cerr << "Unable to parse expression id " << id << ": " << input
                 << "\n";
            return false;
        }
    } else if (allowPlain) {
        expr = input;
    } else {
        cerr << "Expression id " << id
             << " is not in /pattern/flags format: " << input << "\n";
        return false;
    }

    out->exprs.push_back(expr);
    out->flags.push_back(flags);
    out->ids.push_back(id);
    out->ext.push_back(ext);
    return true;
}

bool loadPatternSet(const Options &opts, PatternSet *patterns) {
    ExpressionMap exprMap;
    loadExpressions(opts.exprPath, exprMap);

    for (const auto &m : exprMap) {
        if (!addExpression(m.first, m.second, false, patterns)) {
            return false;
        }
    }

    if (patterns->exprs.empty()) {
        cerr << "No expressions loaded\n";
        return false;
    }
    if (patterns->exprs.size() > std::numeric_limits<unsigned int>::max()) {
        cerr << "Too many expressions\n";
        return false;
    }

    return true;
}

bool loadCorpus(const Options &opts, vector<DataBlock> *blocks) {
    try {
        vector<DataBlock> dbBlocks = readCorpus(opts.corpusPath);
        blocks->insert(blocks->end(), dbBlocks.begin(), dbBlocks.end());
    } catch (const DataCorpusError &e) {
        cerr << "Corpus sqlite read failed: " << e.msg << "\n";
        return false;
    }

    if (blocks->empty()) {
        cerr << "No corpus blocks loaded\n";
        return false;
    }

    return true;
}

unsigned long long corpusBytes(const vector<DataBlock> &blocks) {
    unsigned long long total = 0;
    for (const auto &block : blocks) {
        total += block.payload.size();
    }
    return total;
}

string formatCount(unsigned long long value) {
    string s = std::to_string(value);
    string out;
    out.reserve(s.size() + s.size() / 3);
    for (size_t i = 0; i < s.size(); i++) {
        if (i && (s.size() - i) % 3 == 0) {
            out.push_back(',');
        }
        out.push_back(s[i]);
    }
    return out;
}

string formatFixed(double value, int precision) {
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(precision) << value;
    return oss.str();
}

string formatFixedWithCommas(double value, int precision) {
    string s = formatFixed(value, precision);
    const size_t dot = s.find('.');
    string integer = dot == string::npos ? s : s.substr(0, dot);
    string suffix = dot == string::npos ? string() : s.substr(dot);
    string sign;
    if (!integer.empty() && integer[0] == '-') {
        sign = "-";
        integer.erase(integer.begin());
    }

    string out;
    out.reserve(integer.size() + integer.size() / 3 + suffix.size() +
                sign.size());
    out += sign;
    for (size_t i = 0; i < integer.size(); i++) {
        if (i && (integer.size() - i) % 3 == 0) {
            out.push_back(',');
        }
        out.push_back(integer[i]);
    }
    out += suffix;
    return out;
}

string formatHex32(uint32_t value) {
    std::ostringstream oss;
    oss << "0x" << hex << value << dec;
    return oss.str();
}

string formatHex64(uint64_t value) {
    std::ostringstream oss;
    oss << "0x" << hex << value << dec;
    return oss.str();
}

string signatureName(const Options &opts) { return opts.exprPath; }

const char *scanModeName(ScanMode mode) {
    switch (mode) {
    case ScanMode::STREAMING:
        return "stream";
    case ScanMode::BLOCK:
        return "block";
    case ScanMode::VECTORED:
        return "vector";
    }
    return "unknown";
}

unsigned int compileModeFlags(ScanMode mode) {
    switch (mode) {
    case ScanMode::STREAMING:
        return HS_MODE_STREAM | HS_MODE_SOM_HORIZON_LARGE;
    case ScanMode::BLOCK:
        return HS_MODE_BLOCK;
    case ScanMode::VECTORED:
        return HS_MODE_VECTORED;
    }
    return HS_MODE_STREAM | HS_MODE_SOM_HORIZON_LARGE;
}

void printField(const char *label, const string &value) {
    cout << left << setw(28) << label << value << "\n";
}

void printField(const char *label, unsigned long long value) {
    printField(label, formatCount(value));
}

bool compileDatabase(const PatternSet &patterns, ScanMode mode,
                     const hs_compile_context_t *ctx, DatabasePtr *out) {
    vector<const char *> exprPtrs(patterns.exprs.size());
    vector<const hs_expr_ext *> extPtrs(patterns.ext.size());

    for (size_t i = 0; i < patterns.exprs.size(); i++) {
        exprPtrs[i] = patterns.exprs[i].c_str();
        extPtrs[i] = &patterns.ext[i];
    }

    hs_database_t *rawDb = nullptr;
    hs_compile_error_t *compileErr = nullptr;
    const unsigned int count = static_cast<unsigned int>(patterns.exprs.size());
    hs_error_t err = HS_SUCCESS;
    if (ctx) {
        err = hs_compile_ext_multi_with_context(
            exprPtrs.data(), patterns.flags.data(), patterns.ids.data(),
            extPtrs.data(), count, compileModeFlags(mode), nullptr, ctx, &rawDb,
            &compileErr);
    } else {
        err = hs_compile_ext_multi(exprPtrs.data(), patterns.flags.data(),
                                   patterns.ids.data(), extPtrs.data(), count,
                                   compileModeFlags(mode), nullptr, &rawDb,
                                   &compileErr);
    }

    if (err != HS_SUCCESS) {
        if (compileErr) {
            cerr << "Compile failed";
            if (compileErr->expression >= 0) {
                cerr << " at expression index " << compileErr->expression;
            }
            cerr << ": " << compileErr->message << "\n";
            hs_free_compile_error(compileErr);
        } else {
            cerr << "Compile failed with error " << err << "\n";
        }
        return false;
    }

    out->reset(rawDb);
    return true;
}

bool allocScratch(const hs_database_t *db, ScratchPtr *scratch) {
    hs_scratch_t *rawScratch = nullptr;
    hs_error_t err = hs_alloc_scratch(db, &rawScratch);
    if (err != HS_SUCCESS) {
        cerr << "hs_alloc_scratch failed with error " << err << "\n";
        return false;
    }

    scratch->reset(rawScratch);
    return true;
}

bool queryDatabaseStats(const hs_database_t *db, const hs_scratch_t *scratch,
                        const string &signatures, size_t expressionCount,
                        double compileSeconds, DatabaseStats *stats) {
    char *info = nullptr;
    hs_error_t err = hs_database_info(db, &info);
    if (err != HS_SUCCESS || !info) {
        cerr << "hs_database_info failed with error " << err << "\n";
        return false;
    }

    stats->signatures = signatures;
    stats->info.assign(info);
    std::free(info);
    stats->expressionCount = expressionCount;
    stats->crc32 = db->crc32;
    stats->compileSeconds = compileSeconds;
    stats->peakHeap = getPeakHeap();

    err = hs_database_size(db, &stats->bytecodeSize);
    if (err != HS_SUCCESS) {
        cerr << "hs_database_size failed with error " << err << "\n";
        return false;
    }

    err = hs_scratch_size(scratch, &stats->scratchSize);
    if (err != HS_SUCCESS) {
        cerr << "hs_scratch_size failed with error " << err << "\n";
        return false;
    }

    return true;
}

int HS_CDECL onMatch(unsigned int id, unsigned long long, unsigned long long to,
                     unsigned int, void *ctx) {
    if (ctx) {
        MatchContext *matchCtx = static_cast<MatchContext *>(ctx);
        if (matchCtx->stats) {
            matchCtx->stats->matches++;
        }
        if (matchCtx->echoMatches) {
            if (matchCtx->streaming) {
                std::printf("Match @%u:%u:%llu for %u\n", matchCtx->streamId,
                            matchCtx->blockId, to, id);
            } else {
                std::printf("Match @%u:%llu for %u\n", matchCtx->blockId, to,
                            id);
            }
        }
    }
    return 0;
}

size_t streamSlotCount(const vector<DataBlock> &blocks) {
    size_t count = 0;
    for (const auto &block : blocks) {
        count = std::max(count,
                         static_cast<size_t>(block.internal_stream_index) + 1);
    }
    return count;
}

struct StreamInfo {
    unsigned int streamId = ~0U;
    unsigned int firstBlockId = ~0U;
    unsigned int lastBlockId = 0;
    hs_stream_t *handle = nullptr;
};

vector<StreamInfo> prepStreamingData(const vector<DataBlock> &blocks) {
    vector<StreamInfo> streams(streamSlotCount(blocks));
    for (const auto &block : blocks) {
        StreamInfo &stream = streams[block.internal_stream_index];
        if (stream.firstBlockId > stream.lastBlockId) {
            stream.streamId = block.stream_id;
            stream.firstBlockId = block.id;
            stream.lastBlockId = block.id;
        } else {
            stream.lastBlockId = block.id;
        }
    }
    return streams;
}

struct VectoredInfo {
    vector<const char *> data;
    vector<unsigned int> length;
    unsigned int streamId = ~0U;
    unsigned long long bytes = 0;
};

bool prepVectorData(const vector<DataBlock> &blocks,
                    vector<VectoredInfo> *vectors, string *error) {
    vectors->clear();
    vectors->resize(streamSlotCount(blocks));

    for (const auto &block : blocks) {
        if (block.payload.size() > std::numeric_limits<unsigned int>::max()) {
            std::ostringstream oss;
            oss << "Corpus block " << block.id
                << " is too large for vector scan";
            if (error) {
                *error = oss.str();
            }
            return false;
        }

        VectoredInfo &vector = (*vectors)[block.internal_stream_index];
        if (vector.data.size() >= std::numeric_limits<unsigned int>::max()) {
            std::ostringstream oss;
            oss << "Corpus stream " << block.stream_id
                << " has too many blocks for vector scan";
            if (error) {
                *error = oss.str();
            }
            return false;
        }
        if (vector.data.empty()) {
            vector.streamId = block.stream_id;
        }
        vector.data.push_back(block.payload.data());
        vector.length.push_back(
            static_cast<unsigned int>(block.payload.size()));
        vector.bytes += block.payload.size();
    }

    return true;
}

bool scanBlocks(const hs_database_t *db, hs_scratch_t *scratch,
                const vector<DataBlock> &blocks, unsigned rounds,
                hs_fp_collector_t *collector, bool echoMatches, RunStats *stats,
                string *error) {
    for (unsigned round = 0; round < rounds; round++) {
        for (const auto &block : blocks) {
            if (block.payload.size() >
                std::numeric_limits<unsigned int>::max()) {
                std::ostringstream oss;
                oss << "Corpus block " << block.id
                    << " is too large for block scan";
                if (error) {
                    *error = oss.str();
                }
                return false;
            }

            const char *data = block.payload.data();
            const unsigned int len =
                static_cast<unsigned int>(block.payload.size());
            MatchContext matchCtx;
            matchCtx.stats = stats;
            matchCtx.blockId = block.id;
            matchCtx.echoMatches = echoMatches;
            hs_error_t err = HS_SUCCESS;
            if (collector) {
                err = hs_scan_with_collector(db, data, len, 0, scratch, onMatch,
                                             &matchCtx, collector);
            } else {
                err = hs_scan(db, data, len, 0, scratch, onMatch, &matchCtx);
            }
            if (err != HS_SUCCESS) {
                std::ostringstream oss;
                oss << "scan failed with error " << err;
                if (error) {
                    *error = oss.str();
                }
                return false;
            }

            stats->scanCalls++;
            stats->bytes += len;
        }
    }

    return true;
}

bool closeStream(StreamInfo *stream, hs_scratch_t *scratch,
                 hs_fp_collector_t *collector, bool echoMatches,
                 RunStats *stats, string *error) {
    (void)collector;
    MatchContext matchCtx;
    matchCtx.stats = stats;
    matchCtx.blockId = 0;
    matchCtx.streamId = stream->streamId;
    matchCtx.echoMatches = echoMatches;
    matchCtx.streaming = true;

    hs_error_t err =
        hs_close_stream(stream->handle, scratch, onMatch, &matchCtx);
    stream->handle = nullptr;

    if (err != HS_SUCCESS) {
        std::ostringstream oss;
        oss << "stream close failed with error " << err;
        if (error) {
            *error = oss.str();
        }
        return false;
    }
    return true;
}

bool scanStreaming(const hs_database_t *db, hs_scratch_t *scratch,
                   const vector<DataBlock> &blocks, unsigned rounds,
                   hs_fp_collector_t *collector, bool echoMatches,
                   RunStats *stats, string *error) {
    for (unsigned round = 0; round < rounds; round++) {
        vector<StreamInfo> streams = prepStreamingData(blocks);

        for (const auto &block : blocks) {
            if (block.payload.size() >
                std::numeric_limits<unsigned int>::max()) {
                std::ostringstream oss;
                oss << "Corpus block " << block.id
                    << " is too large for stream scan";
                if (error) {
                    *error = oss.str();
                }
                return false;
            }

            StreamInfo &stream = streams[block.internal_stream_index];
            if (block.id == stream.firstBlockId) {
                hs_error_t err = hs_open_stream(db, 0, &stream.handle);
                if (err != HS_SUCCESS || !stream.handle) {
                    std::ostringstream oss;
                    oss << "stream open failed with error " << err;
                    if (error) {
                        *error = oss.str();
                    }
                    return false;
                }
            }

            MatchContext matchCtx;
            matchCtx.stats = stats;
            matchCtx.blockId = block.id;
            matchCtx.streamId = stream.streamId;
            matchCtx.echoMatches = echoMatches;
            matchCtx.streaming = true;

            const char *data = block.payload.data();
            const unsigned int len =
                static_cast<unsigned int>(block.payload.size());
            hs_error_t err = HS_SUCCESS;
            if (collector) {
                err = hs_scan_stream_with_collector(stream.handle, data, len, 0,
                                                    scratch, onMatch, &matchCtx,
                                                    collector);
            } else {
                err = hs_scan_stream(stream.handle, data, len, 0, scratch,
                                     onMatch, &matchCtx);
            }
            if (err != HS_SUCCESS) {
                std::ostringstream oss;
                oss << "stream scan failed with error " << err;
                if (error) {
                    *error = oss.str();
                }
                return false;
            }

            stats->scanCalls++;
            stats->bytes += len;

            if (block.id == stream.lastBlockId &&
                !closeStream(&stream, scratch, collector, echoMatches, stats,
                             error)) {
                return false;
            }
        }
    }

    return true;
}

bool scanVectored(const hs_database_t *db, hs_scratch_t *scratch,
                  const vector<DataBlock> &blocks, unsigned rounds,
                  hs_fp_collector_t *collector, bool echoMatches,
                  RunStats *stats, string *error) {
    vector<VectoredInfo> vectors;
    if (!prepVectorData(blocks, &vectors, error)) {
        return false;
    }

    for (unsigned round = 0; round < rounds; round++) {
        for (const auto &vector : vectors) {
            if (vector.data.empty()) {
                continue;
            }

            MatchContext matchCtx;
            matchCtx.stats = stats;
            matchCtx.blockId = vector.streamId;
            matchCtx.echoMatches = echoMatches;

            hs_error_t err = HS_SUCCESS;
            if (collector) {
                err = hs_scan_vector_with_collector(
                    db, vector.data.data(), vector.length.data(),
                    static_cast<unsigned int>(vector.data.size()), 0, scratch,
                    onMatch, &matchCtx, collector);
            } else {
                err = hs_scan_vector(
                    db, vector.data.data(), vector.length.data(),
                    static_cast<unsigned int>(vector.data.size()), 0, scratch,
                    onMatch, &matchCtx);
            }
            if (err != HS_SUCCESS) {
                std::ostringstream oss;
                oss << "vector scan failed with error " << err;
                if (error) {
                    *error = oss.str();
                }
                return false;
            }

            stats->scanCalls += vector.data.size();
            stats->bytes += vector.bytes;
        }
    }

    return true;
}

bool scanCorpus(const hs_database_t *db, hs_scratch_t *scratch,
                const vector<DataBlock> &blocks, unsigned rounds,
                hs_fp_collector_t *collector, bool echoMatches, ScanMode mode,
                RunStats *stats, string *error) {
    switch (mode) {
    case ScanMode::STREAMING:
        return scanStreaming(db, scratch, blocks, rounds, collector,
                             echoMatches, stats, error);
    case ScanMode::BLOCK:
        return scanBlocks(db, scratch, blocks, rounds, collector, echoMatches,
                          stats, error);
    case ScanMode::VECTORED:
        return scanVectored(db, scratch, blocks, rounds, collector, echoMatches,
                            stats, error);
    }
    if (error) {
        *error = "unknown scan mode";
    }
    return false;
}

void addRunStats(RunStats *dst, const RunStats &src) {
    dst->scanCalls += src.scanCalls;
    dst->bytes += src.bytes;
    dst->matches += src.matches;
}

double secondsSince(const std::chrono::steady_clock::time_point &start,
                    const std::chrono::steady_clock::time_point &end);

bool allocScratches(const hs_database_t *db, unsigned count,
                    vector<ScratchPtr> *scratches) {
    scratches->clear();
    scratches->resize(count);
    for (unsigned i = 0; i < count; i++) {
        if (!allocScratch(db, &(*scratches)[i])) {
            return false;
        }
    }
    return true;
}

bool createCollectors(const hs_database_t *db, unsigned count,
                      vector<CollectorPtr> *collectors) {
    collectors->clear();
    collectors->resize(count);
    for (unsigned i = 0; i < count; i++) {
        hs_fp_collector_t *rawCollector = nullptr;
        hs_error_t err = hs_fp_collector_create(db, &rawCollector);
        if (err != HS_SUCCESS) {
            cerr << "hs_fp_collector_create failed with error " << err << "\n";
            return false;
        }
        (*collectors)[i].reset(rawCollector);
    }
    return true;
}

bool mergeCollectors(const hs_database_t *db,
                     const vector<CollectorPtr> &collectors,
                     CollectorPtr *merged) {
    (void)db;
    if (collectors.empty()) {
        cerr << "no collectors to merge\n";
        return false;
    }

    vector<hs_fp_collector_t *> inputs;
    inputs.reserve(collectors.size());
    for (const auto &collector : collectors) {
        inputs.push_back(collector.get());
    }

    hs_fp_collector_t *rawCollector = nullptr;
    hs_error_t err = hs_fp_collector_merge(
        inputs.data(), static_cast<unsigned>(inputs.size()), &rawCollector);
    if (err != HS_SUCCESS) {
        cerr << "hs_fp_collector_merge failed with error " << err << "\n";
        return false;
    }
    merged->reset(rawCollector);
    return true;
}

bool bindCurrentThread(unsigned cpu, string *error) {
#if defined(__linux__)
    if (cpu >= CPU_SETSIZE) {
        if (error) {
            std::ostringstream oss;
            oss << "CPU " << cpu << " is outside CPU_SETSIZE";
            *error = oss.str();
        }
        return false;
    }

    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(cpu, &cpuset);
    int rv = pthread_setaffinity_np(pthread_self(), sizeof(cpuset), &cpuset);
    if (rv != 0) {
        if (error) {
            std::ostringstream oss;
            oss << "pthread_setaffinity_np failed for CPU " << cpu
                << " with error " << rv;
            *error = oss.str();
        }
        return false;
    }
    return true;
#else
    (void)cpu;
    if (error) {
        *error = "thread affinity is not available on this platform";
    }
    return false;
#endif
}

bool runParallelScan(const hs_database_t *db,
                     const vector<ScratchPtr> &scratches,
                     const vector<DataBlock> &blocks, unsigned rounds,
                     const vector<CollectorPtr> *collectors,
                     const vector<unsigned> *cpuList, bool echoMatches,
                     ScanMode mode, ParallelRunResult *result) {
    const size_t threadCount = scratches.size();
    if (!threadCount) {
        cerr << "no worker scratch allocated\n";
        return false;
    }
    if (collectors && collectors->size() != threadCount) {
        cerr << "collector count does not match worker count\n";
        return false;
    }
    if (cpuList && cpuList->size() != threadCount) {
        cerr << "CPU list does not match worker count\n";
        return false;
    }

    vector<ThreadScanResult> results(threadCount);
    vector<std::thread> threads;
    threads.reserve(threadCount);

    const auto wallStart = std::chrono::steady_clock::now();
    try {
        for (size_t i = 0; i < threadCount; i++) {
            hs_fp_collector_t *collector =
                collectors ? (*collectors)[i].get() : nullptr;
            threads.emplace_back([&, i, collector, echoMatches, mode]() {
                if (cpuList &&
                    !bindCurrentThread((*cpuList)[i], &results[i].error)) {
                    results[i].ok = false;
                    return;
                }
                const auto workerStart = std::chrono::steady_clock::now();
                results[i].ok = scanCorpus(
                    db, scratches[i].get(), blocks, rounds, collector,
                    echoMatches, mode, &results[i].stats, &results[i].error);
                const auto workerEnd = std::chrono::steady_clock::now();
                results[i].seconds = secondsSince(workerStart, workerEnd);
            });
        }
    } catch (const std::exception &e) {
        for (auto &thread : threads) {
            if (thread.joinable()) {
                thread.join();
            }
        }
        cerr << "failed to launch worker thread: " << e.what() << "\n";
        return false;
    }

    for (auto &thread : threads) {
        thread.join();
    }
    const auto wallEnd = std::chrono::steady_clock::now();

    result->stats = RunStats();
    result->seconds = secondsSince(wallStart, wallEnd);
    result->fastestWorkerSeconds = 0.0;
    for (size_t i = 0; i < threadCount; i++) {
        if (!results[i].ok) {
            cerr << "worker " << i << " failed: " << results[i].error << "\n";
            return false;
        }
        addRunStats(&result->stats, results[i].stats);
        if (results[i].seconds > 0.0 &&
            (!result->fastestWorkerSeconds ||
             results[i].seconds < result->fastestWorkerSeconds)) {
            result->fastestWorkerSeconds = results[i].seconds;
        }
    }

    return true;
}

const char *tableName(unsigned int table) {
    switch (table) {
    case HS_FP_TABLE_FLOATING:
        return "floating";
    case HS_FP_TABLE_EOD_ANCHORED:
        return "eod";
    case HS_FP_TABLE_SMALL_BLOCK:
        return "small";
    case HS_FP_TABLE_DELAY_REBUILD:
        return "delay";
    case HS_FP_TABLE_ANCHORED:
        return "anchored";
    default:
        return "unknown";
    }
}

const char *engineName(unsigned int engine) {
    switch (engine) {
    case HS_FP_ENGINE_NOODLE:
        return "noodle";
    case HS_FP_ENGINE_FDR:
        return "fdr";
    case HS_FP_ENGINE_NEO_FDR:
        return "neo_fdr";
    case HS_FP_ENGINE_HAO:
        return "hao";
    case HS_FP_ENGINE_TEDDY:
        return "teddy";
    default:
        return "unknown";
    }
}

string escapedBytes(const unsigned char *bytes, size_t length) {
    std::ostringstream oss;
    const size_t maxShown = 32;
    const size_t shown = std::min(length, maxShown);
    for (size_t i = 0; i < shown; i++) {
        const unsigned char c = bytes[i];
        if (c >= 0x20 && c <= 0x7e && c != '\\' && c != '"') {
            oss << static_cast<char>(c);
        } else {
            oss << "\\x" << hex << setw(2) << setfill('0')
                << static_cast<unsigned int>(c) << dec << setfill(' ');
        }
    }
    if (shown < length) {
        oss << "...";
    }
    return oss.str();
}

string hexBytes(const unsigned char *bytes, size_t length) {
    if (!length) {
        return "";
    }

    std::ostringstream oss;
    oss << "0x";
    for (size_t i = 0; i < length; i++) {
        oss << hex << setw(2) << setfill('0')
            << static_cast<unsigned int>(bytes[i]);
    }
    return oss.str();
}

double falsePositiveRate(const hs_fp_fragment_info_t &fragment) {
    if (!fragment.trigger_count) {
        return 0.0;
    }
    return static_cast<double>(fragment.false_positive_count) /
           static_cast<double>(fragment.trigger_count);
}

double falsePositiveShare(const hs_fp_fragment_info_t &fragment,
                          unsigned long long totalFalsePositive) {
    if (!totalFalsePositive) {
        return 0.0;
    }
    return static_cast<double>(fragment.false_positive_count) /
           static_cast<double>(totalFalsePositive);
}

string formatPercent(double ratio) {
    return formatFixed(ratio * 100.0, 2) + "%";
}

void copyFragment(const hs_fp_fragment_info_t &src, OwnedFragment *dst) {
    dst->info = src;
    dst->bytes.assign(src.bytes, src.bytes + src.length);
    if (src.mask_length) {
        dst->mask.assign(src.mask, src.mask + src.mask_length);
        dst->cmp.assign(src.cmp, src.cmp + src.mask_length);
    } else {
        dst->mask.clear();
        dst->cmp.clear();
    }
    dst->info.bytes = dst->bytes.empty() ? nullptr : dst->bytes.data();
    dst->info.mask = dst->mask.empty() ? nullptr : dst->mask.data();
    dst->info.cmp = dst->cmp.empty() ? nullptr : dst->cmp.data();
}

void appendOwnedFragment(vector<OwnedFragment> *fragments,
                         const hs_fp_fragment_info_t &fragment) {
    fragments->push_back(OwnedFragment());
    copyFragment(fragment, &fragments->back());
}

vector<hs_fp_fragment_info_t>
fragmentInfos(const vector<OwnedFragment> &fragments) {
    vector<hs_fp_fragment_info_t> out;
    out.reserve(fragments.size());
    for (const auto &fragment : fragments) {
        out.push_back(fragment.info);
    }
    return out;
}

void sortFragments(vector<hs_fp_fragment_info_t> *fragments) {
    std::sort(
        fragments->begin(), fragments->end(),
        [](const hs_fp_fragment_info_t &a, const hs_fp_fragment_info_t &b) {
            if (a.false_positive_count != b.false_positive_count) {
                return a.false_positive_count > b.false_positive_count;
            }
            if (a.trigger_count != b.trigger_count) {
                return a.trigger_count > b.trigger_count;
            }
            return a.key < b.key;
        });
}

void printFragmentHeader() {
    cout << left << setw(4) << "#" << setw(20) << "key" << setw(10) << "table"
         << setw(10) << "engine" << right << setw(12) << "trigger" << setw(12)
         << "true" << setw(12) << "false" << setw(10) << "fp_rate" << setw(10)
         << "fp_share" << left << "  " << setw(18) << "msk" << setw(18) << "cmp"
         << "fragment\n";
}

string fragmentKeyString(const hs_fp_fragment_info_t &fragment) {
    std::ostringstream key;
    key << "0x" << hex << setw(16) << setfill('0') << fragment.key << dec
        << setfill(' ');
    return key.str();
}

string quotedBytes(const unsigned char *bytes, size_t length) {
    return "\"" + escapedBytes(bytes, length) + "\"";
}

string quotedHexBytes(const unsigned char *bytes, size_t length) {
    return "\"" + hexBytes(bytes, length) + "\"";
}

void printFragmentRow(size_t index, const hs_fp_fragment_info_t &fragment,
                      unsigned long long totalFalsePositive) {
    const string mask = quotedHexBytes(fragment.mask, fragment.mask_length);
    const string cmp = quotedHexBytes(fragment.cmp, fragment.mask_length);
    const string bytes = quotedBytes(fragment.bytes, fragment.length);

    cout << left << setw(4) << index << setw(20) << fragmentKeyString(fragment)
         << setw(10) << tableName(fragment.table) << setw(10)
         << engineName(fragment.engine) << right << setw(12)
         << fragment.trigger_count << setw(12) << fragment.true_trigger_count
         << setw(12) << fragment.false_positive_count << setw(10)
         << formatPercent(falsePositiveRate(fragment)) << setw(10)
         << formatPercent(falsePositiveShare(fragment, totalFalsePositive))
         << left << "  " << setw(18) << mask << setw(18) << cmp << bytes
         << "\n";
}

void printTopReportFragments(const DumpData &dump, unsigned top) {
    vector<hs_fp_fragment_info_t> fragments =
        fragmentInfos(dump.reportFragments);
    sortFragments(&fragments);

    cout << "\nTop report fragments:\n";
    if (fragments.empty()) {
        cout << "  (none)\n";
        return;
    }

    printFragmentHeader();
    const size_t limit = std::min<size_t>(top, fragments.size());
    for (size_t i = 0; i < limit; i++) {
        printFragmentRow(i + 1, fragments[i],
                         dump.summary.false_positive_count);
    }
}

void printFeedbackFragments(const vector<OwnedFragment> &selected, unsigned top,
                            unsigned long long totalFalsePositive) {
    vector<hs_fp_fragment_info_t> fragments = fragmentInfos(selected);
    sortFragments(&fragments);

    cout << "\nBad fragments selected for feedback:\n";
    if (fragments.empty()) {
        cout << "  (none)\n";
        return;
    }

    printFragmentHeader();
    const size_t limit = std::min<size_t>(top, fragments.size());
    for (size_t i = 0; i < limit; i++) {
        printFragmentRow(i + 1, fragments[i], totalFalsePositive);
    }
}

string csvEscape(const string &value) {
    bool needsQuotes = false;
    for (char c : value) {
        if (c == '"' || c == ',' || c == '\n' || c == '\r') {
            needsQuotes = true;
            break;
        }
    }
    if (!needsQuotes) {
        return value;
    }

    string out = "\"";
    for (char c : value) {
        if (c == '"') {
            out += "\"\"";
        } else {
            out.push_back(c);
        }
    }
    out.push_back('"');
    return out;
}

bool writeFragmentCsv(const string &path,
                      vector<hs_fp_fragment_info_t> fragments,
                      unsigned long long totalFalsePositive) {
    std::ofstream out(path.c_str(), std::ios::binary);
    if (!out.good()) {
        cerr << "Unable to open output file: " << path << "\n";
        return false;
    }

    sortFragments(&fragments);
    out << "key,table,engine,trigger_count,true_trigger_count,"
           "false_positive_count,fp_rate,fp_share,msk,cmp,fragment\n";
    for (const auto &fragment : fragments) {
        const string bytes = escapedBytes(fragment.bytes, fragment.length);
        const string mask = hexBytes(fragment.mask, fragment.mask_length);
        const string cmp = hexBytes(fragment.cmp, fragment.mask_length);
        out << fragmentKeyString(fragment) << ',' << tableName(fragment.table)
            << ',' << engineName(fragment.engine) << ','
            << fragment.trigger_count << ',' << fragment.true_trigger_count
            << ',' << fragment.false_positive_count << ','
            << formatFixed(falsePositiveRate(fragment) * 100.0, 2) << ','
            << formatFixed(
                   falsePositiveShare(fragment, totalFalsePositive) * 100.0, 2)
            << ',' << csvEscape(mask) << ',' << csvEscape(cmp) << ','
            << csvEscape(bytes) << "\n";
    }

    if (!out.good()) {
        cerr << "Unable to write output file: " << path << "\n";
        return false;
    }
    return true;
}

bool dumpReportCsvFile(const DumpData &dump, const string &path) {
    if (path.empty()) {
        return true;
    }
    return writeFragmentCsv(path, fragmentInfos(dump.reportFragments),
                            dump.summary.false_positive_count);
}

bool dumpFeedbackCsvFile(const vector<OwnedFragment> &selected,
                         unsigned long long totalFalsePositive,
                         const string &path) {
    if (path.empty()) {
        return true;
    }
    return writeFragmentCsv(path, fragmentInfos(selected), totalFalsePositive);
}

bool dumpCsvOutputs(const DumpData *dump, const vector<OwnedFragment> &selected,
                    unsigned long long totalFalsePositive, const string &dir) {
    if (dir.empty()) {
        return true;
    }
    if (!ensureDirectory(dir, "CSV output")) {
        return false;
    }
    if (dump &&
        !dumpReportCsvFile(*dump, joinPath(dir, HSPGO_REPORT_CSV_NAME))) {
        return false;
    }
    if (!dumpFeedbackCsvFile(selected, totalFalsePositive,
                             joinPath(dir, HSPGO_FEEDBACK_CSV_NAME))) {
        return false;
    }
    return true;
}

bool prepareOutputDirectories(const Options &opts) {
    if (!opts.reportCsvPath.empty() &&
        !ensureDirectory(opts.reportCsvPath, "CSV output")) {
        return false;
    }
    if (!opts.feedbackBinPath.empty() &&
        !ensureDirectory(opts.feedbackBinPath, "feedback binary output")) {
        return false;
    }
    return true;
}

const uint8_t HSPGO_FEEDBACK_BIN_MAGIC[8] = {'H', 'S', 'P', 'G',
                                             'O', 'F', 'B', '1'};
const uint32_t HSPGO_FEEDBACK_BIN_VERSION = 4;
const uint64_t MAX_FEEDBACK_BIN_SIZE = 128ULL * 1024ULL * 1024ULL;
const uint32_t MAX_SERIALIZED_FRAGMENT_BYTES = 4096;
const uint64_t FNV1A64_OFFSET = 14695981039346656037ULL;
const uint64_t FNV1A64_PRIME = 1099511628211ULL;

uint32_t scanModeCode(ScanMode mode) {
    switch (mode) {
    case ScanMode::STREAMING:
        return 1;
    case ScanMode::BLOCK:
        return 2;
    case ScanMode::VECTORED:
        return 3;
    }
    return 0;
}

uint64_t fnv1a64(const vector<uint8_t> &bytes) {
    uint64_t hash = FNV1A64_OFFSET;
    for (uint8_t byte : bytes) {
        hash ^= byte;
        hash *= FNV1A64_PRIME;
    }
    return hash;
}

void appendBytes(vector<uint8_t> *out, const void *bytes, size_t length) {
    if (!length) {
        return;
    }
    const uint8_t *p = static_cast<const uint8_t *>(bytes);
    out->insert(out->end(), p, p + length);
}

void appendU32(vector<uint8_t> *out, uint32_t value) {
    for (unsigned i = 0; i < 4; i++) {
        out->push_back(static_cast<uint8_t>((value >> (i * 8)) & 0xffU));
    }
}

void appendU64(vector<uint8_t> *out, uint64_t value) {
    for (unsigned i = 0; i < 8; i++) {
        out->push_back(static_cast<uint8_t>((value >> (i * 8)) & 0xffU));
    }
}

void appendString(vector<uint8_t> *out, const string &value) {
    appendU64(out, static_cast<uint64_t>(value.size()));
    appendBytes(out, value.data(), value.size());
}

uint64_t computeSourceFingerprint(const PatternSet &patterns, ScanMode mode,
                                  const string &greyOverrides) {
    vector<uint8_t> payload;
    appendString(&payload, "hspgo-source-v1");
    appendU32(&payload, scanModeCode(mode));
    appendString(&payload, greyOverrides);
    appendU64(&payload, static_cast<uint64_t>(patterns.exprs.size()));
    for (size_t i = 0; i < patterns.exprs.size(); i++) {
        appendU32(&payload, patterns.ids[i]);
        appendU32(&payload, patterns.flags[i]);
        const hs_expr_ext &ext = patterns.ext[i];
        appendU64(&payload, ext.flags);
        appendU64(&payload, ext.min_offset);
        appendU64(&payload, ext.max_offset);
        appendU64(&payload, ext.min_length);
        appendU32(&payload, ext.edit_distance);
        appendU32(&payload, ext.hamming_distance);
        appendString(&payload, patterns.exprs[i]);
    }
    return fnv1a64(payload);
}

bool readU32(const vector<uint8_t> &bytes, size_t *offset, uint32_t *out) {
    if (!offset || !out || *offset > bytes.size() ||
        bytes.size() - *offset < 4) {
        return false;
    }
    uint32_t value = 0;
    for (unsigned i = 0; i < 4; i++) {
        value |= static_cast<uint32_t>(bytes[*offset + i]) << (i * 8);
    }
    *offset += 4;
    *out = value;
    return true;
}

bool readU64(const vector<uint8_t> &bytes, size_t *offset, uint64_t *out) {
    if (!offset || !out || *offset > bytes.size() ||
        bytes.size() - *offset < 8) {
        return false;
    }
    uint64_t value = 0;
    for (unsigned i = 0; i < 8; i++) {
        value |= static_cast<uint64_t>(bytes[*offset + i]) << (i * 8);
    }
    *offset += 8;
    *out = value;
    return true;
}

bool readBytes(const vector<uint8_t> &bytes, size_t *offset, size_t length,
               vector<u8> *out) {
    if (!offset || !out || *offset > bytes.size() ||
        bytes.size() - *offset < length) {
        return false;
    }
    out->assign(bytes.begin() + *offset, bytes.begin() + *offset + length);
    *offset += length;
    return true;
}

bool readBinaryFile(const string &path, vector<uint8_t> *bytes) {
    std::ifstream in(path.c_str(), std::ios::binary);
    if (!in.good()) {
        cerr << "Unable to open feedback binary: " << path << "\n";
        return false;
    }

    in.seekg(0, std::ios::end);
    const std::streamoff end = in.tellg();
    if (end < 0 || static_cast<uint64_t>(end) > MAX_FEEDBACK_BIN_SIZE) {
        cerr << "Feedback binary is too large or unreadable: " << path << "\n";
        return false;
    }
    in.seekg(0, std::ios::beg);

    bytes->resize(static_cast<size_t>(end));
    if (!bytes->empty()) {
        in.read(reinterpret_cast<char *>(bytes->data()),
                static_cast<std::streamsize>(bytes->size()));
    }
    if (!in.good() && !in.eof()) {
        cerr << "Unable to read feedback binary: " << path << "\n";
        return false;
    }
    return true;
}

bool writeBinaryFile(const string &path, const vector<uint8_t> &bytes) {
    std::ofstream out(path.c_str(), std::ios::binary);
    if (!out.good()) {
        cerr << "Unable to open feedback binary output: " << path << "\n";
        return false;
    }
    if (!bytes.empty()) {
        out.write(reinterpret_cast<const char *>(bytes.data()),
                  static_cast<std::streamsize>(bytes.size()));
    }
    if (!out.good()) {
        cerr << "Unable to write feedback binary output: " << path << "\n";
        return false;
    }
    return true;
}

bool appendSerializedFragment(vector<uint8_t> *payload,
                              const hs_fp_fragment_info_t &fragment) {
    if (!fragment.bytes || !fragment.length ||
        fragment.length > MAX_SERIALIZED_FRAGMENT_BYTES ||
        fragment.mask_length > MAX_SERIALIZED_FRAGMENT_BYTES ||
        (fragment.mask_length && (!fragment.mask || !fragment.cmp))) {
        cerr << "Invalid feedback fragment cannot be serialized\n";
        return false;
    }

    appendU64(payload, fragment.key);
    appendU32(payload, fragment.table);
    appendU32(payload, fragment.engine);
    appendU32(payload, fragment.flags);
    appendU32(payload, static_cast<uint32_t>(fragment.length));
    appendU32(payload, static_cast<uint32_t>(fragment.mask_length));
    appendU64(payload, fragment.trigger_count);
    appendU64(payload, fragment.true_trigger_count);
    appendU64(payload, fragment.false_positive_count);
    appendBytes(payload, fragment.bytes, fragment.length);
    appendBytes(payload, fragment.mask, fragment.mask_length);
    appendBytes(payload, fragment.cmp, fragment.mask_length);
    return true;
}

bool dumpFeedbackBin(const vector<OwnedFragment> &selected, const string &path,
                     ScanMode mode, uint64_t sourceFingerprint,
                     unsigned long long totalFalsePositive) {
    if (path.empty()) {
        return true;
    }
    if (selected.size() > std::numeric_limits<uint32_t>::max()) {
        cerr << "Too many feedback fragments for feedback binary output\n";
        return false;
    }

    vector<uint8_t> payload;
    appendU32(&payload, scanModeCode(mode));
    appendU32(&payload, 0);
    appendU64(&payload, sourceFingerprint);
    appendU64(&payload, totalFalsePositive);
    appendU32(&payload, static_cast<uint32_t>(selected.size()));
    appendU32(&payload, 0);
    for (const auto &fragment : selected) {
        if (!appendSerializedFragment(&payload, fragment.info)) {
            return false;
        }
    }

    vector<uint8_t> file;
    appendBytes(&file, HSPGO_FEEDBACK_BIN_MAGIC,
                sizeof(HSPGO_FEEDBACK_BIN_MAGIC));
    appendU32(&file, HSPGO_FEEDBACK_BIN_VERSION);
    appendU64(&file, static_cast<uint64_t>(payload.size()));
    appendU64(&file, fnv1a64(payload));
    appendBytes(&file, payload.data(), payload.size());
    return writeBinaryFile(path, file);
}

bool dumpFeedbackBinToDir(const vector<OwnedFragment> &selected,
                          const string &dir, ScanMode mode,
                          uint64_t sourceFingerprint,
                          unsigned long long totalFalsePositive) {
    if (dir.empty()) {
        return true;
    }
    if (!ensureDirectory(dir, "feedback binary output")) {
        return false;
    }
    return dumpFeedbackBin(selected, joinPath(dir, HSPGO_FEEDBACK_BIN_NAME),
                           mode, sourceFingerprint, totalFalsePositive);
}

struct OwnedImportFragment {
    hs_fp_feedback_import_fragment fragment = {};
    vector<u8> bytes;
    vector<u8> mask;
    vector<u8> cmp;
};

bool readSerializedFragment(const vector<uint8_t> &payload, size_t *offset,
                            OwnedImportFragment *out) {
    uint64_t value64 = 0;
    uint32_t value32 = 0;
    if (!readU64(payload, offset, &value64)) {
        return false;
    }
    out->fragment.key = value64;
    if (!readU32(payload, offset, &out->fragment.table) ||
        !readU32(payload, offset, &out->fragment.engine) ||
        !readU32(payload, offset, &out->fragment.flags) ||
        !readU32(payload, offset, &value32)) {
        return false;
    }
    out->fragment.length = value32;
    if (!readU32(payload, offset, &value32)) {
        return false;
    }
    out->fragment.mask_length = value32;
    if (out->fragment.length > MAX_SERIALIZED_FRAGMENT_BYTES ||
        out->fragment.mask_length > MAX_SERIALIZED_FRAGMENT_BYTES) {
        return false;
    }

    uint64_t triggerCount = 0;
    uint64_t trueTriggerCount = 0;
    uint64_t falsePositiveCount = 0;
    if (!readU64(payload, offset, &triggerCount) ||
        !readU64(payload, offset, &trueTriggerCount) ||
        !readU64(payload, offset, &falsePositiveCount)) {
        return false;
    }
    out->fragment.trigger_count = triggerCount;
    out->fragment.true_trigger_count = trueTriggerCount;
    out->fragment.false_positive_count = falsePositiveCount;

    if (!readBytes(payload, offset, out->fragment.length, &out->bytes) ||
        !readBytes(payload, offset, out->fragment.mask_length, &out->mask) ||
        !readBytes(payload, offset, out->fragment.mask_length, &out->cmp)) {
        return false;
    }

    out->fragment.bytes = out->bytes.empty() ? nullptr : out->bytes.data();
    out->fragment.mask = out->mask.empty() ? nullptr : out->mask.data();
    out->fragment.cmp = out->cmp.empty() ? nullptr : out->cmp.data();
    return true;
}

bool loadFeedbackBin(const string &path, ScanMode mode,
                     uint64_t sourceFingerprint, FeedbackPtr *feedback,
                     vector<OwnedFragment> *selected,
                     unsigned long long *totalFalsePositiveOut) {
    vector<uint8_t> file;
    if (!readBinaryFile(path, &file)) {
        return false;
    }

    const size_t headerSize = sizeof(HSPGO_FEEDBACK_BIN_MAGIC) + 4 + 8 + 8;
    if (file.size() < headerSize ||
        !std::equal(HSPGO_FEEDBACK_BIN_MAGIC,
                    HSPGO_FEEDBACK_BIN_MAGIC + sizeof(HSPGO_FEEDBACK_BIN_MAGIC),
                    file.begin())) {
        cerr << "Feedback binary has invalid magic: " << path << "\n";
        return false;
    }

    size_t offset = sizeof(HSPGO_FEEDBACK_BIN_MAGIC);
    uint32_t version = 0;
    uint64_t payloadSize = 0;
    uint64_t checksum = 0;
    if (!readU32(file, &offset, &version) ||
        !readU64(file, &offset, &payloadSize) ||
        !readU64(file, &offset, &checksum)) {
        cerr << "Feedback binary header is truncated: " << path << "\n";
        return false;
    }
    if (version != HSPGO_FEEDBACK_BIN_VERSION) {
        cerr << "Feedback binary version " << version
             << " is unsupported; regenerate feedback.bin with this hspgo\n";
        return false;
    }
    if (payloadSize != file.size() - offset) {
        cerr << "Feedback binary header is invalid: " << path << "\n";
        return false;
    }

    vector<uint8_t> payload(file.begin() + offset, file.end());
    if (fnv1a64(payload) != checksum) {
        cerr << "Feedback binary checksum mismatch: " << path << "\n";
        return false;
    }

    offset = 0;
    uint32_t storedMode = 0;
    uint32_t reservedHeader = 0;
    uint64_t storedSourceFingerprint = 0;
    uint64_t totalFalsePositive = 0;
    uint32_t fragmentCount = 0;
    uint32_t reserved = 0;
    if (!readU32(payload, &offset, &storedMode) ||
        !readU32(payload, &offset, &reservedHeader) ||
        !readU64(payload, &offset, &storedSourceFingerprint) ||
        !readU64(payload, &offset, &totalFalsePositive) ||
        !readU32(payload, &offset, &fragmentCount) ||
        !readU32(payload, &offset, &reserved)) {
        cerr << "Feedback binary payload is truncated: " << path << "\n";
        return false;
    }
    if (reservedHeader != 0) {
        cerr << "Feedback binary reserved header field is not zero: " << path
             << "\n";
        return false;
    }
    if (reserved != 0) {
        cerr << "Feedback binary reserved field is not zero: " << path << "\n";
        return false;
    }
    if (storedMode != scanModeCode(mode)) {
        cerr << "Feedback binary scan mode does not match current hspgo mode\n";
        return false;
    }
    if (storedSourceFingerprint != sourceFingerprint) {
        cerr << "Feedback binary source fingerprint "
             << formatHex64(storedSourceFingerprint)
             << " does not match current source fingerprint "
             << formatHex64(sourceFingerprint) << "\n";
        return false;
    }
    const size_t minFragmentPayloadSize = 53;
    if (fragmentCount &&
        fragmentCount > (payload.size() - offset) / minFragmentPayloadSize) {
        cerr << "Feedback binary fragment count exceeds payload capacity: "
             << path << "\n";
        return false;
    }

    vector<OwnedImportFragment> owned(fragmentCount);
    vector<hs_fp_feedback_import_fragment> imports(fragmentCount);
    if (selected) {
        selected->clear();
        selected->reserve(fragmentCount);
    }
    for (uint32_t i = 0; i < fragmentCount; i++) {
        if (!readSerializedFragment(payload, &offset, &owned[i])) {
            cerr << "Feedback binary fragment " << i << " is invalid\n";
            return false;
        }
        imports[i] = owned[i].fragment;
        if (selected) {
            OwnedFragment copy;
            copy.info.key = owned[i].fragment.key;
            copy.info.table = owned[i].fragment.table;
            copy.info.engine = owned[i].fragment.engine;
            copy.info.flags = owned[i].fragment.flags;
            copy.info.length = owned[i].fragment.length;
            copy.info.mask_length = owned[i].fragment.mask_length;
            copy.info.trigger_count = owned[i].fragment.trigger_count;
            copy.info.true_trigger_count = owned[i].fragment.true_trigger_count;
            copy.info.false_positive_count =
                owned[i].fragment.false_positive_count;
            copy.bytes = owned[i].bytes;
            copy.mask = owned[i].mask;
            copy.cmp = owned[i].cmp;
            copy.info.bytes = copy.bytes.empty() ? nullptr : copy.bytes.data();
            copy.info.mask = copy.mask.empty() ? nullptr : copy.mask.data();
            copy.info.cmp = copy.cmp.empty() ? nullptr : copy.cmp.data();
            selected->push_back(move(copy));
        }
    }
    if (offset != payload.size()) {
        cerr << "Feedback binary has trailing payload bytes: " << path << "\n";
        return false;
    }

    hs_fp_feedback_t *rawFeedback = nullptr;
    hs_error_t err = hs_fp_feedback_create_from_fragments(
        imports.empty() ? nullptr : imports.data(), fragmentCount,
        &rawFeedback);
    if (err != HS_SUCCESS) {
        cerr << "hs_fp_feedback_create_from_fragments failed with error " << err
             << "\n";
        return false;
    }
    feedback->reset(rawFeedback);
    if (totalFalsePositiveOut) {
        *totalFalsePositiveOut = totalFalsePositive;
    }
    return true;
}

bool loadFeedbackBinFromDir(const string &dir, ScanMode mode,
                            uint64_t sourceFingerprint, FeedbackPtr *feedback,
                            vector<OwnedFragment> *selected,
                            unsigned long long *totalFalsePositive) {
    const string trimmed = trimTrailingSeparators(dir);
    if (trimmed.empty()) {
        cerr << "feedback binary input directory must not be empty\n";
        return false;
    }
    if (!directoryExists(trimmed)) {
        cerr << "feedback binary input directory does not exist: " << trimmed
             << "\n";
        return false;
    }
    return loadFeedbackBin(joinPath(trimmed, HSPGO_FEEDBACK_BIN_NAME), mode,
                           sourceFingerprint, feedback, selected,
                           totalFalsePositive);
}

void printDumpSummaryWithTitle(const char *title, const DumpData &dump) {
    const hs_fp_feedback_dump_summary_t &summary = dump.summary;
    cout << "\n" << title << ":\n";
    printField("Fragments:",
               static_cast<unsigned long long>(summary.fragment_count));
    printField("Triggers:", summary.trigger_count);
    printField("True triggers:", summary.true_trigger_count);
    printField("False positives:", summary.false_positive_count);
}

void printReportSummary(const DumpData &dump) {
    printDumpSummaryWithTitle("Collection report", dump);
}

void printFeedbackSummary(const DumpData &dump) {
    cout << "\nFeedback summary:\n";
    printField("Bad fragments:", static_cast<unsigned long long>(
                                     dump.summary.bad_fragment_count));
    printField("Source false positives:", dump.summary.false_positive_count);
}

string formatScaledRatio(unsigned long long scaled) {
    if (scaled >= HS_FP_FEEDBACK_RATE_SCALE) {
        return "1";
    }

    const unsigned long long whole = scaled / HS_FP_FEEDBACK_RATE_SCALE;
    const unsigned long long frac = scaled % HS_FP_FEEDBACK_RATE_SCALE;
    if (!frac) {
        return whole ? "1" : "0";
    }

    std::ostringstream oss;
    oss << whole << "." << setw(HSPGO_RATE_SCALE_DECIMALS) << setfill('0')
        << frac;
    string out = oss.str();
    while (out.back() == '0') {
        out.pop_back();
    }
    return out;
}

string feedbackThresholdString(const hs_fp_feedback_params_t &params) {
    const unsigned long long minTrigger =
        (params.flags & HS_FP_FEEDBACK_PARAM_MIN_TRIGGER_COUNT)
            ? params.min_trigger_count
            : HS_FP_FEEDBACK_DEFAULT_MIN_TRIGGER_COUNT;
    const unsigned long long minFp =
        (params.flags & HS_FP_FEEDBACK_PARAM_MIN_FALSE_POSITIVE_COUNT)
            ? params.min_false_positive_count
            : HS_FP_FEEDBACK_DEFAULT_MIN_FALSE_POSITIVE_COUNT;
    const unsigned long long fpRate =
        (params.flags & HS_FP_FEEDBACK_PARAM_MIN_FALSE_POSITIVE_RATE)
            ? params.min_false_positive_rate
            : HS_FP_FEEDBACK_DEFAULT_MIN_FALSE_POSITIVE_RATE;
    const unsigned long long wasteShare =
        (params.flags & HS_FP_FEEDBACK_PARAM_MIN_WASTE_SHARE)
            ? params.min_waste_share
            : HS_FP_FEEDBACK_DEFAULT_MIN_WASTE_SHARE;
    const unsigned topK =
        (params.flags & HS_FP_FEEDBACK_PARAM_MAX_BAD_FRAGMENTS)
            ? params.max_bad_fragments
            : HS_FP_FEEDBACK_DEFAULT_MAX_BAD_FRAGMENTS;

    vector<string> parts;
    parts.push_back("trigger>=" + formatCount(minTrigger));
    parts.push_back("false>=" + formatCount(minFp));
    parts.push_back("fp_rate>=" + formatScaledRatio(fpRate));
    parts.push_back("fp_share>=" + formatScaledRatio(wasteShare));
    if (topK) {
        parts.push_back("topk=" + formatCount(topK));
    } else {
        parts.push_back("topk=all");
    }

    std::ostringstream oss;
    for (size_t i = 0; i < parts.size(); i++) {
        if (i) {
            oss << "; ";
        }
        oss << parts[i];
    }
    return oss.str();
}

void dumpSummaryCallback(const hs_fp_feedback_dump_summary_t *summary,
                         void *context) {
    if (!summary || !context) {
        return;
    }
    DumpData *dump = static_cast<DumpData *>(context);
    dump->summary = *summary;
}

void dumpFragmentCallback(const hs_fp_fragment_info_t *fragment,
                          unsigned int selected, void *context) {
    if (!fragment || !context) {
        return;
    }
    DumpData *dump = static_cast<DumpData *>(context);
    appendOwnedFragment(&dump->reportFragments, *fragment);
    if (selected) {
        appendOwnedFragment(&dump->feedbackFragments, *fragment);
    }
}

bool buildFeedbackFromCollector(hs_fp_collector_t *collector,
                                const hs_fp_feedback_params_t &params,
                                DumpData *dump, FeedbackPtr *feedback) {
    if (!dump) {
        return false;
    }
    dump->summary = hs_fp_feedback_dump_summary_t();
    dump->reportFragments.clear();
    dump->feedbackFragments.clear();

    hs_fp_feedback_t *rawFeedback = nullptr;
    hs_fp_feedback_dump_callbacks_t callbacks = {};
    callbacks.on_summary = dumpSummaryCallback;
    callbacks.on_fragment = dumpFragmentCallback;
    hs_error_t err = hs_fp_collector_to_feedback_with_dump(
        collector, &params, &callbacks, dump, &rawFeedback);
    if (err != HS_SUCCESS) {
        cerr << "hs_fp_collector_to_feedback_with_dump failed with error "
             << err << "\n";
        return false;
    }
    feedback->reset(rawFeedback);
    return true;
}

bool createCompileContext(const hs_fp_feedback_t *feedback,
                          CompileContextPtr *ctx) {
    hs_compile_context_t *rawCtx = nullptr;
    hs_error_t err = hs_compile_context_create(&rawCtx);
    if (err != HS_SUCCESS) {
        cerr << "hs_compile_context_create failed with error " << err << "\n";
        return false;
    }
    ctx->reset(rawCtx);

    err = hs_compile_context_set_fp_feedback(ctx->get(), feedback);
    if (err != HS_SUCCESS) {
        cerr << "hs_compile_context_set_fp_feedback failed with error " << err
             << "\n";
        return false;
    }

    return true;
}

void printCompileContextDiagnostics(const hs_compile_context_t *ctx) {
    struct CheckpointName {
        unsigned int id;
        const char *name;
        bool canBlock;
    };
    const CheckpointName checkpoints[] = {
        {HS_FP_COMPILE_CHECKPOINT_SHORTCUT_LITERAL, "shortcut_literal", true},
        {HS_FP_COMPILE_CHECKPOINT_LITERAL_SPLIT, "literal_split", true},
        {HS_FP_COMPILE_CHECKPOINT_ANCHORED_ACYCLIC, "anchored_acyclic", true},
        {HS_FP_COMPILE_CHECKPOINT_SMALL_LITERAL_SET, "small_literal_set", true},
        {HS_FP_COMPILE_CHECKPOINT_MASKED_LITERAL, "masked_literal", true},
        {HS_FP_COMPILE_CHECKPOINT_VIOLET_SPLIT, "violet_split", true},
        {HS_FP_COMPILE_CHECKPOINT_SOMBE_LITERAL, "sombe_literal", true},
        {HS_FP_COMPILE_CHECKPOINT_REWRITE_EOD_TO_FLOATING,
         "rewrite_eod_to_floating", false},
        {HS_FP_COMPILE_CHECKPOINT_REWRITE_ANCHORED_REHOME,
         "rewrite_anchored_rehome", false},
        {HS_FP_COMPILE_CHECKPOINT_REWRITE_FLOOD_SUFFIX, "rewrite_flood_suffix",
         false},
        {HS_FP_COMPILE_CHECKPOINT_REWRITE_SMALL_BLOCK, "rewrite_small_block",
         false},
        {HS_FP_COMPILE_CHECKPOINT_MATCHER_BUILD, "matcher_build", false},
    };

    unsigned long long blockChecked = 0;
    unsigned long long blockHit = 0;
    unsigned long long blocked = 0;
    unsigned long long passed = 0;
    for (const auto &checkpoint : checkpoints) {
        hs_compile_context_checkpoint_info_t info = {};
        hs_error_t err =
            hs_compile_context_get_checkpoint_info(ctx, checkpoint.id, &info);
        if (err != HS_SUCCESS) {
            continue;
        }
        if (checkpoint.canBlock) {
            blockChecked += info.checked_count;
        }
        blockHit += info.hit_count;
        blocked += info.blocked_count;
        passed += info.passed_count;
    }

    const unsigned long long observeChecked =
        hs_compile_context_observe_checked_count(ctx);
    const unsigned long long observeHit =
        hs_compile_context_observe_hit_count(ctx);
    const unsigned long long observeMissing =
        observeChecked > observeHit ? observeChecked - observeHit : 0;

    cout << "\nCompile feedback diagnostics:\n";
    printField("Observe checked:", observeChecked);
    printField("Observe hit:", observeHit);
    printField("Block checked:", blockChecked);
    printField("Blocked:", blocked);

    cout << "\nCompile feedback consumption:\n";
    printField("Feedback entries:", observeChecked);
    printField("Observed in new DB:", observeHit);
    printField("Not found in new DB:", observeMissing);
    printField("Hit candidates:", blockHit);
    printField("Blocked candidates:", blocked);
    printField("Passed candidates:", passed);

    const int checkpointNameWidth = 28;
    const int checkpointCountWidth = 14;
    cout << "\nCompile feedback checkpoints:\n";
    cout << "Note: rewrite_* and matcher_build are diagnostic-only; hits are "
            "counted as passed.\n";
    cout << left << setw(checkpointNameWidth) << "checkpoint" << right
         << setw(checkpointCountWidth) << "checked"
         << setw(checkpointCountWidth) << "hit" << setw(checkpointCountWidth)
         << "blocked" << setw(checkpointCountWidth) << "passed" << "\n";
    for (const auto &checkpoint : checkpoints) {
        hs_compile_context_checkpoint_info_t info = {};
        hs_error_t err =
            hs_compile_context_get_checkpoint_info(ctx, checkpoint.id, &info);
        if (err != HS_SUCCESS) {
            continue;
        }
        cout << left << setw(checkpointNameWidth) << checkpoint.name << right
             << setw(checkpointCountWidth) << formatCount(info.checked_count)
             << setw(checkpointCountWidth) << formatCount(info.hit_count)
             << setw(checkpointCountWidth) << formatCount(info.blocked_count)
             << setw(checkpointCountWidth) << formatCount(info.passed_count)
             << "\n";
    }
}

double secondsSince(const std::chrono::steady_clock::time_point &start,
                    const std::chrono::steady_clock::time_point &end) {
    std::chrono::duration<double> elapsed = end - start;
    return elapsed.count();
}

double throughputMbit(const RunStats &stats, double seconds) {
    if (seconds <= 0.0) {
        return 0.0;
    }
    return static_cast<double>(stats.bytes) * 8.0 / seconds / 1000000.0;
}

double throughputMbit(unsigned long long bytes, double seconds) {
    if (seconds <= 0.0) {
        return 0.0;
    }
    return static_cast<double>(bytes) * 8.0 / seconds / 1000000.0;
}

void printDatabaseStats(const char *title, const DatabaseStats &stats) {
    cout << "\n" << title << ":\n";
    printField("Signatures:", stats.signatures);
    printField("Ultrascan info:", stats.info);
    printField("Expression count:",
               static_cast<unsigned long long>(stats.expressionCount));
    printField("Bytecode size:", formatCount(static_cast<unsigned long long>(
                                     stats.bytecodeSize)) +
                                     " bytes");
    printField("Database CRC:", formatHex32(stats.crc32));
    printField("Scratch size:",
               formatCount(static_cast<unsigned long long>(stats.scratchSize)) +
                   " bytes");
    printField("Compile time:",
               formatFixed(stats.compileSeconds, 3) + " seconds");
    printField("Peak heap usage:",
               formatCount(static_cast<unsigned long long>(stats.peakHeap)) +
                   " bytes");
}

void printHspgoConfig(const Options &opts, uint64_t sourceFingerprint) {
    cout << "\nHSPGO feedback configuration:\n";
    string workers = formatCount(opts.threadCount);
    if (!opts.threadSpec.empty()) {
        workers += " (-T " + opts.threadSpec + ")";
    }
    printField("Scan mode:", scanModeName(opts.scanMode));
    printField("Threads:", workers);
    printField("Baseline rounds:",
               static_cast<unsigned long long>(opts.baselineRounds));
    printField("Measurement rounds:",
               static_cast<unsigned long long>(opts.measureRounds));
    printField("Source fingerprint:", formatHex64(sourceFingerprint));
    if (!opts.feedbackImportPath.empty()) {
        printField("Feedback source:", "binary import");
        printField("Feedback input dir:", opts.feedbackImportPath);
        printField("Feedback thresholds:", "not used for imported feedback");
    } else {
        printField("Feedback source:", "collector");
        printField("Feedback thresholds:",
                   feedbackThresholdString(opts.feedbackParams));
    }
    if (!opts.reportCsvPath.empty()) {
        printField("CSV output dir:", opts.reportCsvPath);
    }
    if (!opts.feedbackBinPath.empty()) {
        printField("Feedback output dir:", opts.feedbackBinPath);
    }
    if (opts.echoMatches) {
        printField("Echo matches:", "optimized measurement only");
    }
    if (!opts.greyOverrides.empty()) {
        printField("Grey overrides:", opts.greyOverrides);
    }
}

void printCorpusLayoutNote(ScanMode mode, const vector<DataBlock> &blocks) {
    if (mode == ScanMode::BLOCK) {
        return;
    }

    const size_t groups = streamSlotCount(blocks);
    if (groups != blocks.size()) {
        return;
    }

    if (mode == ScanMode::STREAMING) {
        printField("Mode note:",
                   "one block per stream; open/close overhead may dominate");
    } else if (mode == ScanMode::VECTORED) {
        printField("Mode note:",
                   "one block per vector; batching benefit is not amortized");
    }
}

void printScanSummary(const char *title, const vector<DataBlock> &blocks,
                      const ParallelRunResult &result, unsigned rounds,
                      unsigned threads, ScanMode mode) {
    const unsigned long long bytesPerRun = corpusBytes(blocks);
    const unsigned long long blockCount =
        static_cast<unsigned long long>(blocks.size());
    string corpusShape = formatCount(blockCount) + " blocks";
    if (mode == ScanMode::STREAMING) {
        corpusShape += " in " +
                       formatCount(static_cast<unsigned long long>(
                           streamSlotCount(blocks))) +
                       " streams";
    } else if (mode == ScanMode::VECTORED) {
        corpusShape += " in " +
                       formatCount(static_cast<unsigned long long>(
                           streamSlotCount(blocks))) +
                       " vectors";
    }
    const unsigned long long iterations =
        static_cast<unsigned long long>(rounds) * threads;
    const double matchesPerIteration =
        iterations ? static_cast<double>(result.stats.matches) / iterations
                   : 0.0;
    const double matchRate =
        bytesPerRun ? matchesPerIteration * 1024.0 / bytesPerRun : 0.0;
    const double blockRate =
        result.seconds > 0.0
            ? static_cast<double>(result.stats.scanCalls) / result.seconds
            : 0.0;
    const unsigned long long workerBytes = bytesPerRun * rounds;

    cout << "\n" << title << ":\n";
    printField("Time spent scanning:",
               formatFixed(result.seconds, 3) + " seconds");
    printField("Corpus size:",
               formatCount(bytesPerRun) + " bytes (" + corpusShape + ")");
    printField("Matches per iteration:", formatFixed(matchesPerIteration, 0) +
                                             " (" + formatFixed(matchRate, 3) +
                                             " matches/kilobyte)");
    printField("Overall block rate:",
               formatFixedWithCommas(blockRate, 2) + " blocks/sec");
    printField(
        "Mean throughput (overall):",
        formatFixedWithCommas(throughputMbit(result.stats, result.seconds), 2) +
            " Mbit/sec");
    printField(
        "Max throughput (per core):",
        formatFixedWithCommas(
            throughputMbit(workerBytes, result.fastestWorkerSeconds), 2) +
            " Mbit/sec");
}

void printOptimizedBaselineRatio(const ParallelRunResult &baseline,
                                 const ParallelRunResult &optimized) {
    const double baselineThroughput =
        throughputMbit(baseline.stats, baseline.seconds);
    const double optimizedThroughput =
        throughputMbit(optimized.stats, optimized.seconds);
    if (baselineThroughput <= 0.0) {
        return;
    }

    const double ratio = optimizedThroughput * 100.0 / baselineThroughput;
    printField("Optimized/Baseline:", formatFixed(ratio, 2) + "%");
}
} // namespace

int HS_CDECL main(int argc, char **argv) {
    std::setlocale(LC_ALL, "");

    Options opts;
    if (!processArgs(argc, argv, &opts)) {
        return 1;
    }

    PatternSet patterns;
    if (!loadPatternSet(opts, &patterns)) {
        return 1;
    }

    const uint64_t sourceFingerprint =
        computeSourceFingerprint(patterns, opts.scanMode, opts.greyOverrides);
    FeedbackPtr feedback;
    DumpData dump;
    unsigned long long feedbackTotalFalsePositive = 0;

    cout << "hspgo feedback demo\n";
    printHspgoConfig(opts, sourceFingerprint);
    cout.flush();

    if (!opts.feedbackImportPath.empty()) {
        if (!loadFeedbackBinFromDir(opts.feedbackImportPath, opts.scanMode,
                                    sourceFingerprint, &feedback,
                                    &dump.feedbackFragments,
                                    &feedbackTotalFalsePositive)) {
            return 1;
        }
        dump.summary.bad_fragment_count =
            static_cast<unsigned>(dump.feedbackFragments.size());
        dump.summary.false_positive_count = feedbackTotalFalsePositive;
        cout << "\nFeedback import: loaded reusable feedback binary.\n";
        if (opts.showSummaries) {
            printFeedbackSummary(dump);
        }
        if (opts.top) {
            printFeedbackFragments(dump.feedbackFragments, opts.top,
                                   dump.summary.false_positive_count);
        }
    }

    if (!prepareOutputDirectories(opts)) {
        return 1;
    }

    vector<DataBlock> blocks;
    if (!loadCorpus(opts, &blocks)) {
        return 1;
    }

    const bool needBaselineDb =
        opts.feedbackImportPath.empty() || opts.baselineRounds;
    DatabasePtr baselineDb;
    vector<ScratchPtr> baselineScratches;
    DatabaseStats baselineDbStats;
    if (needBaselineDb) {
        const auto baselineCompileStart = std::chrono::steady_clock::now();
        if (!compileDatabase(patterns, opts.scanMode, nullptr, &baselineDb)) {
            return 1;
        }
        const auto baselineCompileEnd = std::chrono::steady_clock::now();
        if (!allocScratches(baselineDb.get(), opts.threadCount,
                            &baselineScratches)) {
            return 1;
        }

        if (!queryDatabaseStats(
                baselineDb.get(), baselineScratches[0].get(),
                signatureName(opts), patterns.exprs.size(),
                secondsSince(baselineCompileStart, baselineCompileEnd),
                &baselineDbStats)) {
            return 1;
        }
    }

    printCorpusLayoutNote(opts.scanMode, blocks);
    if (needBaselineDb) {
        printDatabaseStats("Baseline database", baselineDbStats);
    } else {
        cout << "\nBaseline database: skipped (binary feedback import).\n";
    }

    ParallelRunResult baselineResult;
    bool haveBaselineResult = false;
    if (opts.baselineRounds && baselineDb) {
        if (!runParallelScan(baselineDb.get(), baselineScratches, blocks,
                             opts.baselineRounds, nullptr,
                             opts.cpuList.empty() ? nullptr : &opts.cpuList,
                             false, opts.scanMode, &baselineResult)) {
            return 1;
        }
        printScanSummary("Baseline scan", blocks, baselineResult,
                         opts.baselineRounds, opts.threadCount, opts.scanMode);
        haveBaselineResult = true;
    }

    vector<CollectorPtr> workerCollectors;
    CollectorPtr collector;
    if (opts.feedbackImportPath.empty()) {
        if (!createCollectors(baselineDb.get(), opts.threadCount,
                              &workerCollectors)) {
            return 1;
        }

        ParallelRunResult collectResult;
        if (!runParallelScan(baselineDb.get(), baselineScratches, blocks,
                             opts.collectRounds, &workerCollectors,
                             opts.cpuList.empty() ? nullptr : &opts.cpuList,
                             false, opts.scanMode, &collectResult)) {
            return 1;
        }

        if (!mergeCollectors(baselineDb.get(), workerCollectors, &collector)) {
            return 1;
        }

        if (!buildFeedbackFromCollector(collector.get(), opts.feedbackParams,
                                        &dump, &feedback)) {
            return 1;
        }

        if (opts.showSummaries) {
            printReportSummary(dump);
            printFeedbackSummary(dump);
        }
        if (opts.top) {
            printTopReportFragments(dump, opts.top);
            printFeedbackFragments(dump.feedbackFragments, opts.top,
                                   dump.summary.false_positive_count);
        }
    }

    if (!dumpCsvOutputs(dump.reportFragments.empty() ? nullptr : &dump,
                        dump.feedbackFragments,
                        dump.summary.false_positive_count,
                        opts.reportCsvPath)) {
        return 1;
    }
    if (!dumpFeedbackBinToDir(dump.feedbackFragments, opts.feedbackBinPath,
                              opts.scanMode, sourceFingerprint,
                              dump.summary.false_positive_count)) {
        return 1;
    }

    CompileContextPtr compileCtx;
    if (!createCompileContext(feedback.get(), &compileCtx)) {
        return 1;
    }

    DatabasePtr optimizedDb;
    const auto optimizedCompileStart = std::chrono::steady_clock::now();
    if (!compileDatabase(patterns, opts.scanMode, compileCtx.get(),
                         &optimizedDb)) {
        return 1;
    }
    const auto optimizedCompileEnd = std::chrono::steady_clock::now();
    if (opts.showDiagnostics) {
        printCompileContextDiagnostics(compileCtx.get());
    }

    vector<ScratchPtr> optimizedScratches;
    if (!allocScratches(optimizedDb.get(), opts.threadCount,
                        &optimizedScratches)) {
        return 1;
    }

    DatabaseStats optimizedDbStats;
    if (!queryDatabaseStats(
            optimizedDb.get(), optimizedScratches[0].get(), signatureName(opts),
            patterns.exprs.size(),
            secondsSince(optimizedCompileStart, optimizedCompileEnd),
            &optimizedDbStats)) {
        return 1;
    }
    printDatabaseStats("Feedback database", optimizedDbStats);

    workerCollectors.clear();
    collector.reset();
    baselineScratches.clear();
    baselineDb.reset();
    cout << "\nHot switch: active database replaced by feedback-compiled DB.\n";

    ParallelRunResult measureResult;
    if (!runParallelScan(optimizedDb.get(), optimizedScratches, blocks,
                         opts.measureRounds, nullptr,
                         opts.cpuList.empty() ? nullptr : &opts.cpuList,
                         opts.echoMatches, opts.scanMode, &measureResult)) {
        return 1;
    }
    printScanSummary("Optimized measurement", blocks, measureResult,
                     opts.measureRounds, opts.threadCount, opts.scanMode);
    if (haveBaselineResult) {
        printOptimizedBaselineRatio(baselineResult, measureResult);
    }

    return 0;
}
