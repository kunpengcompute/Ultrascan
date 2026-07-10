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

#include "config.h"

#include "ExpressionParser.h"
#include "data_corpus.h"
#include "expressions.h"
#include "hs.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <climits>
#include <clocale>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <limits>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include <getopt.h>

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

enum {
    OPT_COLLECT_ROUNDS = 256,
    OPT_MEASURE_ROUNDS,
    OPT_TEXT,
    OPT_TOP,
    OPT_DUMP_REPORT,
    OPT_DUMP_FEEDBACK,
    OPT_THREADS
};

struct Options {
    string exprPath;
    string pattern;
    string corpusPath;
    string text;
    string dumpReportPath;
    string dumpFeedbackPath;
    string threadSpec;
    unsigned collectRounds = 3;
    unsigned measureRounds = 5;
    unsigned top = 10;
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
         << "  -e PATH                 Load hsbench expression file/directory.\n"
         << "  -s FILE                 Load one hsbench signature file.\n"
         << "  -p PATTERN              Add one quick pattern. Plain text and /re/flags are accepted.\n"
         << "  -c FILE                 Load hsbench sqlite corpus; fallback to one raw file block.\n"
         << "      --text TEXT         Add one inline corpus block.\n"
         << "      --collect-rounds N  Scan N rounds with hs_scan_with_collector() (default 3).\n"
         << "  -n N, --measure-rounds N\n"
         << "                           Scan N rounds after feedback recompile and DB switch (default 5).\n"
         << "  -N                      Block mode marker, accepted for hsbench familiarity.\n"
         << "  -T LIST, --threads LIST Accepted for hsbench familiarity; first version runs one thread.\n"
         << "      --top N             Print top N report/feedback fragments (default 10).\n"
         << "      --dump-report FILE  Serialize collected report to FILE.\n"
         << "      --dump-feedback FILE\n"
         << "                           Serialize generated feedback to FILE.\n\n"
         << "Throughput is measured only after the feedback-optimized DB is active.\n";

    if (error) {
        cerr << "Error: " << error << "\n";
    }
}

bool parsePositiveUnsigned(const char *text, unsigned *out) {
    if (!text || !*text || !out) {
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
        {"text", required_argument, nullptr, OPT_TEXT},
        {"collect-rounds", required_argument, nullptr, OPT_COLLECT_ROUNDS},
        {"measure-rounds", required_argument, nullptr, OPT_MEASURE_ROUNDS},
        {"top", required_argument, nullptr, OPT_TOP},
        {"dump-report", required_argument, nullptr, OPT_DUMP_REPORT},
        {"dump-feedback", required_argument, nullptr, OPT_DUMP_FEEDBACK},
        {"threads", required_argument, nullptr, OPT_THREADS},
        {nullptr, 0, nullptr, 0}
    };

    int optionIndex = 0;
    for (;;) {
        int c = getopt_long(argc, argv, "hc:e:s:p:n:NT:V", longopts,
                            &optionIndex);
        if (c < 0) {
            break;
        }

        unsigned value = 0;
        switch (c) {
        case 'h':
            usage(nullptr);
            std::exit(0);
        case 'c':
            opts->corpusPath.assign(optarg);
            break;
        case 'e':
        case 's':
            opts->exprPath.assign(optarg);
            break;
        case 'p':
            opts->pattern.assign(optarg);
            break;
        case 'n':
        case OPT_MEASURE_ROUNDS:
            if (!parsePositiveUnsigned(optarg, &value)) {
                usage("measure rounds must be a positive integer");
                return false;
            }
            opts->measureRounds = value;
            break;
        case 'N':
            break;
        case 'T':
        case OPT_THREADS:
            if (!optarg || !*optarg) {
                usage("thread list must not be empty");
                return false;
            }
            opts->threadSpec.assign(optarg);
            break;
        case 'V':
            usage("hspgo first version supports block mode only");
            return false;
        case OPT_TEXT:
            opts->text.assign(optarg);
            break;
        case OPT_COLLECT_ROUNDS:
            if (!parsePositiveUnsigned(optarg, &value)) {
                usage("collect rounds must be a positive integer");
                return false;
            }
            opts->collectRounds = value;
            break;
        case OPT_TOP:
            if (!parsePositiveUnsigned(optarg, &value)) {
                usage("top must be a positive integer");
                return false;
            }
            opts->top = value;
            break;
        case OPT_DUMP_REPORT:
            opts->dumpReportPath.assign(optarg);
            break;
        case OPT_DUMP_FEEDBACK:
            opts->dumpFeedbackPath.assign(optarg);
            break;
        default:
            usage("unknown argument");
            return false;
        }
    }

    if (optind < argc) {
        usage("unexpected positional argument");
        return false;
    }
    if (opts->exprPath.empty() && opts->pattern.empty()) {
        usage("provide -e/-s or -p");
        return false;
    }
    if (opts->corpusPath.empty() && opts->text.empty()) {
        usage("provide -c or --text");
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
    if (!opts.exprPath.empty()) {
        loadExpressions(opts.exprPath, exprMap);
    }

    for (const auto &m : exprMap) {
        if (!addExpression(m.first, m.second, false, patterns)) {
            return false;
        }
    }

    if (!opts.pattern.empty()) {
        unsigned int id = 1;
        if (!patterns->ids.empty()) {
            id = *std::max_element(patterns->ids.begin(), patterns->ids.end());
            if (id == std::numeric_limits<unsigned int>::max()) {
                cerr << "Cannot allocate id for -p pattern\n";
                return false;
            }
            id++;
        }
        if (!addExpression(id, opts.pattern, true, patterns)) {
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

bool readPlainFileBlock(const string &path, vector<DataBlock> *blocks) {
    std::ifstream in(path.c_str(), std::ios::binary);
    if (!in.good()) {
        cerr << "Unable to open corpus file: " << path << "\n";
        return false;
    }

    string payload((std::istreambuf_iterator<char>(in)),
                   std::istreambuf_iterator<char>());
    if (payload.empty()) {
        cerr << "Corpus file is empty: " << path << "\n";
        return false;
    }

    blocks->emplace_back(0, 0, 0, move(payload));
    return true;
}

bool loadCorpus(const Options &opts, vector<DataBlock> *blocks) {
    if (!opts.corpusPath.empty()) {
        try {
            vector<DataBlock> dbBlocks = readCorpus(opts.corpusPath);
            blocks->insert(blocks->end(), dbBlocks.begin(), dbBlocks.end());
        } catch (const DataCorpusError &e) {
            cerr << "Corpus sqlite read failed: " << e.msg
                 << "; treating file as one raw block.\n";
            if (!readPlainFileBlock(opts.corpusPath, blocks)) {
                return false;
            }
        }
    }

    if (!opts.text.empty()) {
        blocks->emplace_back(static_cast<unsigned int>(blocks->size()), 0, 0,
                             opts.text);
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
                hs_fp_collector_t *collector, RunStats *stats) {
    for (unsigned round = 0; round < rounds; round++) {
        for (const auto &block : blocks) {
            if (block.payload.size() >
                std::numeric_limits<unsigned int>::max()) {
                cerr << "Corpus block " << block.id
                     << " is too large for block scan\n";
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
                cerr << "scan failed with error " << err << "\n";
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
         << setw(18) << "key"
         << setw(9) << "table"
         << setw(9) << "engine"
         << right << setw(10) << "trigger"
         << setw(10) << "true"
         << setw(10) << "false"
         << setw(9) << "fp_rate"
         << "  fragment\n";
}

void printFragmentRow(size_t index, const hs_fp_fragment_info_t &fragment) {
    std::ostringstream key;
    key << "0x" << hex << setw(16) << setfill('0') << fragment.key << dec
        << setfill(' ');

    cout << left << setw(4) << index
         << setw(18) << key.str()
         << setw(9) << tableName(fragment.table)
         << setw(9) << engineName(fragment.engine)
         << right << setw(10) << fragment.trigger_count
         << setw(10) << fragment.true_trigger_count
         << setw(10) << fragment.false_positive_count
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

bool writeBlob(const string &path, const char *bytes, size_t length) {
    std::ofstream out(path.c_str(), std::ios::binary);
    if (!out.good()) {
        cerr << "Unable to open output file: " << path << "\n";
        return false;
    }
    out.write(bytes, static_cast<std::streamsize>(length));
    if (!out.good()) {
        cerr << "Unable to write output file: " << path << "\n";
        return false;
    }
    return true;
}

bool dumpReport(const hs_fp_report_t *report, const string &path) {
    if (path.empty()) {
        return true;
    }
    char *bytes = nullptr;
    size_t length = 0;
    hs_error_t err = hs_fp_report_serialize(report, &bytes, &length);
    if (err != HS_SUCCESS) {
        cerr << "hs_fp_report_serialize failed with error " << err << "\n";
        return false;
    }

    bool ok = writeBlob(path, bytes, length);
    std::free(bytes);
    return ok;
}

bool dumpFeedback(const hs_fp_feedback_t *feedback, const string &path) {
    if (path.empty()) {
        return true;
    }
    char *bytes = nullptr;
    size_t length = 0;
    hs_error_t err = hs_fp_feedback_serialize(feedback, &bytes, &length);
    if (err != HS_SUCCESS) {
        cerr << "hs_fp_feedback_serialize failed with error " << err << "\n";
        return false;
    }

    bool ok = writeBlob(path, bytes, length);
    std::free(bytes);
    return ok;
}

void printReportSummary(const hs_fp_report_t *report) {
    hs_fp_report_summary_t summary = {};
    hs_error_t err = hs_fp_report_get_summary(report, &summary);
    if (err != HS_SUCCESS) {
        cerr << "hs_fp_report_get_summary failed with error " << err << "\n";
        return;
    }

    cout << "\nCollection report:\n"
         << "  fragments:       " << summary.fragment_count << "\n"
         << "  scan calls:      " << summary.scan_calls << "\n"
         << "  scan bytes:      " << summary.scan_bytes << "\n"
         << "  triggers:        " << summary.trigger_count << "\n"
         << "  true triggers:   " << summary.true_trigger_count << "\n"
         << "  final reports:   " << summary.final_report_count << "\n"
         << "  false positives: " << summary.false_positive_count << "\n"
         << "  unknown reports: " << summary.unknown_report_count << "\n"
         << "  dropped:         " << summary.dropped_trigger_count << "\n";
}

void printFeedbackSummary(const hs_fp_feedback_t *feedback) {
    hs_fp_feedback_summary_t summary = {};
    hs_error_t err = hs_fp_feedback_get_summary(feedback, &summary);
    if (err != HS_SUCCESS) {
        cerr << "hs_fp_feedback_get_summary failed with error " << err << "\n";
        return;
    }

    cout << "\nFeedback summary:\n"
         << "  bad fragments:          " << summary.bad_fragment_count << "\n"
         << "  source scan calls:      " << summary.scan_calls << "\n"
         << "  source scan bytes:      " << summary.scan_bytes << "\n"
         << "  source false positives: "
         << summary.total_false_positive_count << "\n";
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
    cout << "\nCompile feedback diagnostics:\n"
         << "  observe checked: " << hs_compile_context_observe_checked_count(ctx)
         << "\n"
         << "  observe hit:     " << hs_compile_context_observe_hit_count(ctx)
         << "\n"
         << "  block checked:   " << hs_compile_context_block_checked_count(ctx)
         << "\n"
         << "  blocked:         " << hs_compile_context_blocked_count(ctx)
         << "\n";
}

double secondsSince(const std::chrono::steady_clock::time_point &start,
                    const std::chrono::steady_clock::time_point &end) {
    std::chrono::duration<double> elapsed = end - start;
    return elapsed.count();
}

} // namespace

int HS_CDECL main(int argc, char **argv) {
    std::setlocale(LC_ALL, "");

    Options opts;
    if (!processArgs(argc, argv, &opts)) {
        return 1;
    }

    if (!opts.threadSpec.empty()) {
        cout << "hspgo first version runs one scanning thread; requested -T "
             << opts.threadSpec << " is accepted but not used yet.\n";
    }

    PatternSet patterns;
    if (!loadPatternSet(opts, &patterns)) {
        return 1;
    }

    vector<DataBlock> blocks;
    if (!loadCorpus(opts, &blocks)) {
        return 1;
    }

    cout << "hspgo block feedback demo\n"
         << "  expressions:     " << patterns.exprs.size() << "\n"
         << "  corpus blocks:   " << blocks.size() << "\n"
         << "  corpus bytes:    " << corpusBytes(blocks) << "\n"
         << "  collect rounds:  " << opts.collectRounds << "\n"
         << "  measure rounds:  " << opts.measureRounds << "\n";

    DatabasePtr baselineDb;
    if (!compileDatabase(patterns, nullptr, &baselineDb)) {
        return 1;
    }
    ScratchPtr baselineScratch;
    if (!allocScratch(baselineDb.get(), &baselineScratch)) {
        return 1;
    }

    hs_fp_collector_t *rawCollector = nullptr;
    hs_error_t err = hs_fp_collector_create(baselineDb.get(), &rawCollector);
    if (err != HS_SUCCESS) {
        cerr << "hs_fp_collector_create failed with error " << err << "\n";
        return 1;
    }
    CollectorPtr collector(rawCollector);

    RunStats collectStats;
    auto collectStart = std::chrono::steady_clock::now();
    if (!scanBlocks(baselineDb.get(), baselineScratch.get(), blocks,
                    opts.collectRounds, collector.get(), &collectStats)) {
        return 1;
    }
    auto collectEnd = std::chrono::steady_clock::now();
    const double collectSeconds = secondsSince(collectStart, collectEnd);

    cout << "\nCollection scan:\n"
         << "  scan calls:      " << collectStats.scanCalls << "\n"
         << "  bytes:           " << collectStats.bytes << "\n"
         << "  matches:         " << collectStats.matches << "\n"
         << "  seconds:         " << std::fixed << std::setprecision(6)
         << collectSeconds << "\n";
    cout.unsetf(std::ios::floatfield);

    ReportPtr report;
    FeedbackPtr feedback;
    if (!buildReportAndFeedback(collector.get(), &report, &feedback)) {
        return 1;
    }

    printReportSummary(report.get());
    printTopReportFragments(report.get(), opts.top);
    printFeedbackSummary(feedback.get());
    printFeedbackFragments(feedback.get(), opts.top);

    if (!dumpReport(report.get(), opts.dumpReportPath) ||
        !dumpFeedback(feedback.get(), opts.dumpFeedbackPath)) {
        return 1;
    }

    CompileContextPtr compileCtx;
    if (!createCompileContext(feedback.get(), &compileCtx)) {
        return 1;
    }

    DatabasePtr optimizedDb;
    if (!compileDatabase(patterns, compileCtx.get(), &optimizedDb)) {
        return 1;
    }
    printCompileContextDiagnostics(compileCtx.get());

    ScratchPtr optimizedScratch;
    if (!allocScratch(optimizedDb.get(), &optimizedScratch)) {
        return 1;
    }

    baselineScratch.reset();
    baselineDb.reset();
    cout << "\nHot switch: active database replaced by feedback-compiled DB.\n";

    RunStats measureStats;
    auto measureStart = std::chrono::steady_clock::now();
    if (!scanBlocks(optimizedDb.get(), optimizedScratch.get(), blocks,
                    opts.measureRounds, nullptr, &measureStats)) {
        return 1;
    }
    auto measureEnd = std::chrono::steady_clock::now();
    const double measureSeconds = secondsSince(measureStart, measureEnd);
    const double mbps = measureSeconds > 0.0
                            ? (static_cast<double>(measureStats.bytes) * 8.0 /
                               measureSeconds / 1000000.0)
                            : 0.0;

    cout << "\nOptimized measurement:\n"
         << "  scan calls:      " << measureStats.scanCalls << "\n"
         << "  bytes:           " << measureStats.bytes << "\n"
         << "  matches:         " << measureStats.matches << "\n"
         << "  seconds:         " << std::fixed << std::setprecision(6)
         << measureSeconds << "\n"
         << "  throughput:      " << std::fixed << std::setprecision(2)
         << mbps << " Mbit/sec\n";

    return 0;
}
