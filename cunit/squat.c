#include "config.h"
#include "cunit/cunit.h"
#include "squat_internal.h"

static void test_coding_int32(void)
{
    SquatInt32 out;
    char *r;
    char buf[4];
#define TESTCASE(value) \
{ \
    SquatInt32 _v = (value); \
    memset(buf, 0xa5, sizeof(buf)); \
    r = squat_encode_32(buf, _v); \
    CU_ASSERT_PTR_EQUAL(r, buf+4); \
    out = squat_decode_32(buf); \
    CU_ASSERT_EQUAL(out, _v); \
}

    TESTCASE(0x0);
    TESTCASE(0x1);
    TESTCASE(0x100);
    TESTCASE(0x10000);
    TESTCASE(0x1000000);
    TESTCASE(0x80);
    TESTCASE(0x8000);
    TESTCASE(0x800000);
    TESTCASE(0x80000000);
    TESTCASE(0xff);
    TESTCASE(0xffff);
    TESTCASE(0xffffff);
    TESTCASE(0xffffffff);
    TESTCASE(0xcafebabe);
    TESTCASE(0xbdefaced);
#undef TESTCASE
}

static void test_coding_int64(void)
{
    SquatInt64 out;
    char *r;
    char buf[8];
#define TESTCASE(value) \
{ \
    SquatInt64 _v = (value); \
    memset(buf, 0xa5, sizeof(buf)); \
    r = squat_encode_64(buf, _v); \
    CU_ASSERT_PTR_EQUAL(r, buf+8); \
    out = squat_decode_64(buf); \
    CU_ASSERT_EQUAL(out, _v); \
}

    TESTCASE(0x0);
    TESTCASE(0x1);
    TESTCASE(0x100);
    TESTCASE(0x10000);
    TESTCASE(0x1000000);
    TESTCASE(0x100000000);
    TESTCASE(0x10000000000);
    TESTCASE(0x1000000000000);
    TESTCASE(0x100000000000000);
    TESTCASE(0x80);
    TESTCASE(0x8000);
    TESTCASE(0x800000);
    TESTCASE(0x80000000);
    TESTCASE(0x8000000000);
    TESTCASE(0x800000000000);
    TESTCASE(0x80000000000000);
    TESTCASE(0x8000000000000000);
    TESTCASE(0xff);
    TESTCASE(0xffff);
    TESTCASE(0xffffff);
    TESTCASE(0xffffffff);
    TESTCASE(0xffffffffff);
    TESTCASE(0xffffffffffff);
    TESTCASE(0xffffffffffffff);
    TESTCASE(0xffffffffffffffff);
    TESTCASE(0xcafebabebdefaced);
#undef TESTCASE
}

/* test the variable-length int encoding */
static void test_coding_I(void)
{
    SquatInt64 out;
    int count;
    const char *s;
    char *r;
    char buf[9];
#define TESTCASE(value) \
{ \
    SquatInt64 _v = (value); \
    memset(buf, 0xa5, sizeof(buf)); \
    count = squat_count_encode_I(_v); \
    CU_ASSERT(count >= 1); \
    CU_ASSERT(count <= 9); \
    r = squat_encode_I(buf, _v); \
    CU_ASSERT_PTR_EQUAL(r, buf+count); \
    s = buf; \
    out = squat_decode_I(&s); \
    CU_ASSERT_EQUAL(out, _v); \
    CU_ASSERT_PTR_EQUAL((char *)s, buf+count); \
}

    TESTCASE(0x0);
    TESTCASE(0x1);
    TESTCASE(0x100);
    TESTCASE(0x10000);
    TESTCASE(0x1000000);
    TESTCASE(0x100000000);
    TESTCASE(0x10000000000);
    TESTCASE(0x1000000000000);
    TESTCASE(0x100000000000000);
    TESTCASE(0x80);
    TESTCASE(0x8000);
    TESTCASE(0x800000);
    TESTCASE(0x80000000);
    TESTCASE(0x8000000000);
    TESTCASE(0x800000000000);
    TESTCASE(0x80000000000000);
    TESTCASE(0xff);
    TESTCASE(0xffff);
    TESTCASE(0xffffff);
    TESTCASE(0xffffffff);
    TESTCASE(0xffffffffff);
    TESTCASE(0xffffffffffff);
    TESTCASE(0xffffffffffffff);
    TESTCASE(0x4afebabebdefaced);
#undef TESTCASE
}
