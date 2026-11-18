/****************************************************************************
**
** Copyright (C) 2026 Intel Corporation
**
** Permission is hereby granted, free of charge, to any person obtaining a copy
** of this software and associated documentation files (the "Software"), to deal
** in the Software without restriction, including without limitation the rights
** to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
** copies of the Software, and to permit persons to whom the Software is
** furnished to do so, subject to the following conditions:
**
** The above copyright notice and this permission notice shall be included in
** all copies or substantial portions of the Software.
**
** THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
** IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
** FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
** AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
** LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
** OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
** THE SOFTWARE.
**
****************************************************************************/

#include "cbor.h"
#include "cborjson.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;

#define CHECK(cond) \
    do { \
        if (!(cond)) { \
            fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #cond); \
            ++failures; \
        } \
    } while (0)

/* Converts |data| to JSON, capturing how many bytes were committed to the
 * output stream and what they were. */
static CborError convert_budget(const uint8_t *data, size_t len, int max_recursions,
                                size_t max_output_bytes, char *out, size_t *out_len)
{
    CborParser parser;
    CborValue value;
    CborError err;
    FILE *stream;
    long pos;

    err = cbor_parser_init(data, len, 0, &parser, &value);
    if (err)
        return err;

    stream = tmpfile();
    if (stream == NULL)
        return CborErrorIO;

    err = cbor_value_to_json_advance_budget(stream, &value, 0, max_recursions, max_output_bytes);
    pos = ftell(stream);
    *out_len = pos < 0 ? 0 : (size_t)pos;
    if (out != NULL) {
        rewind(stream);
        if (*out_len != 0 && fread(out, 1, *out_len, stream) != *out_len)
            fprintf(stderr, "%s:%d: failed to read back output\n", __FILE__, __LINE__);
        out[*out_len] = '\0';
    }
    fclose(stream);
    return err;
}

static CborError convert_default(const uint8_t *data, size_t len, char *out, size_t *out_len)
{
    CborParser parser;
    CborValue value;
    CborError err;
    FILE *stream;
    long pos;

    err = cbor_parser_init(data, len, 0, &parser, &value);
    if (err)
        return err;

    stream = tmpfile();
    if (stream == NULL)
        return CborErrorIO;

    err = cbor_value_to_json_advance(stream, &value, 0);
    pos = ftell(stream);
    *out_len = pos < 0 ? 0 : (size_t)pos;
    if (out != NULL) {
        rewind(stream);
        if (*out_len != 0 && fread(out, 1, *out_len, stream) != *out_len)
            fprintf(stderr, "%s:%d: failed to read back output\n", __FILE__, __LINE__);
        out[*out_len] = '\0';
    }
    fclose(stream);
    return err;
}

static void test_runtime_recursion_budget(void)
{
    /* 8 nested arrays around an integer */
    static const uint8_t nested_arrays[] = {
        0x81, 0x81, 0x81, 0x81, 0x81, 0x81, 0x81, 0x81, 0x00
    };
    /* 4 nested tags around an integer */
    static const uint8_t nested_tags[] = { 0xc1, 0xc1, 0xc1, 0xc1, 0x00 };
    /* 4 nested maps, each mapping "": <inner> */
    static const uint8_t nested_maps[] = {
        0xa1, 0x60, 0xa1, 0x60, 0xa1, 0x60, 0xa1, 0x60, 0x00
    };
    char buffer[64];
    size_t len;
    CborError err;

    /* a small runtime budget fails and commits no bytes */
    err = convert_budget(nested_arrays, sizeof(nested_arrays), 4, 0, buffer, &len);
    CHECK(err == CborErrorNestingTooDeep);
    CHECK(len == 0);

    err = convert_budget(nested_tags, sizeof(nested_tags), 2, 0, buffer, &len);
    CHECK(err == CborErrorNestingTooDeep);
    CHECK(len == 0);

    err = convert_budget(nested_maps, sizeof(nested_maps), 2, 0, buffer, &len);
    CHECK(err == CborErrorNestingTooDeep);
    CHECK(len == 0);

    /* a sufficient budget converts normally (the innermost integer also
     * counts against the budget, so 8 arrays require a budget of 9) */
    err = convert_budget(nested_arrays, sizeof(nested_arrays), 9, 0, buffer, &len);
    CHECK(err == CborNoError);
    CHECK(len == strlen("[[[[[[[[0]]]]]]]]"));
    CHECK(strcmp(buffer, "[[[[[[[[0]]]]]]]]") == 0);

    /* a negative budget selects the compile-time default; normal depth works */
    err = convert_budget(nested_arrays, sizeof(nested_arrays), -1, 0, buffer, &len);
    CHECK(err == CborNoError);
    CHECK(strcmp(buffer, "[[[[[[[[0]]]]]]]]") == 0);
}

static void test_runtime_budget_beyond_compile_time(void)
{
    /* nesting deeper than the compile-time CBOR_PARSER_MAX_RECURSIONS */
    const size_t depth = 1025;
    uint8_t *data = (uint8_t *)malloc(depth + 1);
    size_t len;
    CborError err;

    CHECK(data != NULL);
    if (data == NULL)
        return;
    memset(data, 0x81, depth);
    data[depth] = 0x00;

    /* the default budget rejects it and commits nothing */
    err = convert_budget(data, depth + 1, -1, 0, NULL, &len);
    CHECK(err == CborErrorNestingTooDeep);
    CHECK(len == 0);

    /* a larger runtime budget permits it */
    err = convert_budget(data, depth + 1, (int)depth + 1, 0, NULL, &len);
    CHECK(err == CborNoError);
    CHECK(len == 2 * depth + 1);

    free(data);
}

static void test_output_budget(void)
{
    static const uint8_t text[] = { 0x65, 'H', 'e', 'l', 'l', 'o' };
    static const uint8_t array[] = { 0x83, 0x01, 0x02, 0x03 };
    char buffer[64];
    size_t len;
    CborError err;

    /* exact budget succeeds: "\"Hello\"" is 7 bytes */
    err = convert_budget(text, sizeof(text), -1, 7, buffer, &len);
    CHECK(err == CborNoError);
    CHECK(len == 7);
    CHECK(strcmp(buffer, "\"Hello\"") == 0);

    /* one byte less fails and commits nothing */
    err = convert_budget(text, sizeof(text), -1, 6, buffer, &len);
    CHECK(err == CborErrorDataTooLarge);
    CHECK(len == 0);

    /* "[1,2,3]" is 7 bytes */
    err = convert_budget(array, sizeof(array), -1, 6, buffer, &len);
    CHECK(err == CborErrorDataTooLarge);
    CHECK(len == 0);

    /* zero budget means unlimited */
    err = convert_budget(text, sizeof(text), -1, 0, buffer, &len);
    CHECK(err == CborNoError);
    CHECK(len == 7);
}

static void test_floating_point_special_values(void)
{
    static const uint8_t nan_double[] = { 0xfb, 0x7f, 0xf8, 0, 0, 0, 0, 0, 0 };
    static const uint8_t neg_inf_double[] = { 0xfb, 0xff, 0xf0, 0, 0, 0, 0, 0, 0 };
    static const uint8_t inf_float[] = { 0xfa, 0x7f, 0x80, 0, 0 };
    char buffer[64];
    size_t len;
    CborError err;

    /* non-finite values convert to JSON null, as before */
    err = convert_budget(nan_double, sizeof(nan_double), -1, 0, buffer, &len);
    CHECK(err == CborNoError);
    CHECK(strcmp(buffer, "null") == 0);

    err = convert_budget(neg_inf_double, sizeof(neg_inf_double), -1, 0, buffer, &len);
    CHECK(err == CborNoError);
    CHECK(strcmp(buffer, "null") == 0);

    err = convert_budget(inf_float, sizeof(inf_float), -1, 0, buffer, &len);
    CHECK(err == CborNoError);
    CHECK(strcmp(buffer, "null") == 0);

    /* they are also subject to the output budget */
    err = convert_budget(nan_double, sizeof(nan_double), -1, 3, buffer, &len);
    CHECK(err == CborErrorDataTooLarge);
    CHECK(len == 0);
}

static void test_no_partial_output_on_error(void)
{
    /* invalid UTF-8 in the second element of an array */
    static const uint8_t bad_utf8[] = { 0x82, 0x61, 'H', 0x61, 0xff };
    /* non-string map key without CborConvertStringifyMapKeys */
    static const uint8_t bad_key[] = { 0xa1, 0x00, 0x01 };
    char buffer[64];
    size_t len;
    CborError err;

    err = convert_budget(bad_utf8, sizeof(bad_utf8), -1, 0, buffer, &len);
    CHECK(err == CborErrorInvalidUtf8TextString);
    CHECK(len == 0);

    err = convert_budget(bad_key, sizeof(bad_key), -1, 0, buffer, &len);
    CHECK(err == CborErrorJsonObjectKeyNotString);
    CHECK(len == 0);

    /* the classic entry point is transactional too */
    err = convert_default(bad_utf8, sizeof(bad_utf8), buffer, &len);
    CHECK(err == CborErrorInvalidUtf8TextString);
    CHECK(len == 0);
}

static void test_normal_conversion(void)
{
    static const uint8_t data[] = { 0x82, 0x01, 0x61, 'x' };
    char buffer[64];
    size_t len;
    CborError err;

    err = convert_default(data, sizeof(data), buffer, &len);
    CHECK(err == CborNoError);
    CHECK(strcmp(buffer, "[1,\"x\"]") == 0);
}

int main(void)
{
    test_runtime_recursion_budget();
    test_runtime_budget_beyond_compile_time();
    test_output_budget();
    test_floating_point_special_values();
    test_no_partial_output_on_error();
    test_normal_conversion();

    if (failures) {
        fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    printf("all checks passed\n");
    return 0;
}
