/*
 * Optional native accelerator for ebmlite's reading/decoding.
 *
 * Everything here mirrors a pure-Python implementation in `decoding.py` or
 * `core.py`; the pure-Python versions are the reference, and the results
 * (including the exceptions raised for bad data) are intended to be
 * identical.
 *
 * All I/O goes through the Python stream protocol (`seek()` and `read()`),
 * so any seekable file-like object works, not just real files. No stream
 * state is kept between calls: every call seeks explicitly before reading,
 * so it is safe to use with `ThreadAwareFile` and with streams that are also
 * being read/seeked by other code.
 */
#define PY_SSIZE_T_CLEAN
#include <Python.h>
#include <datetime.h>
#include <stdint.h>
#include <string.h>

/* Type codes for values decoded during `scanElements()`. Exposed to Python
 * as module constants (`TYPE_INT`, etc.). */
#define TYPE_NONE 0
#define TYPE_INT 1
#define TYPE_UINT 2
#define TYPE_FLOAT 3
#define TYPE_STRING 4
#define TYPE_UNICODE 5
#define TYPE_DATE 6
#define TYPE_BINARY 7

/* The number of bytes initially read at a time when scanning. It doubles
 * with each subsequent read, up to `SCAN_CHUNK_MAX`. Starting small keeps
 * reading only the first few elements (e.g. indexing a `Document`) fast;
 * growing keeps long scans efficient. */
#define SCAN_CHUNK_MIN 4096
#define SCAN_CHUNK_MAX 65536

/* Largest payload that will be decoded during a scan; bigger ones are left
 * for Python to read lazily. */
#define MAX_SCAN_PAYLOAD 65536

/* The longest possible element header: 4 byte ID + 8 byte size. */
#define MAX_HEADER 12

/* Microseconds per day, for building `timedelta`s. */
#define US_PER_DAY 86400000000LL

static PyObject *strRead = NULL;
static PyObject *strSeek = NULL;
static PyObject *dateEpoch = NULL;      /* datetime(2001, 1, 1) */
static PyObject *structError = NULL;    /* struct.error */


/* ===========================================================================
 * Low-level decoding of raw bytes
 * ======================================================================== */

/* Get the length of an EBML variable-length integer from its first byte
 * (the number of leading zeros + 1). Returns 8 for a first byte of 0,
 * consistent with `decoding.decodeIntLength()`. */
static inline int
vintLength(unsigned char b)
{
    int len = 1;
    unsigned char mask = 0x80;
    while (len < 8 && !(b & mask)) {
        mask >>= 1;
        len++;
    }
    return len;
}


/* Decode big-endian unsigned bytes, the way `decoding.readUInt()` does:
 * up to 8 bytes are right-justified; if more than 8, only the first 8 are
 * used. */
static inline uint64_t
decodeUIntBytes(const unsigned char *data, Py_ssize_t len)
{
    uint64_t v = 0;
    Py_ssize_t i;
    if (len > 8)
        len = 8;
    for (i = 0; i < len; i++)
        v = (v << 8) | data[i];
    return v;
}


/* Decode big-endian signed bytes, the way `decoding.readInt()` does.
 * `len` must be > 0. */
static inline int64_t
decodeIntBytes(const unsigned char *data, Py_ssize_t len)
{
    uint64_t v = (data[0] & 0x80) ? UINT64_MAX : 0;
    Py_ssize_t i;
    if (len > 8)
        len = 8;
    for (i = 0; i < len; i++)
        v = (v << 8) | data[i];
    return (int64_t)v;
}


/* Decode a big-endian IEEE 754 float of 4 or 8 bytes. */
static double
decodeFloatBytes(const unsigned char *data, Py_ssize_t len)
{
    if (len == 4) {
        uint32_t u = (uint32_t)decodeUIntBytes(data, 4);
        float f;
        memcpy(&f, &u, 4);
        return (double)f;
    }
    else {
        uint64_t u = decodeUIntBytes(data, 8);
        double d;
        memcpy(&d, &u, 8);
        return d;
    }
}


/* Create a `datetime` from an EBML date (nanoseconds since 2001-01-01). */
static PyObject *
makeDate(int64_t nanoseconds)
{
    /* Floor division, like Python's `//` */
    int64_t us = nanoseconds / 1000;
    int64_t days, rem;
    PyObject *delta, *result;

    if ((nanoseconds % 1000) && (nanoseconds < 0))
        us -= 1;

    days = us / US_PER_DAY;
    rem = us % US_PER_DAY;
    if (rem < 0) {
        rem += US_PER_DAY;
        days -= 1;
    }

    delta = PyDelta_FromDSU((int)days, (int)(rem / 1000000),
                            (int)(rem % 1000000));
    if (delta == NULL)
        return NULL;
    result = PyNumber_Add(dateEpoch, delta);
    Py_DECREF(delta);
    return result;
}


/* Get the length of a string up to (but not including) the first null. */
static inline Py_ssize_t
strLength(const unsigned char *data, Py_ssize_t len)
{
    const unsigned char *nul = memchr(data, 0, (size_t)len);
    return nul ? (Py_ssize_t)(nul - data) : len;
}


/* Decode a complete payload, already in memory, as the given type. Returns
 * a new reference, or NULL (with no exception set) if the value can't be
 * decoded here and should be left for the Python implementation (which
 * will produce the 'official' result or exception). */
static PyObject *
decodePayload(int typeCode, const unsigned char *data, Py_ssize_t size)
{
    PyObject *result = NULL;

    switch (typeCode) {
    case TYPE_INT:
        if (size == 0)
            return PyLong_FromLong(0);
        if (size > 8)
            return NULL;
        result = PyLong_FromLongLong(decodeIntBytes(data, size));
        break;
    case TYPE_UINT:
        if (size > 8)
            return NULL;
        result = PyLong_FromUnsignedLongLong(decodeUIntBytes(data, size));
        break;
    case TYPE_FLOAT:
        if (size == 0)
            return PyFloat_FromDouble(0.0);
        if (size != 4 && size != 8)
            return NULL;
        result = PyFloat_FromDouble(decodeFloatBytes(data, size));
        break;
    case TYPE_STRING:
        result = PyUnicode_DecodeASCII((const char *)data,
                                       strLength(data, size), NULL);
        break;
    case TYPE_UNICODE:
        result = PyUnicode_DecodeUTF8((const char *)data,
                                      strLength(data, size), NULL);
        break;
    case TYPE_DATE:
        if (size != 8)
            return NULL;
        result = makeDate(decodeIntBytes(data, 8));
        break;
    case TYPE_BINARY:
        result = PyByteArray_FromStringAndSize((const char *)data, size);
        break;
    default:
        return NULL;
    }

    if (result == NULL) {
        /* Bad data (e.g. invalid ASCII). Let Python handle it later. */
        PyErr_Clear();
    }
    return result;
}


/* ===========================================================================
 * Stream access
 * ======================================================================== */

static int
streamSeek(PyObject *stream, long long pos)
{
    PyObject *p, *r;
    p = PyLong_FromLongLong(pos);
    if (p == NULL)
        return -1;
    r = PyObject_CallMethodOneArg(stream, strSeek, p);
    Py_DECREF(p);
    if (r == NULL)
        return -1;
    Py_DECREF(r);
    return 0;
}


/* Call `stream.read(n)` once. Returns a new reference to a bytes-like
 * object, or `Py_None` at EOF (some streams return `None` instead of
 * empty bytes). */
static PyObject *
streamReadOnce(PyObject *stream, Py_ssize_t n)
{
    PyObject *p, *r;
    p = PyLong_FromSsize_t(n);
    if (p == NULL)
        return NULL;
    r = PyObject_CallMethodOneArg(stream, strRead, p);
    Py_DECREF(p);
    return r;
}


/* Read up to `n` bytes from the stream's current position into `dest`,
 * calling `read()` repeatedly if it returns less than requested (as pipes
 * and network streams may). Returns the number of bytes read (fewer than
 * `n` only at EOF), or -1 on error. */
static Py_ssize_t
streamReadInto(PyObject *stream, unsigned char *dest, Py_ssize_t n)
{
    Py_ssize_t got = 0;
    while (got < n) {
        Py_buffer view;
        PyObject *data = streamReadOnce(stream, n - got);
        if (data == NULL)
            return -1;
        if (data == Py_None) {
            Py_DECREF(data);
            break;
        }
        if (PyObject_GetBuffer(data, &view, PyBUF_SIMPLE) < 0) {
            Py_DECREF(data);
            return -1;
        }
        if (view.len == 0) {
            PyBuffer_Release(&view);
            Py_DECREF(data);
            break;
        }
        if (view.len > n - got) {
            /* Misbehaving stream; read more than asked. */
            view.len = n - got;
        }
        memcpy(dest + got, view.buf, (size_t)view.len);
        got += view.len;
        PyBuffer_Release(&view);
        Py_DECREF(data);
    }
    return got;
}


/* `stream.read(n)`, as a `bytes`, the same as the Python implementation
 * does. Returns a new reference. A `None` result becomes empty bytes. */
static PyObject *
streamRead(PyObject *stream, Py_ssize_t n)
{
    PyObject *data = streamReadOnce(stream, n);
    if (data == NULL)
        return NULL;
    if (data == Py_None) {
        Py_DECREF(data);
        return PyBytes_FromStringAndSize(NULL, 0);
    }
    if (!PyBytes_Check(data)) {
        PyObject *b = PyBytes_FromObject(data);
        Py_DECREF(data);
        return b;
    }
    return data;
}


/* ===========================================================================
 * Stream-based decoding functions (drop-in replacements for `decoding.py`)
 * ======================================================================== */

/* Raise the same exception `ord()` does when the pure-Python version reads
 * past the end of the stream. Note: `core.py` (and possibly other code)
 * checks for "ord()" in the message to detect EOF! */
static PyObject *
raiseEOF(void)
{
    PyErr_SetString(PyExc_TypeError,
                    "ord() expected a character, but string of length 0 found");
    return NULL;
}


static PyObject *
cReadElementID(PyObject *module, PyObject *stream)
{
    PyObject *data;
    unsigned char buf[4] = {0, 0, 0, 0};
    Py_ssize_t len, got, i;
    uint32_t eid = 0;
    unsigned char first;

    data = streamRead(stream, 1);
    if (data == NULL)
        return NULL;
    if (PyBytes_GET_SIZE(data) == 0) {
        Py_DECREF(data);
        return raiseEOF();
    }
    first = (unsigned char)PyBytes_AS_STRING(data)[0];
    Py_DECREF(data);

    len = vintLength(first);
    if (len > 4) {
        return PyErr_Format(PyExc_IOError, "Invalid length for ID: %zd", len);
    }

    if (len == 1)
        return Py_BuildValue("(kn)", (unsigned long)first, len);

    /* Like the Python version, a truncated ID is right-justified. */
    buf[0] = first;
    got = streamReadInto(stream, buf + 1, len - 1);
    if (got < 0)
        return NULL;
    got += 1;
    for (i = 0; i < got; i++)
        eid = (eid << 8) | buf[i];
    return Py_BuildValue("(kn)", (unsigned long)eid, len);
}


/* Decode an element size from (up to `avail` bytes of) `data`, whose
 * declared length is `len`. Mirrors `decoding.readElementSize()`, including
 * its handling of truncated data. Sets `*unknown` if the size is the EBML
 * 'unknown' size. */
static uint64_t
decodeSizeBytes(const unsigned char *data, Py_ssize_t len, Py_ssize_t avail,
                int *unknown)
{
    uint64_t size;
    Py_ssize_t i;

    size = data[0] & (0xFF >> len);
    if (avail > len)
        avail = len;
    for (i = 1; i < avail; i++)
        size = (size << 8) | data[i];

    *unknown = (size == (((uint64_t)1) << (7 * len)) - 1);
    return size;
}


static PyObject *
cReadElementSize(PyObject *module, PyObject *stream)
{
    PyObject *data;
    unsigned char buf[8];
    Py_ssize_t len, got;
    uint64_t size;
    int unknown;

    data = streamRead(stream, 1);
    if (data == NULL)
        return NULL;
    if (PyBytes_GET_SIZE(data) == 0) {
        Py_DECREF(data);
        return raiseEOF();
    }
    buf[0] = (unsigned char)PyBytes_AS_STRING(data)[0];
    Py_DECREF(data);

    len = vintLength(buf[0]);
    got = 1;
    if (len > 1) {
        got = streamReadInto(stream, buf + 1, len - 1);
        if (got < 0)
            return NULL;
        got += 1;
    }

    size = decodeSizeBytes(buf, len, got, &unknown);
    if (unknown)
        return Py_BuildValue("(On)", Py_None, len);
    return Py_BuildValue("(Kn)", (unsigned long long)size, len);
}


/* Parse the common `(stream, size)` arguments. */
static int
parseStreamSize(PyObject *const *args, Py_ssize_t nargs, const char *fname,
                Py_ssize_t defaultSize, PyObject **stream, Py_ssize_t *size)
{
    if (nargs < 1 || nargs > 2 || (nargs == 1 && defaultSize < 0)) {
        PyErr_Format(PyExc_TypeError, "%s() takes 2 positional arguments "
                     "but %zd were given", fname, nargs);
        return -1;
    }
    *stream = args[0];
    if (nargs == 1) {
        *size = defaultSize;
        return 0;
    }
    *size = PyLong_AsSsize_t(args[1]);
    if (*size == -1 && PyErr_Occurred())
        return -1;
    return 0;
}


static PyObject *
cReadUInt(PyObject *module, PyObject *const *args, Py_ssize_t nargs)
{
    PyObject *stream, *data, *result;
    Py_ssize_t size;

    if (parseStreamSize(args, nargs, "readUInt", -1, &stream, &size) < 0)
        return NULL;
    if (size == 0)
        return PyLong_FromLong(0);

    data = streamRead(stream, size);
    if (data == NULL)
        return NULL;
    result = PyLong_FromUnsignedLongLong(decodeUIntBytes(
        (const unsigned char *)PyBytes_AS_STRING(data),
        PyBytes_GET_SIZE(data)));
    Py_DECREF(data);
    return result;
}


static PyObject *
cReadInt(PyObject *module, PyObject *const *args, Py_ssize_t nargs)
{
    PyObject *stream, *data, *result;
    Py_ssize_t size;

    if (parseStreamSize(args, nargs, "readInt", -1, &stream, &size) < 0)
        return NULL;
    if (size == 0)
        return PyLong_FromLong(0);

    data = streamRead(stream, size);
    if (data == NULL)
        return NULL;
    if (PyBytes_GET_SIZE(data) == 0) {
        /* Same as Python indexing the first byte of empty data. */
        Py_DECREF(data);
        PyErr_SetString(PyExc_IndexError, "index out of range");
        return NULL;
    }
    result = PyLong_FromLongLong(decodeIntBytes(
        (const unsigned char *)PyBytes_AS_STRING(data),
        PyBytes_GET_SIZE(data)));
    Py_DECREF(data);
    return result;
}


static PyObject *
cReadFloat(PyObject *module, PyObject *const *args, Py_ssize_t nargs)
{
    PyObject *stream, *data, *result;
    Py_ssize_t size;

    if (parseStreamSize(args, nargs, "readFloat", -1, &stream, &size) < 0)
        return NULL;
    if (size == 0)
        return PyFloat_FromDouble(0.0);
    if (size != 4 && size != 8) {
        return PyErr_Format(PyExc_IOError,
                            "Cannot read floating point value of length %zd; "
                            "only lengths of 0, 4, or 8 bytes supported.",
                            size);
    }

    data = streamRead(stream, size);
    if (data == NULL)
        return NULL;
    if (PyBytes_GET_SIZE(data) != size) {
        Py_DECREF(data);
        return PyErr_Format(structError,
                            "unpack requires a buffer of %zd bytes", size);
    }
    result = PyFloat_FromDouble(decodeFloatBytes(
        (const unsigned char *)PyBytes_AS_STRING(data), size));
    Py_DECREF(data);
    return result;
}


static PyObject *
cReadString(PyObject *module, PyObject *const *args, Py_ssize_t nargs)
{
    PyObject *stream, *data, *result;
    Py_ssize_t size, len;
    const char *s;

    if (parseStreamSize(args, nargs, "readString", -1, &stream, &size) < 0)
        return NULL;
    if (size == 0)
        return PyUnicode_FromStringAndSize(NULL, 0);

    data = streamRead(stream, size);
    if (data == NULL)
        return NULL;
    s = PyBytes_AS_STRING(data);
    len = strLength((const unsigned char *)s, PyBytes_GET_SIZE(data));

    result = PyUnicode_DecodeASCII(s, len, NULL);
    if (result == NULL && PyErr_ExceptionMatches(PyExc_UnicodeDecodeError)) {
        /* Like the Python version: warn, then decode with replacement. */
        PyObject *msg;
#if PY_VERSION_HEX >= 0x030C0000
        PyObject *exc = PyErr_GetRaisedException();
#else
        PyObject *excType, *exc, *excTb;
        PyErr_Fetch(&excType, &exc, &excTb);
        PyErr_NormalizeException(&excType, &exc, &excTb);
        Py_XDECREF(excType);
        Py_XDECREF(excTb);
#endif
        msg = PyObject_Str(exc);
        Py_DECREF(exc);
        if (msg == NULL) {
            Py_DECREF(data);
            return NULL;
        }
        if (PyErr_WarnEx(PyExc_UnicodeWarning, PyUnicode_AsUTF8(msg), 1) < 0) {
            Py_DECREF(msg);
            Py_DECREF(data);
            return NULL;
        }
        Py_DECREF(msg);
        result = PyUnicode_DecodeASCII(s, len, "replace");
    }
    Py_DECREF(data);
    return result;
}


static PyObject *
cReadUnicode(PyObject *module, PyObject *const *args, Py_ssize_t nargs)
{
    PyObject *stream, *data, *result;
    Py_ssize_t size;
    const char *s;

    if (parseStreamSize(args, nargs, "readUnicode", -1, &stream, &size) < 0)
        return NULL;
    if (size == 0)
        return PyUnicode_FromStringAndSize(NULL, 0);

    data = streamRead(stream, size);
    if (data == NULL)
        return NULL;
    s = PyBytes_AS_STRING(data);
    result = PyUnicode_DecodeUTF8(
        s, strLength((const unsigned char *)s, PyBytes_GET_SIZE(data)), NULL);
    Py_DECREF(data);
    return result;
}


static PyObject *
cReadDate(PyObject *module, PyObject *const *args, Py_ssize_t nargs)
{
    PyObject *stream, *data, *result;
    Py_ssize_t size;

    if (parseStreamSize(args, nargs, "readDate", 8, &stream, &size) < 0)
        return NULL;
    if (size != 8) {
        return PyErr_Format(PyExc_IOError,
                            "Cannot read date value of length %zd, only 8.",
                            size);
    }

    data = streamRead(stream, size);
    if (data == NULL)
        return NULL;
    if (PyBytes_GET_SIZE(data) != 8) {
        Py_DECREF(data);
        return PyErr_Format(structError,
                            "unpack requires a buffer of 8 bytes");
    }
    result = makeDate(decodeIntBytes(
        (const unsigned char *)PyBytes_AS_STRING(data), 8));
    Py_DECREF(data);
    return result;
}


/* ===========================================================================
 * Batch scanning of elements
 * ======================================================================== */

/* A window of stream data, read in chunks. */
typedef struct {
    PyObject *stream;
    unsigned char *buf;
    Py_ssize_t capacity;
    long long start;        /* Stream position of `buf[0]` */
    Py_ssize_t len;         /* Number of valid bytes in `buf` */
    Py_ssize_t chunk;       /* Number of bytes to read on the next refill */
} Window;


/* Make sure the window contains `need` bytes starting at stream position
 * `pos`, refilling if it doesn't. A refill reads the window's current chunk
 * size, limited to `limit` bytes (e.g. the remainder of the parent element)
 * but at least `need`. On return, `*avail` is the number of bytes actually available
 * at `pos` (fewer than `need` only at EOF) and the result is a pointer to
 * them. Returns NULL on error. */
static const unsigned char *
windowGet(Window *w, long long pos, Py_ssize_t need, long long limit,
          Py_ssize_t *avail)
{
    Py_ssize_t got, want;

    if (pos >= w->start && pos + need <= w->start + w->len) {
        *avail = (Py_ssize_t)(w->start + w->len - pos);
        return w->buf + (pos - w->start);
    }

    want = w->chunk;
    if (limit >= 0 && limit < want)
        want = (Py_ssize_t)limit;
    if (want < need)
        want = need;
    if (w->chunk < SCAN_CHUNK_MAX)
        w->chunk *= 2;

    if (want > w->capacity) {
        unsigned char *newBuf = PyMem_Realloc(w->buf, (size_t)want);
        if (newBuf == NULL) {
            PyErr_NoMemory();
            return NULL;
        }
        w->buf = newBuf;
        w->capacity = want;
    }

    if (streamSeek(w->stream, pos) < 0)
        return NULL;
    got = streamReadInto(w->stream, w->buf, want);
    if (got < 0)
        return NULL;

    w->start = pos;
    w->len = got;
    *avail = got;
    return w->buf;
}


/* Add an entry tuple `(eid, offset, size, payloadOffset, value)` to the
 * list. Steals the reference to `value`. */
static int
appendEntry(PyObject *entries, unsigned long eid, long long offset,
            int unknown, uint64_t size, long long payloadOffset,
            PyObject *value)
{
    PyObject *entry;
    int r;

    if (unknown) {
        entry = Py_BuildValue("(kLOLN)", eid, offset, Py_None, payloadOffset,
                              value);
    }
    else {
        entry = Py_BuildValue("(kLKLN)", eid, offset, (unsigned long long)size,
                              payloadOffset, value);
    }
    if (entry == NULL)
        return -1;
    r = PyList_Append(entries, entry);
    Py_DECREF(entry);
    return r;
}


PyDoc_STRVAR(scanElements_doc,
"scanElements(stream, pos, end, typeMap, validIds, maxCount)\n"
"--\n\n"
"Read the headers of consecutive elements from a stream, starting at `pos`.\n"
"\n"
":param stream: The source file-like stream. Must have `seek()` and\n"
"    `read()` methods.\n"
":param pos: The stream offset of the first element.\n"
":param end: The offset at which to stop (e.g. the end of a parent\n"
"    element's payload), or `None` to read to the end of the stream.\n"
":param typeMap: A dictionary of element IDs to type codes; elements with\n"
"    a type code will have their values decoded. `None` to decode none.\n"
":param validIds: A container of element IDs; scanning stops at the first\n"
"    element whose ID is not in it. `None` to accept any ID.\n"
":param maxCount: The maximum number of elements to read.\n"
":return: A tuple containing a list of\n"
"    `(id, offset, size, payloadOffset, value)` tuples and a Boolean that\n"
"    is `True` if the end was reached (EOF, `end`, or an invalid ID). Scanning\n"
"    also stops after an element with an unknown (`None`) size. `value` is\n"
"    `None` if the value was not decoded.\n");

static PyObject *
cScanElements(PyObject *module, PyObject *const *args, Py_ssize_t nargs)
{
    PyObject *stream, *typeMap, *validIds, *entries;
    long long pos, end;
    int hasEnd, done = 0;
    Py_ssize_t maxCount;
    Window w;

    if (nargs != 6) {
        PyErr_Format(PyExc_TypeError, "scanElements() takes 6 positional "
                     "arguments but %zd were given", nargs);
        return NULL;
    }

    stream = args[0];
    pos = PyLong_AsLongLong(args[1]);
    if (pos == -1 && PyErr_Occurred())
        return NULL;
    hasEnd = (args[2] != Py_None);
    end = 0;
    if (hasEnd) {
        end = PyLong_AsLongLong(args[2]);
        if (end == -1 && PyErr_Occurred())
            return NULL;
    }
    typeMap = (args[3] == Py_None) ? NULL : args[3];
    validIds = (args[4] == Py_None) ? NULL : args[4];
    maxCount = PyLong_AsSsize_t(args[5]);
    if (maxCount == -1 && PyErr_Occurred())
        return NULL;

    if (typeMap != NULL && !PyDict_Check(typeMap)) {
        PyErr_SetString(PyExc_TypeError, "typeMap must be a dict or None");
        return NULL;
    }

    entries = PyList_New(0);
    if (entries == NULL)
        return NULL;

    w.stream = stream;
    w.buf = NULL;
    w.capacity = 0;
    w.start = -1;
    w.len = 0;
    w.chunk = SCAN_CHUNK_MIN;

    while (PyList_GET_SIZE(entries) < maxCount) {
        const unsigned char *p;
        Py_ssize_t avail, idLen, sizeLen, i;
        unsigned long eid;
        uint64_t size;
        int unknown, typeCode = TYPE_NONE;
        long long payloadOffset;
        PyObject *value = NULL;

        if (hasEnd && pos >= end) {
            done = 1;
            break;
        }

        /* Read no more than the parent needs, but at least a header. */
        p = windowGet(&w, pos, MAX_HEADER, hasEnd ? end - pos : -1, &avail);
        if (p == NULL)
            goto error;
        if (avail == 0) {
            /* EOF */
            done = 1;
            break;
        }

        /* Element ID */
        idLen = vintLength(p[0]);
        if (idLen > 4) {
            if (PyList_GET_SIZE(entries) > 0) {
                /* Return the good elements first; the error will be raised
                 * by the next call (starting at this element). */
                break;
            }
            PyErr_Format(PyExc_IOError, "Invalid length for ID: %zd", idLen);
            goto error;
        }
        if (avail <= idLen) {
            /* Truncated header. The pure-Python version hits EOF reading
             * the size. */
            done = 1;
            break;
        }
        eid = 0;
        for (i = 0; i < idLen; i++)
            eid = (eid << 8) | p[i];

        if (validIds != NULL) {
            PyObject *key = PyLong_FromUnsignedLong(eid);
            int valid;
            if (key == NULL)
                goto error;
            valid = PySequence_Contains(validIds, key);
            Py_DECREF(key);
            if (valid < 0)
                goto error;
            if (!valid) {
                done = 1;
                break;
            }
        }

        /* Element size. As in the Python version, a size truncated by EOF
         * is decoded from whatever bytes are available. */
        sizeLen = vintLength(p[idLen]);
        size = decodeSizeBytes(p + idLen, sizeLen, avail - idLen, &unknown);
        payloadOffset = pos + idLen + sizeLen;

        /* Value */
        if (typeMap != NULL && !unknown) {
            PyObject *key = PyLong_FromUnsignedLong(eid);
            PyObject *code;
            if (key == NULL)
                goto error;
            code = PyDict_GetItemWithError(typeMap, key);
            Py_DECREF(key);
            if (code == NULL && PyErr_Occurred())
                goto error;
            if (code != NULL) {
                typeCode = (int)PyLong_AsLong(code);
                if (typeCode == -1 && PyErr_Occurred())
                    goto error;
            }
        }

        if (typeCode != TYPE_NONE && size <= MAX_SCAN_PAYLOAD) {
            Py_ssize_t payloadSize = (Py_ssize_t)size;
            if (payloadSize == 0) {
                value = decodePayload(typeCode, (const unsigned char *)"", 0);
            }
            else {
                p = windowGet(&w, payloadOffset, payloadSize,
                              hasEnd ? end - payloadOffset : -1, &avail);
                if (p == NULL)
                    goto error;
                if (avail >= payloadSize) {
                    value = decodePayload(typeCode, p, payloadSize);
                }
                /* Otherwise truncated; the Python version will handle it. */
            }
        }

        if (value == NULL) {
            value = Py_None;
            Py_INCREF(value);
        }
        if (appendEntry(entries, eid, pos, unknown, size, payloadOffset,
                        value) < 0)
            goto error;

        if (unknown) {
            /* The end of an unknown-size element can only be determined by
             * the caller (parsing its children). */
            break;
        }
        pos = payloadOffset + (long long)size;
    }

    PyMem_Free(w.buf);
    return Py_BuildValue("(NO)", entries, done ? Py_True : Py_False);

error:
    PyMem_Free(w.buf);
    Py_DECREF(entries);
    return NULL;
}


/* ===========================================================================
 * Module definition
 * ======================================================================== */

static PyMethodDef cebmlMethods[] = {
    {"readElementID", (PyCFunction)cReadElementID, METH_O,
     "Read an element ID from a file (or file-like stream)."},
    {"readElementSize", (PyCFunction)cReadElementSize, METH_O,
     "Read an element size from a file (or file-like stream)."},
    {"readUInt", (PyCFunction)(void(*)(void))cReadUInt, METH_FASTCALL,
     "Read an unsigned integer from a file (or file-like stream)."},
    {"readInt", (PyCFunction)(void(*)(void))cReadInt, METH_FASTCALL,
     "Read a signed integer from a file (or file-like stream)."},
    {"readFloat", (PyCFunction)(void(*)(void))cReadFloat, METH_FASTCALL,
     "Read a floating point value from a file (or file-like stream)."},
    {"readString", (PyCFunction)(void(*)(void))cReadString, METH_FASTCALL,
     "Read an ASCII string from a file (or file-like stream)."},
    {"readUnicode", (PyCFunction)(void(*)(void))cReadUnicode, METH_FASTCALL,
     "Read a UTF-8 encoded string from a file (or file-like stream)."},
    {"readDate", (PyCFunction)(void(*)(void))cReadDate, METH_FASTCALL,
     "Read an EBML encoded date from a file (or file-like stream)."},
    {"scanElements", (PyCFunction)(void(*)(void))cScanElements, METH_FASTCALL,
     scanElements_doc},
    {NULL, NULL, 0, NULL}
};


static struct PyModuleDef cebmlModule = {
    PyModuleDef_HEAD_INIT,
    "_cebml",
    "Native accelerator for ebmlite reading and decoding.",
    -1,
    cebmlMethods
};


PyMODINIT_FUNC
PyInit__cebml(void)
{
    PyObject *m, *structModule;

    PyDateTime_IMPORT;
    if (PyDateTimeAPI == NULL)
        return NULL;

    strRead = PyUnicode_InternFromString("read");
    strSeek = PyUnicode_InternFromString("seek");
    if (strRead == NULL || strSeek == NULL)
        return NULL;

    dateEpoch = PyDateTime_FromDateAndTime(2001, 1, 1, 0, 0, 0, 0);
    if (dateEpoch == NULL)
        return NULL;

    structModule = PyImport_ImportModule("struct");
    if (structModule == NULL)
        return NULL;
    structError = PyObject_GetAttrString(structModule, "error");
    Py_DECREF(structModule);
    if (structError == NULL)
        return NULL;

    m = PyModule_Create(&cebmlModule);
    if (m == NULL)
        return NULL;

    if (PyModule_AddIntConstant(m, "TYPE_INT", TYPE_INT) < 0
            || PyModule_AddIntConstant(m, "TYPE_UINT", TYPE_UINT) < 0
            || PyModule_AddIntConstant(m, "TYPE_FLOAT", TYPE_FLOAT) < 0
            || PyModule_AddIntConstant(m, "TYPE_STRING", TYPE_STRING) < 0
            || PyModule_AddIntConstant(m, "TYPE_UNICODE", TYPE_UNICODE) < 0
            || PyModule_AddIntConstant(m, "TYPE_DATE", TYPE_DATE) < 0
            || PyModule_AddIntConstant(m, "TYPE_BINARY", TYPE_BINARY) < 0) {
        Py_DECREF(m);
        return NULL;
    }

    return m;
}
