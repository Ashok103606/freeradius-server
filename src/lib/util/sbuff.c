/*
 *   This library is free software; you can redistribute it and/or
 *   modify it under the terms of the GNU Lesser General Public
 *   License as published by the Free Software Foundation; either
 *   version 2.1 of the License, or (at your option) any later version.
 *
 *   This library is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 *   Lesser General Public License for more details.
 *
 *   You should have received a copy of the GNU Lesser General Public
 *   License along with this library; if not, write to the Free Software
 *   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

/** A generic string buffer structure for string printing and parsing
 *
 * @file src/lib/util/sbuff.c
 *
 * @copyright 2020 Arran Cudbard-Bell <a.cudbardb@freeradius.org>
 */
RCSID("$Id$")

#include <freeradius-devel/util/print.h>
#include <freeradius-devel/util/sbuff.h>
#include <freeradius-devel/util/strerror.h>
#include <freeradius-devel/util/talloc.h>
#include <freeradius-devel/util/thread_local.h>

#include <ctype.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

_Thread_local char *sbuff_scratch;

static_assert(sizeof(long long) >= sizeof(int64_t), "long long must be as wide or wider than an int64_t");
static_assert(sizeof(unsigned long long) >= sizeof(uint64_t), "long long must be as wide or wider than an uint64_t");

fr_table_num_ordered_t const sbuff_parse_error_table[] = {
	{ "ok",			FR_SBUFF_PARSE_OK				},
	{ "token not found",	FR_SBUFF_PARSE_ERROR_NOT_FOUND			},
	{ "integer overflow",	FR_SBUFF_PARSE_ERROR_NUM_OVERFLOW		},
	{ "integer underflow",	FR_SBUFF_PARSE_ERROR_NUM_UNDERFLOW		},
};
size_t sbuff_parse_error_table_len = NUM_ELEMENTS(sbuff_parse_error_table);

#if defined(__clang_analyzer__) || !defined(NDEBUG)
#  define CHECK_SBUFF_INIT(_sbuff)	if (!(_sbuff)->buff || !(_sbuff)->start || !(_sbuff)->end || !(_sbuff)->p) return 0;
#else
#  define CHECK_SBUFF_INIT(_sbuff)
#endif

/** Update all markers and pointers in the set of sbuffs to point to new_buff
 *
 * This function should be used if the underlying buffer is realloced.
 *
 * @param[in] sbuff	to update.
 * @param[in] new_buff	to assign to to sbuff.
 * @param[in] new_len	Length of the new buffer.
 * @return
 *	- 0 on success.
 *	- <0 if pointer is out of range.
 */
int fr_sbuff_update(fr_sbuff_t *sbuff, char *new_buff, size_t new_len)
{
	fr_sbuff_t		*sbuff_i;
	char			*old_buff;	/* Current start */
	int			ret = 0;

	CHECK_SBUFF_INIT(sbuff);

#define update_ptr(_old_buff, _new_buff, _field) (_field = (_new_buff) + ((_field) - (_old_buff)))

	old_buff = sbuff->start;

	/*
	 *	Update pointers to point to positions
	 *	in new buffer based on their relative
	 *	offsets in the old buffer.
	 */
	for (sbuff_i = sbuff; sbuff_i; sbuff_i = sbuff_i->parent) {
		fr_sbuff_marker_t	*m_i;

		sbuff_i->buff = new_buff;

		update_ptr(old_buff, new_buff, sbuff_i->start);
		sbuff_i->end = sbuff_i->start + new_len;

		update_ptr(old_buff, new_buff, sbuff_i->p);
		if (unlikely(sbuff_i->p > sbuff_i->end)) {
			ret = -1;
			sbuff_i->p = sbuff_i->end;
		}

		for (m_i = sbuff_i->m; m_i; m_i = m_i->next) {
			update_ptr(old_buff, new_buff, m_i->p);
			if (unlikely(m_i->p > sbuff_i->end)) {
				ret = -1;
				sbuff_i->p = sbuff_i->end;
			}
		}
	}

	return ret;
#undef update_ptr
}

/** Shift the contents of the sbuff, returning the number of bytes we managed to shift
 *
 * @param[in] sbuff	to shift.
 * @param[in] shift	the contents of the buffer this many bytes
 *			towards the start of the buffer.
 * @return
 *	- 0 the shift failed due to constraining pointers.
 *	- >0 the number of bytes we managed to shift pointers
 *	  in the sbuff.  memmove should be used to move the
 *	  existing contents of the buffer, and fill the free
 *	  space at the end of the buffer with additional data.
 */
size_t fr_sbuff_shift(fr_sbuff_t *sbuff, size_t shift)
{
	fr_sbuff_t		*sbuff_i;
	char			*buff;		/* Current start */
	size_t			max_shift = shift;
	bool			reterminate = false;

	CHECK_SBUFF_INIT(sbuff);

#define update_ptr(_buff, _shift, _field) _field = ((_field) - (_shift)) <= (_buff) ? (_buff) : ((_field) - (_shift))
#define update_max_shift(_buff, _max_shift, _field) if (((_buff) + (_max_shift)) > (_field)) _max_shift -= (((_buff) + (_max_shift)) - (_field))

	buff = sbuff->start;

	/*
	 *	If the sbuff is already \0 terminated
	 *	and we're not working on a const buffer
	 *	then assume we need to re-terminate
	 *	later.
	 */
	reterminate = (*sbuff->p == '\0') && !sbuff->is_const;

	/*
	 *	Determine the maximum shift amount.
	 *	Shifts are constrained by pointers
	 *	into the buffer.
	 */
	for (sbuff_i = sbuff; sbuff_i; sbuff_i = sbuff_i->parent) {
		fr_sbuff_marker_t	*m_i;

		update_max_shift(buff, max_shift, sbuff_i->p);
		if (!max_shift) return 0;

		for (m_i = sbuff_i->m; m_i; m_i = m_i->next) {
			update_max_shift(buff, max_shift, m_i->p);
			if (!max_shift) return 0;
		}
	}

	for (sbuff_i = sbuff; sbuff_i; sbuff_i = sbuff_i->parent) {
		fr_sbuff_marker_t	*m_i;

		/*
		 *	Current position shifts, but stays the same
		 *	relative to content.
		 */
		update_ptr(buff, max_shift, sbuff_i->p);

		sbuff_i->shifted += max_shift;

		for (m_i = sbuff_i->m; m_i; m_i = m_i->next) update_ptr(buff, max_shift, m_i->p);
	}

	if (reterminate) *sbuff->p = '\0';

#undef update_ptr
#undef update_max_shift

	return max_shift;
}

/** Reallocate the current buffer
 *
 * @param[in] sbuff		to be extended.
 * @param[in] extension		How many additional bytes should be allocated
 *				in the buffer.
 * @return
 *	- 0 the extension operation failed.
 *	- >0 the number of bytes the buffer was extended by.
 */
size_t fr_sbuff_extend_talloc(fr_sbuff_t *sbuff, size_t extension)
{
	fr_sbuff_uctx_talloc_t	*tctx = sbuff->uctx;
	size_t			clen, nlen, elen = extension;
	char			*new_buff;

	CHECK_SBUFF_INIT(sbuff);

	clen = talloc_array_length(sbuff->buff);
	/*
	 *	If the current buffer size + the extension
	 *	is less than init, extend the buffer to init.
	 *
	 *	This can happen if the buffer has been
	 *	trimmed, and then additional data is added.
	 */
	if ((clen + elen) < tctx->init) {
		elen = (tctx->init - clen) + 1;	/* add \0 */
	/*
	 *	Double the buffer size if it's more than the
	 *	requested amount.
	 */
	} else if (elen < clen){
		elen = clen - 1;		/* Don't double alloc \0 */
	}

	/*
	 *	Check we don't exceed the maximum buffer
	 *	length.
	 */
	if (tctx->max && ((clen + elen) > tctx->max)) {
		elen = tctx->max - clen;
		if (elen == 0) {
			fr_strerror_printf("Failed extending buffer by %zu bytes to "
					   "%zu bytes, max is %zu bytes",
					   extension, clen + extension, tctx->max);
			return 0;
		}
		elen += 1;			/* add \0 */
	}
	nlen = clen + elen;

	new_buff = talloc_realloc(tctx->ctx, sbuff->buff, char, nlen);
	if (unlikely(!new_buff)) {
		fr_strerror_printf("Failed extending buffer by %zu bytes to %zu bytes", elen, nlen);
		return 0;
	}

	(void)fr_sbuff_update(sbuff, new_buff, nlen - 1);	/* Shouldn't fail as we're extending */

	return elen;
}

/** Trim a talloced sbuff to the minimum length required to represent the contained string
 *
 * @param[in] sbuff	to trim.
 * @return
 *	- 0 on success.
 *	- -1 on failure - markers present pointing past the end of string data.
 */
int fr_sbuff_trim_talloc(fr_sbuff_t *sbuff)
{
	size_t	clen, nlen;
	char	*new_buff;

	CHECK_SBUFF_INIT(sbuff);

	clen = talloc_array_length(sbuff->start);
	nlen = (sbuff->p - sbuff->start) + 1;

	if (nlen < clen) {
		new_buff = talloc_realloc(NULL, sbuff->start, char, nlen);
		if (!new_buff) {
			fr_strerror_printf("Failed trimming buffer from %zu to %zu", clen, nlen);
			return -1;
		}
		if (fr_sbuff_update(sbuff, new_buff, nlen - 1) < 0) return -1;
	}

	return 0;
}

/** Copy n bytes from the sbuff to a talloced buffer
 *
 * Will fail if output buffer is too small, or insufficient data is available in sbuff.
 *
 * @param[in] ctx	to allocate talloced buffer in.
 * @param[out] out	Where to copy to.
 * @param[in] in	Where to copy from.  Will copy len bytes from current position in buffer.
 * @param[in] len	How many bytes to copy.  If SIZE_MAX the entire buffer will be copied.
 * @return
 *      - 0 if insufficient bytes are available in the sbuff.
 *	- >0 the number of bytes copied to out.
 */
size_t fr_sbuff_out_talloc_bstrncpy_exact(TALLOC_CTX *ctx, char **out, fr_sbuff_t *in, size_t len)
{
	CHECK_SBUFF_INIT(in);

	if (len == SIZE_MAX) len = in->end - in->p;
	if ((in->p + len) > in->end) return 0;	/* Copying off the end of sbuff */

	*out = talloc_bstrndup(ctx, in->p, len);
	if (unlikely(!*out)) return 0;

	fr_sbuff_advance(in, len);

	return len;
}

/** Copy as many bytes as possible from the sbuff to a talloced buffer.
 *
 * Copy size is limited by available data in sbuff.
 *
 * @param[in] ctx	to allocate talloced buffer in.
 * @param[out] out	Where to copy to.
 * @param[in] in	Where to copy from.  Will copy len bytes from current position in buffer.
 * @param[in] len	How many bytes to copy.  If SIZE_MAX the entire buffer will be copied.
 * @return
 *	- 0 no bytes copied.
 *	- >0 the number of bytes copied.
 */
size_t fr_sbuff_out_talloc_bstrncpy(TALLOC_CTX *ctx, char **out, fr_sbuff_t *in, size_t len)
{
	CHECK_SBUFF_INIT(in);

	if (len > fr_sbuff_remaining(in)) len = fr_sbuff_remaining(in);
	if (len == 0) {
		*out = talloc_bstrndup(ctx, "", 0);
		return 0;
	}

	*out = talloc_bstrndup(ctx, in->p, len);
	if (unlikely(!*out)) return 0;

	fr_sbuff_advance(in, len);

	return len;
}

/** Copy as many allowed characters as possible from the sbuff to a talloced buffer.
 *
 * Copy size is limited by available data in sbuff and output buffer length.
 *
 * As soon as a disallowed character is found the copy is stopped.
 *
 * @param[in] ctx		to allocate talloced buffer in.
 * @param[out] out		Where to copy to.
 * @param[in] in		Where to copy from.  Will copy len bytes from current position in buffer.
 * @param[in] len		How many bytes to copy.  If SIZE_MAX the entire buffer will be copied.
 * @param[in] allowed_chars	Characters to include the copy.
 * @return
 *	- 0 no bytes copied.
 *	- >0 the number of bytes copied.
 */
size_t fr_sbuff_out_talloc_bstrncpy_allowed(TALLOC_CTX *ctx, char **out, fr_sbuff_t *in, size_t len,
					    bool const allowed_chars[static UINT8_MAX + 1])
{
	char const	*p = in->p;
	char const	*end;
	size_t		to_copy;

	CHECK_SBUFF_INIT(in);

	if (len > fr_sbuff_remaining(in)) len = fr_sbuff_remaining(in);
	if (len == 0) {
		*out = talloc_bstrndup(ctx, "", 0);
		return 0;
	}

	end = p + len;

	while ((p < end) && allowed_chars[(uint8_t)*p]) p++;
	to_copy = (p - in->p);

	*out = talloc_bstrndup(ctx, in->p, to_copy);
	if (unlikely(!*out)) return 0;

	fr_sbuff_advance(in, to_copy);

	return to_copy;
}

/** Copy as many allowed characters as possible from the sbuff to a talloced buffer
 *
 * Copy size is limited by available data in sbuff and output buffer length.
 *
 * As soon as a disallowed character is found the copy is stopped.
 *
 * @param[in] ctx		to allocate talloced buffer in.
 * @param[out] out		Where to copy to.
 * @param[in] in		Where to copy from.  Will copy len bytes from current position in buffer.
 * @param[in] len		How many bytes to copy.  If SIZE_MAX the entire buffer will be copied.
 * @param[in] until		Characters which stop the copy operation.
 * @return
 *	- 0 no bytes copied.
 *	- >0 the number of bytes copied.
 */
size_t fr_sbuff_out_talloc_bstrncpy_until(TALLOC_CTX *ctx, char **out, fr_sbuff_t *in, size_t len,
					  bool const until[static UINT8_MAX + 1])
{
	char const	*p = in->p;
	char const	*end;
	size_t		to_copy;

	CHECK_SBUFF_INIT(in);

	if (len > fr_sbuff_remaining(in)) len = fr_sbuff_remaining(in);
	if (len == 0) {
		*out = talloc_bstrndup(ctx, "", 0);
		return 0;
	}

	end = p + len;

	while ((p < end) && !until[(uint8_t)*p]) p++;
	to_copy = (p - in->p);

	*out = talloc_bstrndup(ctx, in->p, to_copy);
	if (unlikely(!*out)) return 0;

	fr_sbuff_advance(in, to_copy);

	return to_copy;
}

/** Copy n bytes from the sbuff to another buffer
 *
 * Will fail if output buffer is too small, or insufficient data is available in sbuff.
 *
 * @param[out] out	Where to copy to.
 * @param[in] outlen	Size of output buffer.
 * @param[in] in	Where to copy from.  Will copy len bytes from current position in buffer.
 * @param[in] len	How many bytes to copy.  If SIZE_MAX the entire buffer will be copied.
 * @return
 *      - 0 if insufficient bytes are available in the sbuff.
 *	- <0 the number of additional bytes we'd need in the output buffer as a negative value.
 *	- >0 the number of bytes copied to out.
 */
ssize_t fr_sbuff_out_bstrncpy_exact(char *out, size_t outlen, fr_sbuff_t *in, size_t len)
{
	CHECK_SBUFF_INIT(in);

	if (len == SIZE_MAX) len = in->end - in->p;
	if (unlikely(outlen == 0)) return -(len + 1);

	outlen--;	/* Account the \0 byte */

	if (len > outlen) return outlen - len;	/* Return how many bytes we'd need */
	if ((in->p + len) > in->end) return 0;	/* Copying off the end of sbuff */

	memcpy(out, in->p, len);
	out[len] = '\0';

	fr_sbuff_advance(in, len);

	return len;
}

#define STRNCPY_TRIM_LEN(_len, _in, _outlen) \
do { \
	if (_len == SIZE_MAX) _len = _in->end - _in->p; \
	if (_len > _outlen) _len = _outlen; \
	if ((_in->p + _len) > _in->end) _len = (_in->end - _in->p); \
} while(0)

/** Copy as many bytes as possible from the sbuff to another buffer
 *
 * Copy size is limited by available data in sbuff and output buffer length.
 *
 * @param[out] out		Where to copy to.
 * @param[in] outlen		Size of output buffer.
 * @param[in] in		Where to copy from.  Will copy len bytes from current position in buffer.
 * @param[in] len		How many bytes to copy.  If SIZE_MAX the entire buffer will be copied.
 * @return
 *	- 0 no bytes copied.
 *	- >0 the number of bytes copied.
 */
size_t fr_sbuff_out_bstrncpy(char *out, size_t outlen, fr_sbuff_t *in, size_t len)
{
	CHECK_SBUFF_INIT(in);

	if (unlikely(outlen == 0)) return 0;

	outlen--;	/* Account the \0 byte */ \

	STRNCPY_TRIM_LEN(len, in, outlen);

	memcpy(out, in->p, len);
	out[len] = '\0';

	fr_sbuff_advance(in, len);

	return len;
}

/** Copy as many allowed characters as possible from the sbuff to another buffer
 *
 * Copy size is limited by available data in sbuff and output buffer length.
 *
 * As soon as a disallowed character is found the copy is stopped.
 *
 * @param[out] out		Where to copy to.
 * @param[in] outlen		Size of output buffer.
 * @param[in] in		Where to copy from.  Will copy len bytes from current position in buffer.
 * @param[in] len		How many bytes to copy.  If SIZE_MAX the entire buffer will be copied.
 * @param[in] allowed_chars	Characters to include the copy.
 * @return
 *	- 0 no bytes copied.
 *	- >0 the number of bytes copied.
 */
size_t fr_sbuff_out_bstrncpy_allowed(char *out, size_t outlen, fr_sbuff_t *in, size_t len,
				     bool const allowed_chars[static UINT8_MAX + 1])
{
	char const	*p = in->p;
	char const	*end;
	char		*out_p = out;
	size_t		copied;

	CHECK_SBUFF_INIT(in);

	if (unlikely(outlen == 0)) return 0;

	outlen--;	/* Account the \0 byte */

	STRNCPY_TRIM_LEN(len, in, outlen);

	end = p + len;

	while ((p < end) && allowed_chars[(uint8_t)*p]) *out_p++ = *p++;
	*out_p = '\0';

	copied = (p - in->p);

	fr_sbuff_advance(in, copied);

	return copied;
}

/** Copy as many allowed characters as possible from the sbuff to another buffer
 *
 * Copy size is limited by available data in sbuff and output buffer length.
 *
 * As soon as a disallowed character is found the copy is stopped.
 *
 * @param[out] out		Where to copy to.
 * @param[in] outlen		Size of output buffer.
 * @param[in] in		Where to copy from.  Will copy len bytes from current position in buffer.
 * @param[in] len		How many bytes to copy.  If SIZE_MAX the entire buffer will be copied.
 * @param[in] until		Characters which stop the copy operation.
 * @return
 *	- 0 no bytes copied.
 *	- >0 the number of bytes copied.
 */
size_t fr_sbuff_out_bstrncpy_until(char *out, size_t outlen, fr_sbuff_t *in, size_t len,
				   bool const until[static UINT8_MAX + 1])
{
	char const	*p = in->p;
	char const	*end;
	char		*out_p = out;
	size_t		copied;

	CHECK_SBUFF_INIT(in);

	if (unlikely(outlen == 0)) return 0;

	outlen--;	/* Account the \0 byte */

	STRNCPY_TRIM_LEN(len, in, outlen);

	end = p + len;

	while ((p < end) && !until[(uint8_t)*p]) *out_p++ = *p++;
	*out_p = '\0';

	copied = (p - in->p);

	fr_sbuff_advance(in, copied);

	return copied;
}

/** Used to define a number parsing functions for singed integers
 *
 * @param[in] _name	Function suffix.
 * @param[in] _type	Output type.
 * @param[in] _min	value.
 * @param[in] _max	value.
 * @param[in] _max_char	Maximum digits that can be used to represent an integer.
 *			Can't use stringify because of width modifiers like 'u'
 *			used in <stdint.h>.
 * @return
 *	- 0 no bytes copied.  Examine err.
 *	- >0 the number of bytes copied.
 */
#define SBUFF_PARSE_INT_DEF(_name, _type, _min, _max, _max_char) \
size_t fr_sbuff_out_##_name(fr_sbuff_parse_error_t *err, _type *out, fr_sbuff_t *in, bool no_trailing) \
{ \
	char		buff[_max_char + 1]; \
	char		*end; \
	size_t		len; \
	long long	num; \
	fr_sbuff_t	our_in = FR_SBUFF_NO_ADVANCE(in); \
	len = fr_sbuff_out_bstrncpy(buff, sizeof(buff), &our_in, _max_char); \
	if (len == 0) { \
		if (err) *err = FR_SBUFF_PARSE_ERROR_NOT_FOUND; \
		return 0; \
	} \
	num = strtoll(buff, &end, 10); \
	if (end == buff) { \
		if (err) *err = FR_SBUFF_PARSE_ERROR_TRAILING; \
		return 0; \
	} \
	if ((num > (_max)) || ((errno == EINVAL) && (num == LLONG_MAX))) { \
		if (err) *err = FR_SBUFF_PARSE_ERROR_NUM_OVERFLOW; \
		*out = (_type)(_max); \
		return 0; \
	} else if (no_trailing && (*end != '\0')) { \
		if (err) *err = FR_SBUFF_PARSE_ERROR_TRAILING; \
		*out = (_type)(_max); \
		return 0; \
	} else if (num < (_min) || ((errno == EINVAL) && (num == LLONG_MIN))) { \
		if (err) *err = FR_SBUFF_PARSE_ERROR_NUM_UNDERFLOW; \
		*out = (_type)(_min); \
		return 0; \
	} else { \
		if (err) *err = FR_SBUFF_PARSE_OK; \
		*out = (_type)(num); \
	} \
	fr_sbuff_advance(in, end - buff); /* Advance by the length strtoll gives us */ \
	return end - buff; \
}

SBUFF_PARSE_INT_DEF(int8, int8_t, INT8_MIN, INT8_MAX, 2)
SBUFF_PARSE_INT_DEF(int16, int16_t, INT16_MIN, INT16_MAX, 6)
SBUFF_PARSE_INT_DEF(int32, int32_t, INT32_MIN, INT32_MAX, 11)
SBUFF_PARSE_INT_DEF(int64, int64_t, INT64_MIN, INT64_MAX, 20)

/** Used to define a number parsing functions for singed integers
 *
 * @param[in] _name	Function suffix.
 * @param[in] _type	Output type.
 * @param[in] _max	value.
 * @param[in] _max_char	Maximum digits that can be used to represent an integer.
 *			Can't use stringify because of width modifiers like 'u'
 *			used in <stdint.h>.
 */
#define SBUFF_PARSE_UINT_DEF(_name, _type, _max, _max_char) \
size_t fr_sbuff_out_##_name(fr_sbuff_parse_error_t *err, _type *out, fr_sbuff_t *in, bool no_trailing) \
{ \
	char			buff[_max_char + 1]; \
	char			*end; \
	size_t			len; \
	unsigned long long	num; \
	fr_sbuff_t		our_in = FR_SBUFF_NO_ADVANCE(in); \
	len = fr_sbuff_out_bstrncpy(buff, sizeof(buff), &our_in, _max_char); \
	if (len == 0) { \
		if (err) *err = FR_SBUFF_PARSE_ERROR_NOT_FOUND; \
		return 0; \
	} \
	num = strtoull(buff, &end, 10); \
	if (end == buff) { \
		if (err) *err = FR_SBUFF_PARSE_ERROR_TRAILING; \
		return 0; \
	} \
	if ((num > (_max)) || ((errno == EINVAL) && (num == ULLONG_MAX))) { \
		if (err) *err = FR_SBUFF_PARSE_ERROR_NUM_OVERFLOW; \
		*out = (_type)(_max); \
		return 0; \
	} else if (no_trailing && (*end != '\0')) { \
		if (err) *err = FR_SBUFF_PARSE_ERROR_TRAILING; \
		*out = (_type)(_max); \
		return 0; \
	} else { \
		if (err) *err = FR_SBUFF_PARSE_OK; \
		*out = (_type)(num); \
	} \
	fr_sbuff_advance(in, end - buff); /* Advance by the length strtoull gives us */ \
	return end - buff; \
}

SBUFF_PARSE_UINT_DEF(uint8, uint8_t, UINT8_MAX, 1)
SBUFF_PARSE_UINT_DEF(uint16, uint16_t, UINT16_MAX, 5)
SBUFF_PARSE_UINT_DEF(uint32, uint32_t, UINT32_MAX, 10)
SBUFF_PARSE_UINT_DEF(uint64, uint64_t, UINT64_MAX, 20)

static bool float_chars[UINT8_MAX + 1] = {
	['0'] = true, ['1'] = true, ['2'] = true, ['3'] = true, ['4'] = true,
	['5'] = true, ['6'] = true, ['7'] = true, ['8'] = true, ['9'] = true,
	['-'] = true, ['+'] = true, ['e'] = true, ['E'] = true, ['.'] = true,
};

/** Used to define a number parsing functions for floats
 *
 * @param[in] _name	Function suffix.
 * @param[in] _type	Output type.
 * @param[in] _func	Parsing function to use.
 * @param[in] _max_char	Maximum digits that can be used to represent an integer.
 *			Can't use stringify because of width modifiers like 'u'
 *			used in <stdint.h>.
 */
#define SBUFF_PARSE_FLOAT_DEF(_name, _type, _func, _max_char) \
size_t fr_sbuff_out_##_name(fr_sbuff_parse_error_t *err, _type *out, fr_sbuff_t *in, bool no_trailing) \
{ \
	char		buffer[_max_char + 1]; \
	char		*end; \
	fr_sbuff_t	our_in = FR_SBUFF_NO_ADVANCE(in); \
	size_t		len; \
	_type		res; \
	len = fr_sbuff_out_bstrncpy_allowed(buffer, sizeof(buffer), &our_in, SIZE_MAX, float_chars); \
	if (len == sizeof(buffer)) { \
		if (err) *err = FR_SBUFF_PARSE_ERROR_TRAILING; \
		return 0; \
	} else if (len == 0) { \
		if (err) *err = FR_SBUFF_PARSE_ERROR_NOT_FOUND; \
		return 0; \
	} \
	res = _func(buffer, &end); \
	if (errno == ERANGE) { \
		if (err) *err = res == 0 ? FR_SBUFF_PARSE_ERROR_NUM_UNDERFLOW : FR_SBUFF_PARSE_ERROR_NUM_OVERFLOW; \
		return 0; \
	} \
	if (no_trailing && (*end != '\0')) { \
		if (err) *err = FR_SBUFF_PARSE_ERROR_TRAILING; \
		*out = res; \
		return 0; \
	} \
	return fr_sbuff_advance(in, end - buffer); \
}

SBUFF_PARSE_FLOAT_DEF(float32, float, strtof, 100);
SBUFF_PARSE_FLOAT_DEF(float64, double, strtod, 100);

/** Copy bytes into the sbuff up to the first \0
 *
 * @param[in] sbuff	to copy into.
 * @param[in] str	to copy into buffer.
 * @return
 *	- >= 0 the number of bytes copied into the sbuff.
 *	- <0 the number of bytes required to complete the copy operation.
 */
ssize_t fr_sbuff_in_strcpy(fr_sbuff_t *sbuff, char const *str)
{
	size_t len;

	CHECK_SBUFF_INIT(sbuff);

	len = strlen(str);
	FR_SBUFF_EXTEND_OR_RETURN(sbuff, len);

	strlcpy(sbuff->p, str, len + 1);

	return fr_sbuff_advance(sbuff, len);
}

/** Copy bytes into the sbuff up to the first \0
 *
 * @param[in] sbuff	to copy into.
 * @param[in] str	to copy into buffer.
 * @param[in] len	number of bytes to copy.
 * @return
 *	- >= 0 the number of bytes copied into the sbuff.
 *	- <0 the number of bytes required to complete the copy operation.
 */
ssize_t fr_sbuff_in_bstrncpy(fr_sbuff_t *sbuff, char const *str, size_t len)
{
	CHECK_SBUFF_INIT(sbuff);

	FR_SBUFF_EXTEND_OR_RETURN(sbuff, len);

	memcpy(sbuff->p, str, len);
	sbuff->p[len] = '\0';

	return fr_sbuff_advance(sbuff, len);
}

/** Copy bytes into the sbuff up to the first \0
 *
 * @param[in] sbuff	to copy into.
 * @param[in] str	talloced buffer to copy into sbuff.
 * @return
 *	- >= 0 the number of bytes copied into the sbuff.
 *	- <0 the number of bytes required to complete the copy operation.
 */
ssize_t fr_sbuff_in_bstrcpy_buffer(fr_sbuff_t *sbuff, char const *str)
{
	size_t len;

	CHECK_SBUFF_INIT(sbuff);

	len = talloc_array_length(str) - 1;

	FR_SBUFF_EXTEND_OR_RETURN(sbuff, len);

	memcpy(sbuff->p, str, len);
	sbuff->p[len] = '\0';

	return fr_sbuff_advance(sbuff, len);
}

/** Free the scratch buffer used for printf
 *
 */
static void _sbuff_scratch_free(void *arg)
{
	talloc_free(arg);
}

static inline CC_HINT(always_inline) int sbuff_scratch_init(TALLOC_CTX **out)
{
	TALLOC_CTX	*scratch;

	scratch = sbuff_scratch;
	if (!scratch) {
		scratch = talloc_pool(NULL, 4096);
		if (unlikely(!scratch)) {
			fr_strerror_printf("Out of Memory");
			return -1;
		}
		fr_thread_local_set_destructor(sbuff_scratch, _sbuff_scratch_free, scratch);
	}

	*out = scratch;

	return 0;
}

/** Print using a fmt string to an sbuff
 *
 * @param[in] sbuff	to print into.
 * @param[in] fmt	string.
 * @param[in] ap	arguments for format string.
 * @return
 *	- >= 0 the number of bytes printed into the sbuff.
 *	- <0 the number of bytes required to complete the print operation.
 */
ssize_t fr_sbuff_in_vsprintf(fr_sbuff_t *sbuff, char const *fmt, va_list ap)
{
	TALLOC_CTX	*scratch;
	va_list		ap_p;
	char		*tmp;
	ssize_t		slen;

	CHECK_SBUFF_INIT(sbuff);

	if (sbuff_scratch_init(&scratch) < 0) return 0;

	va_copy(ap_p, ap);
	tmp = fr_vasprintf(scratch, fmt, ap_p);
	va_end(ap_p);
	if (!tmp) return 0;

	slen = fr_sbuff_in_bstrcpy_buffer(sbuff, tmp);
	talloc_free(tmp);	/* Free the temporary buffer */

	return slen;
}

/** Print using a fmt string to an sbuff
 *
 * @param[in] sbuff	to print into.
 * @param[in] fmt	string.
 * @param[in] ...	arguments for format string.
 * @return
 *	- >= 0 the number of bytes printed into the sbuff.
 *	- <0 the number of bytes required to complete the print operation.
 */
ssize_t fr_sbuff_in_sprintf(fr_sbuff_t *sbuff, char const *fmt, ...)
{
	va_list		ap;
	ssize_t		slen;

	va_start(ap, fmt);
	slen = fr_sbuff_in_vsprintf(sbuff, fmt, ap);
	va_end(ap);

	return slen;
}

/** Print an escaped string to an sbuff
 *
 * @param[in] sbuff	to print into.
 * @param[in] in	to escape.
 * @param[in] inlen	of string to escape.
 * @param[in] quote	Which quoting character to escape.  Also controls
 *			which characters are escaped.
 * @return
 *	- >= 0 the number of bytes printed into the sbuff.
 *	- <0 the number of bytes required to complete the print operation.
 */
ssize_t fr_sbuff_in_snprint(fr_sbuff_t *sbuff, char const *in, size_t inlen, char quote)
{
	size_t		len;

	CHECK_SBUFF_INIT(sbuff);

	len = fr_snprint_len(in, inlen, quote);
	FR_SBUFF_EXTEND_OR_RETURN(sbuff, len);

	len = fr_snprint(fr_sbuff_current(sbuff), fr_sbuff_remaining(sbuff) + 1, in, inlen, quote);
	fr_sbuff_advance(sbuff, len);

	return len;
}

/** Print an escaped string to an sbuff taking a talloced buffer as input
 *
 * @param[in] sbuff	to print into.
 * @param[in] in	to escape.
 * @param[in] quote	Which quoting character to escape.  Also controls
 *			which characters are escaped.
 * @return
 *	- >= 0 the number of bytes printed into the sbuff.
 *	- <0 the number of bytes required to complete the print operation.
 */
ssize_t fr_sbuff_in_snprint_buffer(fr_sbuff_t *sbuff, char const *in, char quote)
{
	if (unlikely(!in)) return 0;

	return fr_sbuff_in_snprint(sbuff, in, talloc_array_length(in) - 1, quote);
}

/** Return true and advance past the end of the needle if needle occurs next in the sbuff
 *
 * @param[in] sbuff	to search in.
 * @param[in] needle	to search for.
 * @param[in] len	of needle. If SIZE_MAX strlen is used
 *			to determine length of the needle.
 */
bool fr_sbuff_adv_past_str(fr_sbuff_t *sbuff, char const *needle, size_t len)
{
	char const *found;

	CHECK_SBUFF_INIT(sbuff);

	if (len == SIZE_MAX) len = strlen(needle);
	if ((sbuff->p + len) >= sbuff->end) return false;

	found = memmem(sbuff->p, len, needle, len);	/* sbuff len and needle len ensures match must be next */
	if (!found) return false;

	fr_sbuff_advance(sbuff, len);

	return true;
}

/** Return true and advance past the end of the needle if needle occurs next in the sbuff
 *
 * This function is similar to fr_sbuff_adv_past_str but is case insensitive.
 *
 * @param[in] sbuff	to search in.
 * @param[in] needle	to search for.
 * @param[in] len	of needle. If SIZE_MAX strlen is used
 *			to determine length of the needle.
 */
bool fr_sbuff_adv_past_strcase(fr_sbuff_t *sbuff, char const *needle, size_t len)
{
	char const *p, *n_p;
	char const *end;

	CHECK_SBUFF_INIT(sbuff);

	if (len == SIZE_MAX) len = strlen(needle);
	if ((sbuff->p + len) >= sbuff->end) return false;

	p = sbuff->p;
	end = p + len;

	for (p = sbuff->p, n_p = needle; p < end; p++, n_p++) {
		if (tolower(*p) != tolower(*n_p)) return false;
	}

	fr_sbuff_advance(sbuff, len);

	return true;
}

/** Wind position to the first non-whitespace character
 *
 * @param[in] sbuff		sbuff to search in.
 * @return
 *	- 0, first character is not a whitespace character.
 *	- >0 how many whitespace characters we skipped.
 */
size_t fr_sbuff_adv_past_whitespace(fr_sbuff_t *sbuff)
{
	char const *p = sbuff->p;

	CHECK_SBUFF_INIT(sbuff);

	while ((p < sbuff->end) && isspace(*(sbuff->p))) p++;

	return fr_sbuff_set(sbuff, p);
}

/** Wind position to first instance of specified multibyte utf8 char
 *
 * Only use this function if the search char could be multibyte,
 * as there's a large performance penalty.
 *
 * @param[in,out] sbuff		to search in.
 * @param[in] chr		to search for.
 * @return
 *	- 0, no instances found.
 *	- >0 the offset at which the first occurrence of the multi-byte chr was found.
 */
size_t fr_sbuff_adv_to_strchr_utf8(fr_sbuff_t *sbuff, char *chr)
{
	char const *found;
	char const *p = sbuff->p_i;

	CHECK_SBUFF_INIT(sbuff);

	found = fr_utf8_strchr(NULL, p, sbuff->end - sbuff->p, chr);
	if (!found) return 0;

	return (size_t)fr_sbuff_advance(sbuff, found - p);
}

/** Wind position to first instance of specified char
 *
 * @param[in,out] sbuff		to search in.
 * @param[in] c			to search for.
 * @return
 *	- 0, no instances found.
 *	- >0 the offset at which the first occurrence of the char was found.
 */
size_t fr_sbuff_adv_to_strchr(fr_sbuff_t *sbuff, char c)
{
	char const *found;
	char const *p = sbuff->p_i;

	CHECK_SBUFF_INIT(sbuff);

	found = memchr(sbuff->p, c, sbuff->end - sbuff->p);
	if (!found) return 0;

	return (size_t)fr_sbuff_advance(sbuff, found - p);
}

/** Wind position to the first instance of the specified needle
 *
 * @param[in,out] sbuff		sbuff to search in.
 * @param[in] needle		to search for.
 * @param[in] len		Length of the needle.  -1 to use strlen.
 * @return
 *	- 0, no instances found.
 *	- >0 the offset at which the first occurrence of the needle was found.
 */
size_t fr_sbuff_adv_to_strstr(fr_sbuff_t *sbuff, char const *needle, ssize_t len)
{
	char const *found;
	char const *p = sbuff->p;

	CHECK_SBUFF_INIT(sbuff);

	if (len < 0) len = strlen(needle);

	found = memmem(sbuff->p, sbuff->end - sbuff->p, needle, len);
	if (!found) return 0;

	return (size_t)fr_sbuff_advance(sbuff, found - p);
}

/** Return true if the current char matches, and if it does, advance
 *
 */
bool fr_sbuff_next_if_char(fr_sbuff_t *sbuff, char c)
{
	CHECK_SBUFF_INIT(sbuff);

	if (sbuff->p >= sbuff->end) return false;
	if (*sbuff->p != c) return false;

	fr_sbuff_advance(sbuff, 1);

	return true;
}

/** Return true and advance if the next char does not match
 *
 */
bool fr_sbuff_next_unless_char(fr_sbuff_t *sbuff, char c)
{
	CHECK_SBUFF_INIT(sbuff);

	if (sbuff->p >= sbuff->end) return false;
	if (*sbuff->p == c) return false;

	fr_sbuff_advance(sbuff, 1);

	return true;
}
