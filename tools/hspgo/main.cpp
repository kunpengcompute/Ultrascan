/*
 * Copyright (c) 2026, Intel Corporation
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *  * Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 *  * Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *  * Neither the name of Intel Corporation nor the names of its contributors
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
#include "heapstats.h"
#include "hs.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <climits>
#include <clocale>
#include <cstdlib>
#include <cstring>
#include <cctype>
#include <exception>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <limits>
#include <memory>
#include <stdint.h>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <getopt.h>

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

struct Options {
    string exprPath;
    string corpusPath;
    string reportCsvPath;
    string feedbackCsvPath;
    string threadSpec;
    string greyOverrides;
    vector<unsigned> cpuList;
    unsigned baselineRounds = 1;
    unsigned collectRounds = 3;
    unsigned measureRounds = 5;
    unsigned threadCount = 1;
    unsigned top = 0;
    bool showSummaries = false;
    bool showDiagnostics = false;
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
    void operator()(hs_database_t *db) const {
        hs_free_database(db);
    }
};

struct ScratchDeleter {
    void operator()(hs_scratch_t *scratch) const {
        hs_free_scratch(scratch);
    }
};

struct CollectorDeleter {
    void operator()(hs_fp_collector_t *collector) const {
        hs_fp_collector_free(collector);
    }
};

struct ReportDeleter {
    void operator()(hs_fp_report_t *report) const {
        hs_fp_report_free(report);
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
using ReportPtr = std::unique_ptr<hs_fp_report_t, ReportDeleter>;
using FeedbackPtr = std::unique_ptr<hs_fp_feedback_t, FeedbackDeleter>;
using CompileContextPtr =
    std::unique_ptr<hs_compile_context_t, CompileContextDeleter>;

void usage(const char *error) {
    cout << "Usage: hspgo [OPTIONS...]\n\n"
         << "Options:\n\n"
         << "  -h, --help              Display help and exit.\n"
         << "  -G OVERRIDES            Overrides for the grey box.\n"
         << "  -e PATH                 Load hsbench expression file/directory.\n"
         << "  -c FILE                 Load hsbench sqlite corpus.\n"
         << "  -B N                    Run N normal hs_scan() baseline rounds (default 1).\n"
         << "  -R N                    Run N collection rounds with collector (default 3).\n"
         << "  -n N                    Run N measurement rounds after DB switch (default 5).\n"
         << "  -N                      Block mode marker, accepted for hsbench familiarity.\n"
         << "  -T CPU,CPU... or CPU-CPU\n"
         << "                           Run one worker per CPU and bind affinity.\n"
         << "  -r                      Print collection/feedback summaries.\n"
         << "  -f N                    Print top N report and feedback fragments.\n"
         << "  -d                      Print compile feedback diagnostics.\n"
         << "  -v                      Verbose feedback view (-r -d -f 10 if -f omitted).\n"
         << "  -o FILE                 Dump report fragments as CSV.\n"
         << "  -O FILE                 Dump feedback fragments as CSV.\n\n"
         << "Optimized throughput is measured only after the feedback-compiled DB is active.\n";

    if (error) {
        cerr << "Error: " << error << "\n";
    }
}

void usage(const string &error) {
    usage(error.c_str());
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


bool processArgs(int argc, char **argv, Options *opts) {
    static const struct option longopts[] = {
        {"help", no_argument, nullptr, 'h'},
        {nullptr, 0, nullptr, 0}
    };

    opterr = 0;
    int optionIndex = 0;
    for (;;) {
        int c = getopt_long(argc, argv, ":B:c:de:f:G:hNn:o:O:rR:T:vV",
                            longopts,
                            &optionIndex);
        if (c < 0) {
            break;
        }

        unsigned value = 0;
        switch (c) {
        case 'h':
            usage(nullptr);
            std::exit(0);
        case 'G':
            opts->greyOverrides.assign(optarg);
            if (hs_set_grey_overrides(optarg) != HS_SUCCESS) {
                usage("Invalid grey overrides");
                return false;
            }
            break;
        case 'B':
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
        case 'd':
            opts->showDiagnostics = true;
            break;
        case 'e':
            if (!optarg || !*optarg) {
                usage("expression path must not be empty");
                return false;
            }
            opts->exprPath.assign(optarg);
            break;
        case 'f':
            if (!parsePositiveUnsigned(optarg, &value)) {
                usage("top fragment count must be a positive integer");
                return false;
            }
            opts->top = value;
            break;
        case 'n':
            if (!parsePositiveUnsigned(optarg, &value)) {
                usage("measure rounds must be a positive integer");
                return false;
            }
            opts->measureRounds = value;
            break;
        case 'N':
            break;
        case 'o':
            if (!optarg || !*optarg) {
                usage("report CSV path must not be empty");
                return false;
            }
            opts->reportCsvPath.assign(optarg);
            break;
        case 'O':
            if (!optarg || !*optarg) {
                usage("feedback CSV path must not be empty");
                return false;
            }
            opts->feedbackCsvPath.assign(optarg);
            break;
        case 'r':
            opts->showSummaries = true;
            break;
        case 'R':
            if (!parsePositiveUnsigned(optarg, &value)) {
                usage("collection rounds must be a positive integer");
                return false;
            }
            opts->collectRounds = value;
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
                    cpuError = "thread CPU list must be CPU,CPU... or CPU-CPU";
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
            usage("hspgo first version supports block mode only");
            return false;
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
    if (patterns->exprs.size() >
        std::numeric_limits<unsigned int>::max()) {
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

string signatureName(const Options &opts) {
    return opts.exprPath;
}

void printField(const char *label, const string &value) {
    cout << left << setw(28) << label << value << "\n";
}

void printField(const char *label, unsigned long long value) {
    printField(label, formatCount(value));
}

bool compileDatabase(const PatternSet &patterns,
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
            extPtrs.data(), count, HS_MODE_BLOCK, nullptr, ctx, &rawDb,
            &compileErr);
    } else {
        err = hs_compile_ext_multi(exprPtrs.data(), patterns.flags.data(),
                                   patterns.ids.data(), extPtrs.data(), count,
                                   HS_MODE_BLOCK, nullptr, &rawDb,
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

int onMatch(unsigned int, unsigned long long, unsigned long long,
            unsigned int, void *ctx) {
    if (ctx) {
        RunStats *stats = static_cast<RunStats *>(ctx);
        stats->matches++;
    }
    return 0;
}

bool scanBlocks(const hs_database_t *db, hs_scratch_t *scratch,
                const vector<DataBlock> &blocks, unsigned rounds,
                hs_fp_collector_t *collector, RunStats *stats,
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
            hs_error_t err = HS_SUCCESS;
            if (collector) {
                err = hs_scan_with_collector(db, data, len, 0, scratch, onMatch,
                                             stats, collector);
            } else {
                err = hs_scan(db, data, len, 0, scratch, onMatch, stats);
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

double falsePositiveRate(const hs_fp_fragment_info_t &fragment) {
    if (!fragment.trigger_count) {
        return 0.0;
    }
    return static_cast<double>(fragment.false_positive_count) /
           static_cast<double>(fragment.trigger_count);
}

bool loadReportFragments(const hs_fp_report_t *report,
                         vector<hs_fp_fragment_info_t> *fragments) {
    hs_fp_report_summary_t summary = {};
    hs_error_t err = hs_fp_report_get_summary(report, &summary);
    if (err != HS_SUCCESS) {
        cerr << "hs_fp_report_get_summary failed with error " << err << "\n";
        return false;
    }

    fragments->resize(summary.fragment_count);
    for (unsigned int i = 0; i < summary.fragment_count; i++) {
        err = hs_fp_report_get_fragment(report, i, &(*fragments)[i]);
        if (err != HS_SUCCESS) {
            cerr << "hs_fp_report_get_fragment failed with error " << err
                 << "\n";
            return false;
        }
    }
    return true;
}

bool loadFeedbackFragments(const hs_fp_feedback_t *feedback,
                           vector<hs_fp_fragment_info_t> *fragments) {
    hs_fp_feedback_summary_t summary = {};
    hs_error_t err = hs_fp_feedback_get_summary(feedback, &summary);
    if (err != HS_SUCCESS) {
        cerr << "hs_fp_feedback_get_summary failed with error " << err << "\n";
        return false;
    }

    fragments->resize(summary.bad_fragment_count);
    for (unsigned int i = 0; i < summary.bad_fragment_count; i++) {
        err = hs_fp_feedback_get_fragment(feedback, i, &(*fragments)[i]);
        if (err != HS_SUCCESS) {
            cerr << "hs_fp_feedback_get_fragment failed with error " << err
                 << "\n";
            return false;
        }
    }
    return true;
}

void sortFragments(vector<hs_fp_fragment_info_t> *fragments) {
    std::sort(fragments->begin(), fragments->end(),
              [](const hs_fp_fragment_info_t &a,
                 const hs_fp_fragment_info_t &b) {
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
    cout << left << setw(4) << "#"
         << setw(20) << "key"
         << setw(10) << "table"
         << setw(10) << "engine"
         << right << setw(12) << "trigger"
         << setw(12) << "true"
         << setw(12) << "false"
         << setw(9) << "fp_rate"
         << "  fragment\n";
}

string fragmentKeyString(const hs_fp_fragment_info_t &fragment) {
    std::ostringstream key;
    key << "0x" << hex << setw(16) << setfill('0') << fragment.key << dec
        << setfill(' ');
    return key.str();
}

void printFragmentRow(size_t index, const hs_fp_fragment_info_t &fragment) {
    cout << left << setw(4) << index
         << setw(20) << fragmentKeyString(fragment)
         << setw(10) << tableName(fragment.table)
         << setw(10) << engineName(fragment.engine)
         << right << setw(12) << fragment.trigger_count
         << setw(12) << fragment.true_trigger_count
         << setw(12) << fragment.false_positive_count
         << setw(8) << std::fixed << std::setprecision(2)
         << (falsePositiveRate(fragment) * 100.0) << "%"
         << "  \"" << escapedBytes(fragment.bytes, fragment.length) << "\"\n";
    cout.unsetf(std::ios::floatfield);
}

void printTopReportFragments(const hs_fp_report_t *report, unsigned top) {
    vector<hs_fp_fragment_info_t> fragments;
    if (!loadReportFragments(report, &fragments)) {
        return;
    }
    sortFragments(&fragments);

    cout << "\nTop report fragments:\n";
    if (fragments.empty()) {
        cout << "  (none)\n";
        return;
    }

    printFragmentHeader();
    const size_t limit = std::min<size_t>(top, fragments.size());
    for (size_t i = 0; i < limit; i++) {
        printFragmentRow(i + 1, fragments[i]);
    }
}

void printFeedbackFragments(const hs_fp_feedback_t *feedback, unsigned top) {
    vector<hs_fp_fragment_info_t> fragments;
    if (!loadFeedbackFragments(feedback, &fragments)) {
        return;
    }
    sortFragments(&fragments);

    cout << "\nBad fragments selected for feedback:\n";
    if (fragments.empty()) {
        cout << "  (none)\n";
        return;
    }

    printFragmentHeader();
    const size_t limit = std::min<size_t>(top, fragments.size());
    for (size_t i = 0; i < limit; i++) {
        printFragmentRow(i + 1, fragments[i]);
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
                      vector<hs_fp_fragment_info_t> fragments) {
    std::ofstream out(path.c_str(), std::ios::binary);
    if (!out.good()) {
        cerr << "Unable to open output file: " << path << "\n";
        return false;
    }

    sortFragments(&fragments);
    out << "key,table,engine,trigger_count,true_trigger_count,"
           "false_positive_count,fp_rate,fragment\n";
    for (const auto &fragment : fragments) {
        const string bytes = escapedBytes(fragment.bytes, fragment.length);
        out << fragmentKeyString(fragment) << ','
            << tableName(fragment.table) << ','
            << engineName(fragment.engine) << ','
            << fragment.trigger_count << ','
            << fragment.true_trigger_count << ','
            << fragment.false_positive_count << ','
            << formatFixed(falsePositiveRate(fragment), 6) << ','
            << csvEscape(bytes) << "\n";
    }

    if (!out.good()) {
        cerr << "Unable to write output file: " << path << "\n";
        return false;
    }
    return true;
}

bool dumpReportCsv(const hs_fp_report_t *report, const string &path) {
    if (path.empty()) {
        return true;
    }
    vector<hs_fp_fragment_info_t> fragments;
    if (!loadReportFragments(report, &fragments)) {
        return false;
    }
    return writeFragmentCsv(path, move(fragments));
}

bool dumpFeedbackCsv(const hs_fp_feedback_t *feedback, const string &path) {
    if (path.empty()) {
        return true;
    }
    vector<hs_fp_fragment_info_t> fragments;
    if (!loadFeedbackFragments(feedback, &fragments)) {
        return false;
    }
    return writeFragmentCsv(path, move(fragments));
}

void printReportSummary(const hs_fp_report_t *report) {
    hs_fp_report_summary_t summary = {};
    hs_error_t err = hs_fp_report_get_summary(report, &summary);
    if (err != HS_SUCCESS) {
        cerr << "hs_fp_report_get_summary failed with error " << err << "\n";
        return;
    }

    cout << "\nCollection report:\n";
    printField("Fragments:", static_cast<unsigned long long>(
                                  summary.fragment_count));
    printField("Scan calls:", summary.scan_calls);
    printField("Scan bytes:", summary.scan_bytes);
    printField("Triggers:", summary.trigger_count);
    printField("True triggers:", summary.true_trigger_count);
    printField("Final reports:", summary.final_report_count);
    printField("False positives:", summary.false_positive_count);
    printField("Unknown reports:", summary.unknown_report_count);
    printField("Dropped:", summary.dropped_trigger_count);
}

void printFeedbackSummary(const hs_fp_feedback_t *feedback) {
    hs_fp_feedback_summary_t summary = {};
    hs_error_t err = hs_fp_feedback_get_summary(feedback, &summary);
    if (err != HS_SUCCESS) {
        cerr << "hs_fp_feedback_get_summary failed with error " << err << "\n";
        return;
    }

    cout << "\nFeedback summary:\n";
    printField("Bad fragments:",
               static_cast<unsigned long long>(summary.bad_fragment_count));
    printField("Source scan calls:", summary.scan_calls);
    printField("Source scan bytes:", summary.scan_bytes);
    printField("Source false positives:",
               summary.total_false_positive_count);
}

bool buildReportAndFeedback(const hs_fp_collector_t *collector,
                            ReportPtr *report, FeedbackPtr *feedback) {
    hs_fp_report_t *rawReport = nullptr;
    hs_error_t err = hs_fp_collector_report(collector, &rawReport);
    if (err != HS_SUCCESS) {
        cerr << "hs_fp_collector_report failed with error " << err << "\n";
        return false;
    }
    report->reset(rawReport);

    hs_fp_feedback_t *rawFeedback = nullptr;
    err = hs_fp_feedback_build(report->get(), &rawFeedback);
    if (err != HS_SUCCESS) {
        cerr << "hs_fp_feedback_build failed with error " << err << "\n";
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
    cout << "\nCompile feedback diagnostics:\n";
    printField("Observe checked:",
               hs_compile_context_observe_checked_count(ctx));
    printField("Observe hit:", hs_compile_context_observe_hit_count(ctx));
    printField("Block checked:", hs_compile_context_block_checked_count(ctx));
    printField("Blocked:", hs_compile_context_blocked_count(ctx));
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
    printField("Hyperscan info:", stats.info);
    printField("Expression count:",
               static_cast<unsigned long long>(stats.expressionCount));
    printField("Bytecode size:",
               formatCount(static_cast<unsigned long long>(stats.bytecodeSize)) +
                   " bytes");
    printField("Database CRC:", formatHex32(stats.crc32));
    printField("Scratch size:",
               formatCount(static_cast<unsigned long long>(stats.scratchSize)) +
                   " bytes");
    printField("Compile time:", formatFixed(stats.compileSeconds, 3) +
                                    " seconds");
    printField("Peak heap usage:",
               formatCount(static_cast<unsigned long long>(stats.peakHeap)) +
                   " bytes");
}

void printHspgoConfig(const Options &opts) {
    cout << "\nHSPGO feedback configuration:\n";
    string workers = formatCount(opts.threadCount);
    if (!opts.threadSpec.empty()) {
        workers += " (-T " + opts.threadSpec + ")";
    }
    printField("Threads:", workers);
    printField("Baseline rounds:",
               static_cast<unsigned long long>(opts.baselineRounds));
    printField("Collection rounds:",
               static_cast<unsigned long long>(opts.collectRounds));
    printField("Measurement rounds:",
               static_cast<unsigned long long>(opts.measureRounds));
    if (!opts.greyOverrides.empty()) {
        printField("Grey overrides:", opts.greyOverrides);
    }
}

void printScanSummary(const char *title, const vector<DataBlock> &blocks,
                      const ParallelRunResult &result, unsigned rounds,
                      unsigned threads) {
    const unsigned long long bytesPerRun = corpusBytes(blocks);
    const unsigned long long blockCount =
        static_cast<unsigned long long>(blocks.size());
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
    printField("Time spent scanning:", formatFixed(result.seconds, 3) +
                                           " seconds");
    printField("Corpus size:",
               formatCount(bytesPerRun) + " bytes (" +
                   formatCount(blockCount) + " blocks)");
    printField("Matches per iteration:",
               formatFixed(matchesPerIteration, 0) + " (" +
                   formatFixed(matchRate, 3) + " matches/kilobyte)");
    printField("Overall block rate:",
               formatFixedWithCommas(blockRate, 2) + " blocks/sec");
    printField("Mean throughput (overall):",
               formatFixedWithCommas(throughputMbit(result.stats,
                                                    result.seconds),
                                     2) + " Mbit/sec");
    printField("Max throughput (per core):",
               formatFixedWithCommas(throughputMbit(workerBytes,
                                                    result.fastestWorkerSeconds),
                                     2) + " Mbit/sec");
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

    vector<DataBlock> blocks;
    if (!loadCorpus(opts, &blocks)) {
        return 1;
    }

    DatabasePtr baselineDb;
    const auto baselineCompileStart = std::chrono::steady_clock::now();
    if (!compileDatabase(patterns, nullptr, &baselineDb)) {
        return 1;
    }
    const auto baselineCompileEnd = std::chrono::steady_clock::now();
    vector<ScratchPtr> baselineScratches;
    if (!allocScratches(baselineDb.get(), opts.threadCount,
                        &baselineScratches)) {
        return 1;
    }

    DatabaseStats baselineDbStats;
    if (!queryDatabaseStats(baselineDb.get(), baselineScratches[0].get(),
                            signatureName(opts), patterns.exprs.size(),
                            secondsSince(baselineCompileStart,
                                         baselineCompileEnd),
                            &baselineDbStats)) {
        return 1;
    }

    cout << "hspgo block feedback demo\n";
    printDatabaseStats("Baseline database", baselineDbStats);
    printHspgoConfig(opts);

    if (opts.baselineRounds) {
        ParallelRunResult baselineResult;
        if (!runParallelScan(baselineDb.get(), baselineScratches, blocks,
                             opts.baselineRounds, nullptr,
                             opts.cpuList.empty() ? nullptr : &opts.cpuList,
                             &baselineResult)) {
            return 1;
        }
        printScanSummary("Baseline scan", blocks, baselineResult,
                         opts.baselineRounds, opts.threadCount);
    }

    vector<CollectorPtr> workerCollectors;
    if (!createCollectors(baselineDb.get(), opts.threadCount,
                          &workerCollectors)) {
        return 1;
    }

    ParallelRunResult collectResult;
    if (!runParallelScan(baselineDb.get(), baselineScratches, blocks,
                         opts.collectRounds, &workerCollectors,
                         opts.cpuList.empty() ? nullptr : &opts.cpuList,
                         &collectResult)) {
        return 1;
    }
    printScanSummary("Collection scan", blocks, collectResult,
                     opts.collectRounds, opts.threadCount);

    CollectorPtr collector;
    if (!mergeCollectors(baselineDb.get(), workerCollectors, &collector)) {
        return 1;
    }

    ReportPtr report;
    FeedbackPtr feedback;
    if (!buildReportAndFeedback(collector.get(), &report, &feedback)) {
        return 1;
    }

    if (opts.showSummaries) {
        printReportSummary(report.get());
        printFeedbackSummary(feedback.get());
    }
    if (opts.top) {
        printTopReportFragments(report.get(), opts.top);
        printFeedbackFragments(feedback.get(), opts.top);
    }

    if (!dumpReportCsv(report.get(), opts.reportCsvPath) ||
        !dumpFeedbackCsv(feedback.get(), opts.feedbackCsvPath)) {
        return 1;
    }

    CompileContextPtr compileCtx;
    if (!createCompileContext(feedback.get(), &compileCtx)) {
        return 1;
    }

    DatabasePtr optimizedDb;
    const auto optimizedCompileStart = std::chrono::steady_clock::now();
    if (!compileDatabase(patterns, compileCtx.get(), &optimizedDb)) {
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
    if (!queryDatabaseStats(optimizedDb.get(), optimizedScratches[0].get(),
                            signatureName(opts), patterns.exprs.size(),
                            secondsSince(optimizedCompileStart,
                                         optimizedCompileEnd),
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
                         &measureResult)) {
        return 1;
    }
    printScanSummary("Optimized measurement", blocks, measureResult,
                     opts.measureRounds, opts.threadCount);

    return 0;
}
