#include <freeradius-devel/util/acutest.h>

#include "sbuff.c"

//#include <gperftools/profiler.h>

#define TEST_SBUFF_LEN(_sbuff, _num) \
do { \
	size_t _len; \
	_len = talloc_array_length((_sbuff)->buff); \
	TEST_CHECK(_len == (size_t)_num); \
	TEST_MSG("Expected length : %zu", (size_t)_num); \
	TEST_MSG("Got length      : %zu", _len); \
} while(0);

#define TEST_SBUFF_USED(_sbuff, _num) \
do { \
	size_t _len; \
	_len = fr_sbuff_used(_sbuff); \
	TEST_CHECK(_len == (size_t)_num); \
	TEST_MSG("Expected length : %zu", (size_t)_num); \
	TEST_MSG("Got length      : %zu", _len); \
} while(0);

static void test_parse_init(void)
{
	char const	*in = "i am a test string";
	fr_sbuff_t	sbuff;

	TEST_CASE("Parse init with size");
	fr_sbuff_init(&sbuff, in, strlen(in));

	TEST_CHECK(sbuff.start == in);
	TEST_CHECK(sbuff.p == in);
	TEST_CHECK(sbuff.end == in + strlen(in));

	TEST_CASE("Parse init with end");
	fr_sbuff_init(&sbuff, in, in + strlen(in));

	TEST_CHECK(sbuff.start == in);
	TEST_CHECK(sbuff.p == in);
	TEST_CHECK(sbuff.end == in + strlen(in));

	TEST_CASE("Parse init with const end");
	fr_sbuff_init(&sbuff, in, (char const *)(in + strlen(in)));

	TEST_CHECK(sbuff.start == in);
	TEST_CHECK(sbuff.p == in);
	TEST_CHECK(sbuff.end == in + strlen(in));
}

static void test_strncpy_exact(void)
{
	char const	*in = "i am a test string";
	char const	*in_long = "i am a longer test string";
	char		out[18 + 1];
	fr_sbuff_t	sbuff;
	ssize_t		slen;

	fr_sbuff_init(&sbuff, in, strlen(in));

	TEST_CASE("Copy 5 bytes to out");
	slen = fr_sbuff_out_bstrncpy_exact(out, sizeof(out), &sbuff, 5);
	TEST_CHECK(slen == 5);
	TEST_CHECK(strcmp(out, "i am ") == 0);
	TEST_CHECK(strcmp(sbuff.p, "a test string") == 0);

	TEST_CASE("Copy 13 bytes to out");
	slen = fr_sbuff_out_bstrncpy_exact(out, sizeof(out), &sbuff, 13);
	TEST_CHECK(slen == 13);
	TEST_CHECK(strcmp(out, "a test string") == 0);
	TEST_CHECK(strcmp(sbuff.p, "") == 0);
	TEST_CHECK(sbuff.p == sbuff.end);

	TEST_CASE("Copy would overrun input");
	slen = fr_sbuff_out_bstrncpy_exact(out, sizeof(out), &sbuff, 1);
	TEST_CHECK(slen == 0);
	TEST_CHECK(sbuff.p == sbuff.end);

	TEST_CASE("Copy would overrun output (and SIZE_MAX special value)");
	fr_sbuff_init(&sbuff, in_long, strlen(in_long));

	slen = fr_sbuff_out_bstrncpy_exact(out, sizeof(out), &sbuff, SIZE_MAX);
	TEST_CHECK(slen == -7);
	TEST_CHECK(sbuff.p == sbuff.start);

	TEST_CASE("Zero length output buffer");
	fr_sbuff_set_to_start(&sbuff);
	out[0] = 'a';
	slen = fr_sbuff_out_bstrncpy_exact(out, 0, &sbuff, SIZE_MAX);
	TEST_CHECK(slen == -26);
	TEST_CHECK(out[0] == 'a');	/* Must not write \0 */
	TEST_CHECK(sbuff.p == sbuff.start);
}

static void test_strncpy(void)
{
	char const	*in = "i am a test string";
	char const	*in_long = "i am a longer test string";
	char		out[18 + 1];
	fr_sbuff_t	sbuff;
	ssize_t		slen;

	fr_sbuff_init(&sbuff, in, strlen(in));

	TEST_CASE("Copy 5 bytes to out");
	slen = fr_sbuff_out_bstrncpy(out, sizeof(out), &sbuff, 5);
	TEST_CHECK(slen == 5);
	TEST_CHECK(strcmp(out, "i am ") == 0);
	TEST_CHECK(strcmp(sbuff.p, "a test string") == 0);

	TEST_CASE("Copy 13 bytes to out");
	slen = fr_sbuff_out_bstrncpy(out, sizeof(out), &sbuff, 13);
	TEST_CHECK(slen == 13);
	TEST_CHECK(strcmp(out, "a test string") == 0);
	TEST_CHECK(strcmp(sbuff.p, "") == 0);
	TEST_CHECK(sbuff.p == sbuff.end);

	TEST_CASE("Copy would overrun input");
	slen = fr_sbuff_out_bstrncpy(out, sizeof(out), &sbuff, 1);
	TEST_CHECK(slen == 0);
	TEST_CHECK(sbuff.p == sbuff.end);

	TEST_CASE("Copy would overrun output (and SIZE_MAX special value)");
	fr_sbuff_init(&sbuff, in_long, strlen(in_long));

	slen = fr_sbuff_out_bstrncpy(out, sizeof(out), &sbuff, SIZE_MAX);
	TEST_CHECK(slen == 18);
	TEST_CHECK(strcmp(out, "i am a longer test") == 0);

	TEST_CASE("Zero length output buffer");
	fr_sbuff_set_to_start(&sbuff);
	out[0] = 'a';
	slen = fr_sbuff_out_bstrncpy(out, 0, &sbuff, SIZE_MAX);
	TEST_CHECK(slen == 0);
	TEST_CHECK(out[0] == 'a');	/* Must not write \0 */
	TEST_CHECK(sbuff.p == sbuff.start);
}

static void test_no_advance(void)
{
	char const	*in = "i am a test string";
	char		out[18 + 1];
	fr_sbuff_t	sbuff;
	ssize_t		slen;

	fr_sbuff_init(&sbuff, in, strlen(in));

	TEST_CASE("Copy 5 bytes to out - no advance");
	TEST_CHECK(sbuff.p == sbuff.start);
	slen = fr_sbuff_out_bstrncpy_exact(out, sizeof(out), &FR_SBUFF_NO_ADVANCE(&sbuff), 5);
	TEST_CHECK(slen == 5);
	TEST_CHECK(strcmp(out, "i am ") == 0);
	TEST_CHECK(sbuff.p == sbuff.start);
}

static void test_talloc_extend(void)
{
	fr_sbuff_t		sbuff;
	fr_sbuff_uctx_talloc_t	tctx;

	TEST_CASE("Initial allocation");
	TEST_CHECK(fr_sbuff_init_talloc(NULL, &sbuff, &tctx, 32, 50) == 0);
	TEST_SBUFF_USED(&sbuff, 0);
	TEST_SBUFF_LEN(&sbuff, 33);

	TEST_CASE("Trim to zero");
	TEST_CHECK(fr_sbuff_trim_talloc(&sbuff) == 0);
	TEST_SBUFF_USED(&sbuff, 0);
	TEST_SBUFF_LEN(&sbuff, 1);

	TEST_CASE("Print string - Should realloc to init");
	TEST_CHECK(fr_sbuff_in_strcpy(&sbuff, "0123456789") == 10);
	TEST_CHECK(strcmp(fr_sbuff_start(&sbuff), "0123456789") == 0);
	TEST_SBUFF_USED(&sbuff, 10);
	TEST_SBUFF_LEN(&sbuff, 33);

	TEST_CASE("Trim to strlen");
	TEST_CHECK(fr_sbuff_trim_talloc(&sbuff) == 0);
	TEST_SBUFF_LEN(&sbuff, 11);

	TEST_CASE("Print string - Should realloc to init");
	TEST_CHECK(fr_sbuff_in_strcpy(&sbuff, "0123456789") == 10);
	TEST_CHECK(strcmp(fr_sbuff_start(&sbuff), "01234567890123456789") == 0);
	TEST_SBUFF_USED(&sbuff, 20);
	TEST_SBUFF_LEN(&sbuff, 33);

	TEST_CASE("Trim to strlen");
	TEST_CHECK(fr_sbuff_trim_talloc(&sbuff) == 0);
	TEST_SBUFF_LEN(&sbuff, 21);

	TEST_CASE("Print string - Should realloc to double buffer len");
	TEST_CHECK(fr_sbuff_in_strcpy(&sbuff, "012345678901234") == 15);
	TEST_CHECK(strcmp(fr_sbuff_start(&sbuff), "01234567890123456789012345678901234") == 0);
	TEST_SBUFF_USED(&sbuff, 35);
	TEST_SBUFF_LEN(&sbuff, 41);

	TEST_CASE("Print string - Should only add a single char, should not extend the buffer");
	TEST_CHECK(fr_sbuff_in_strcpy(&sbuff, "A") == 1);
	TEST_CHECK(strcmp(fr_sbuff_start(&sbuff), "01234567890123456789012345678901234A") == 0);
	TEST_SBUFF_USED(&sbuff, 36);
	TEST_SBUFF_LEN(&sbuff, 41);

	TEST_CASE("Print string - Use all available buffer data");
	TEST_CHECK(fr_sbuff_in_strcpy(&sbuff, "BCDE") == 4);
	TEST_CHECK(strcmp(fr_sbuff_start(&sbuff), "01234567890123456789012345678901234ABCDE") == 0);
	TEST_SBUFF_USED(&sbuff, 40);
	TEST_SBUFF_LEN(&sbuff, 41);

	TEST_CASE("Print string - Add single char, should trigger doubling constrained by max");
	TEST_CHECK(fr_sbuff_in_strcpy(&sbuff, "F") == 1);
	TEST_CHECK(strcmp(fr_sbuff_start(&sbuff), "01234567890123456789012345678901234ABCDEF") == 0);
	TEST_SBUFF_USED(&sbuff, 41);
	TEST_SBUFF_LEN(&sbuff, 51);

	TEST_CASE("Print string - Add data to take us up to max");
	TEST_CHECK(fr_sbuff_in_strcpy(&sbuff, "GHIJKLMNO") == 9);
	TEST_CHECK(strcmp(fr_sbuff_start(&sbuff), "01234567890123456789012345678901234ABCDEFGHIJKLMNO") == 0);
	TEST_SBUFF_USED(&sbuff, 50);
	TEST_SBUFF_LEN(&sbuff, 51);

	TEST_CASE("Print string - Add single char, should fail");
	TEST_CHECK(fr_sbuff_in_strcpy(&sbuff, "P") == -1);
	TEST_CHECK(strcmp(fr_sbuff_start(&sbuff), "01234567890123456789012345678901234ABCDEFGHIJKLMNO") == 0);
	TEST_SBUFF_USED(&sbuff, 50);
	TEST_SBUFF_LEN(&sbuff, 51);

	TEST_CASE("Trim to strlen (should be noop)");
	TEST_CHECK(fr_sbuff_trim_talloc(&sbuff) == 0);
	TEST_CHECK(strcmp(fr_sbuff_start(&sbuff), "01234567890123456789012345678901234ABCDEFGHIJKLMNO") == 0);
	TEST_SBUFF_USED(&sbuff, 50);
	TEST_SBUFF_LEN(&sbuff, 51);
}

static void test_talloc_extend_init_zero(void)
{
	fr_sbuff_t		sbuff;
	fr_sbuff_uctx_talloc_t	tctx;

	TEST_CASE("Initial allocation");
	TEST_CHECK(fr_sbuff_init_talloc(NULL, &sbuff, &tctx, 0, 50) == 0);
	TEST_SBUFF_USED(&sbuff, 0);
	TEST_SBUFF_LEN(&sbuff, 1);

	TEST_CASE("Print string - Should alloc one byte");
	TEST_CHECK(fr_sbuff_in_strcpy(&sbuff, "A") == 1);
	TEST_CHECK(strcmp(fr_sbuff_start(&sbuff), "A") == 0);
	TEST_SBUFF_USED(&sbuff, 1);
	TEST_SBUFF_LEN(&sbuff, 2);

	TEST_CASE("Print string - Should alloc two bytes");
	TEST_CHECK(fr_sbuff_in_strcpy(&sbuff, "BC") == 2);
	TEST_CHECK(strcmp(fr_sbuff_start(&sbuff), "ABC") == 0);
	TEST_SBUFF_USED(&sbuff, 3);
	TEST_SBUFF_LEN(&sbuff, 4);

	TEST_CASE("Print string - Should alloc three bytes");
	TEST_CHECK(fr_sbuff_in_strcpy(&sbuff, "D") == 1);
	TEST_CHECK(strcmp(fr_sbuff_start(&sbuff), "ABCD") == 0);
	TEST_SBUFF_USED(&sbuff, 4);
	TEST_SBUFF_LEN(&sbuff, 7);
}

static void test_talloc_extend_multi_level(void)
{
	fr_sbuff_t		sbuff_0, sbuff_1;
	fr_sbuff_uctx_talloc_t	tctx;

	TEST_CASE("Initial allocation");
	TEST_CHECK(fr_sbuff_init_talloc(NULL, &sbuff_0, &tctx, 0, 50) == 0);
	TEST_SBUFF_USED(&sbuff_0, 0);
	TEST_SBUFF_LEN(&sbuff_0, 1);

	sbuff_1 = FR_SBUFF_COPY(&sbuff_0);
	TEST_CASE("Check sbuff_1 has extend fields set");
	TEST_CHECK(sbuff_0.extend == sbuff_1.extend);
	TEST_CHECK(sbuff_0.uctx == sbuff_1.uctx);
	TEST_CHECK(sbuff_1.parent == &sbuff_0);
	TEST_SBUFF_USED(&sbuff_1, 0);
	TEST_SBUFF_LEN(&sbuff_1, 1);

	TEST_CASE("Print string - Should alloc one byte");
	TEST_CHECK(fr_sbuff_in_strcpy(&sbuff_1, "A") == 1);
	TEST_CHECK(strcmp(fr_sbuff_start(&sbuff_1), "A") == 0);
	TEST_SBUFF_USED(&sbuff_0, 1);
	TEST_SBUFF_LEN(&sbuff_0, 2);
	TEST_SBUFF_USED(&sbuff_1, 1);
	TEST_SBUFF_LEN(&sbuff_1, 2);

	TEST_CHECK(sbuff_0.start == sbuff_1.start);
	TEST_CHECK(sbuff_0.end == sbuff_1.end);
	TEST_CHECK(sbuff_0.p == sbuff_1.p);
}

static void test_talloc_extend_with_marker(void)
{
	fr_sbuff_t		sbuff_0, sbuff_1;
	fr_sbuff_marker_t	marker_0, marker_1;
	fr_sbuff_uctx_talloc_t	tctx;

	TEST_CASE("Initial allocation");
	TEST_CHECK(fr_sbuff_init_talloc(NULL, &sbuff_0, &tctx, 0, 50) == 0);
	TEST_SBUFF_USED(&sbuff_0, 0);
	TEST_SBUFF_LEN(&sbuff_0, 1);

	TEST_CASE("Print string - Should alloc one byte");
	TEST_CHECK(fr_sbuff_in_strcpy(&sbuff_0, "A") == 1);
	TEST_CHECK(strcmp(fr_sbuff_start(&sbuff_0), "A") == 0);
	TEST_SBUFF_USED(&sbuff_0, 1);
	TEST_SBUFF_LEN(&sbuff_0, 2);

	fr_sbuff_marker(&marker_0, &sbuff_0);
	TEST_CHECK((marker_0.p - sbuff_0.start) == 1);

	TEST_CASE("Print string - Ensure marker is updated");
	TEST_CHECK(fr_sbuff_in_strcpy(&sbuff_0, "B") == 1);
	TEST_CHECK(strcmp(fr_sbuff_start(&sbuff_0), "AB") == 0);
	TEST_SBUFF_USED(&sbuff_0, 2);
	TEST_SBUFF_LEN(&sbuff_0, 3);
	TEST_CHECK((marker_0.p - sbuff_0.start) == 1);

	TEST_CASE("Print string - Copy sbuff");
	sbuff_1 = FR_SBUFF_COPY(&sbuff_0);	/* Dup sbuff_0 */
	TEST_CHECK(sbuff_0.p == sbuff_1.start);
	fr_sbuff_marker(&marker_1, &sbuff_1);

	TEST_CHECK((marker_1.p - sbuff_1.start) == 0);
	TEST_CHECK((marker_1.p - sbuff_0.start) == 2);
	TEST_CHECK(sbuff_0.p == sbuff_1.start);

	TEST_CASE("Print string - Trigger re-alloc, ensure all pointers are updated");
	TEST_CHECK(fr_sbuff_in_strcpy(&sbuff_1, "C") == 1);
	TEST_CHECK(strcmp(fr_sbuff_start(&sbuff_1), "C") == 0);
	TEST_CHECK(sbuff_0.buff == sbuff_1.buff);
	TEST_CHECK(sbuff_0.p == sbuff_1.start + 1);
	TEST_CHECK((marker_1.p - sbuff_1.start) == 0);
	TEST_CHECK((marker_1.p - sbuff_0.start) == 2);
	TEST_SBUFF_USED(&sbuff_0, 3);
	TEST_SBUFF_LEN(&sbuff_0, 5);
}

TEST_LIST = {
	/*
	 *	Basic tests
	 */
	{ "fr_sbuff_init",			test_parse_init },
	{ "fr_sbuff_out_bstrncpy_exact",	test_strncpy_exact },
	{ "fr_sbuff_bstrncpy_out",		test_strncpy },

	/*
	 *	Extending buffer
	 */
	{ "fr_sbuff_talloc_extend",		test_talloc_extend },
	{ "fr_sbuff_talloc_extend_init_zero",	test_talloc_extend_init_zero },
	{ "fr_sbuff_talloc_extend_multi_level",	test_talloc_extend_multi_level },
	{ "fr_sbuff_talloc_extend_with_marker",	test_talloc_extend_with_marker },

	{ "fr_sbuff_no_advance",		test_no_advance },

	{ NULL }
};
