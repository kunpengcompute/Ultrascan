#include "config.h"

#include "gtest/gtest.h"
#include "nfa/vermicelli.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <random>
#include <vector>

namespace {

class AlignedInput {
public:
    explicit AlignedInput(size_t len)
        : storage(len + VERM_BOUNDARY + 1), len(len) {
        uintptr_t raw = reinterpret_cast<uintptr_t>(storage.data());
        uintptr_t aligned = (raw + VERM_BOUNDARY - 1) & ~(uintptr_t)(VERM_BOUNDARY - 1);
        ptr = reinterpret_cast<u8 *>(aligned);
    }

    u8 *data() {
        return ptr;
    }

    const u8 *data() const {
        return ptr;
    }

    const u8 *end() const {
        return ptr + len;
    }

    size_t size() const {
        return len;
    }

    void fill(u8 value) {
        std::fill(ptr, ptr + len, value);
    }

private:
    std::vector<u8> storage;
    u8 *ptr = nullptr;
    size_t len = 0;
};

const u8 *findInWindow(const u8 *buf, size_t width, u8 c1, u8 c2) {
    for (size_t i = 0; i < width; i++) {
        if (buf[i] == c1 && buf[i + 1] == c2) {
            return buf + i;
        }
    }
    return nullptr;
}

const u8 *expectedDvermSearchAligned(const u8 *buf, const u8 *buf_end,
                                     u8 c1, u8 c2) {
    const u8 *cur = buf;

#if defined(HAVE_AVX512)
    for (; cur + 64 < buf_end; cur += 64) {
        const u8 *rv = findInWindow(cur, 64, c1, c2);
        if (rv) {
            return rv;
        }
    }
#else
#if ((defined __ARM_NEON) || (defined __ARM_NEON__))
    for (; cur + 64 < buf_end; cur += 64) {
        const u8 *rv = findInWindow(cur, 64, c1, c2);
        if (rv) {
            return rv;
        }
    }
#endif

    for (; cur + 16 < buf_end; cur += 16) {
        const u8 *rv = findInWindow(cur, 16, c1, c2);
        if (rv) {
            return rv;
        }
    }
#endif

    return nullptr;
}

const u8 *runDvermSearchAligned(const AlignedInput &input, u8 c1, u8 c2) {
    VERM_TYPE chars1 = VERM_SET_FN(c1);
    VERM_TYPE chars2 = VERM_SET_FN(c2);
    return dvermSearchAligned(chars1, chars2, c1, c2, input.data(),
                              input.end());
}

ptrdiff_t offsetFrom(const AlignedInput &input, const u8 *ptr) {
    return ptr ? ptr - input.data() : -1;
}

void checkCase(size_t len, u8 c1, u8 c2, std::initializer_list<size_t> hits) {
    ASSERT_GT(len, 0U);

    AlignedInput input(len);
    input.fill('x');

    for (size_t pos : hits) {
        ASSERT_LT(pos + 1, len);
        input.data()[pos] = c1;
        input.data()[pos + 1] = c2;
    }

    const u8 *expected = expectedDvermSearchAligned(input.data(), input.end(),
                                                    c1, c2);
    const u8 *actual = runDvermSearchAligned(input, c1, c2);
    EXPECT_EQ(offsetFrom(input, expected), offsetFrom(input, actual))
        << "len=" << len << " c1=" << static_cast<unsigned>(c1)
        << " c2=" << static_cast<unsigned>(c2);
}

size_t clampPosition(size_t pos, size_t len) {
    if (len < 2) {
        return 0;
    }
    return std::min(pos, len - 2);
}

} // namespace

TEST(DVermSearchAlignedFuzz, DirectedWindows) {
    const u8 c1 = 'a';
    const u8 c2 = 'b';

    checkCase(16, c1, c2, {});
    checkCase(17, c1, c2, {0});
    checkCase(17, c1, c2, {15});
    checkCase(33, c1, c2, {16});
    checkCase(65, c1, c2, {});
    checkCase(80, c1, c2, {});
    checkCase(81, c1, c2, {});
}

TEST(DVermSearchAlignedFuzz, DirectedArmWideLoop) {
    const u8 c1 = 'a';
    const u8 c2 = 'b';

    checkCase(129, c1, c2, {5});
    checkCase(129, c1, c2, {20});
    checkCase(129, c1, c2, {36});
    checkCase(129, c1, c2, {52});

    checkCase(129, c1, c2, {15});
    checkCase(129, c1, c2, {31});
    checkCase(129, c1, c2, {47});
    checkCase(129, c1, c2, {63});

    checkCase(97, c1, c2, {70});
    checkCase(97, c1, c2, {79});
}

TEST(DVermSearchAlignedFuzz, MultipleCandidatesReturnFirst) {
    const u8 c1 = 'q';
    const u8 c2 = 'r';

    checkCase(193, c1, c2, {140, 20, 63});
    checkCase(193, c1, c2, {127, 128, 129});
}

TEST(DVermSearchAlignedFuzz, RandomizedAgainstScalarOracle) {
    static const size_t interesting[] = {
        0,  1,  14, 15, 16, 30, 31, 32, 46, 47,
        48, 62, 63, 64, 79, 80, 95, 96, 127, 128,
    };

    std::mt19937 rng(0xd0e4a11U);
    std::uniform_int_distribution<int> byteDist(0, 255);
    std::uniform_int_distribution<size_t> lenDist(VERM_BOUNDARY + 1, 512);

    for (size_t i = 0; i < 512; i++) {
        const size_t len = lenDist(rng);
        AlignedInput input(len);
        for (size_t j = 0; j < input.size(); j++) {
            input.data()[j] = static_cast<u8>(byteDist(rng));
        }

        const u8 c1 = static_cast<u8>(byteDist(rng));
        const u8 c2 = static_cast<u8>(byteDist(rng));

        if ((i % 4) != 0) {
            const size_t raw = interesting[i % (sizeof(interesting) / sizeof(interesting[0]))];
            const size_t pos = clampPosition(raw + (i / 64) * 64, len);
            input.data()[pos] = c1;
            input.data()[pos + 1] = c2;
        }

        const u8 *expected = expectedDvermSearchAligned(input.data(),
                                                        input.end(), c1, c2);
        const u8 *actual = runDvermSearchAligned(input, c1, c2);
        EXPECT_EQ(offsetFrom(input, expected), offsetFrom(input, actual))
            << "iteration=" << i << " len=" << len
            << " c1=" << static_cast<unsigned>(c1)
            << " c2=" << static_cast<unsigned>(c2);
    }
}
