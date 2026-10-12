"""
Tests for the optional native C extension (`ebmlite._cebml`). These compare
its results against the pure-Python reference implementations, which are
tested in their own right elsewhere. Skipped if the extension isn't built
(or is disabled with the `EBMLITE_NO_C` environment variable).
"""

from contextlib import contextmanager
from io import BytesIO
import struct
import threading
import warnings

import pytest

from ebmlite import core, decoding
from ebmlite.threaded_file import ThreadAwareFile

pytestmark = pytest.mark.skipif(decoding._cebml is None,
                                reason="C extension not available")

PY = decoding.PY_IMPLEMENTATIONS

FILES = [('./tests/SSX46714-doesnot.IDE', 'mide_ide.xml'),
         ('./tests/video-1.mkv', 'matroska.xml'),
         ('./tests/video-2.mkv', 'matroska.xml'),
         ('./tests/video-3.mkv', 'matroska.xml'),
         ('./tests/video-4.ebml', 'matroska.xml')]


# ==============================================================================
# Helpers
# ==============================================================================

def outcome(func, *args):
    """ Call a function, returning its result or the type of exception it
        raised, plus the categories of any warnings issued.
    """
    with warnings.catch_warnings(record=True) as w:
        warnings.simplefilter('always')
        try:
            result = func(*args)
        except Exception as err:
            result = type(err)
    return result, [x.category for x in w]


def compareStreamFunc(name, data, *args):
    """ Compare a C and a Python stream decoding function, including the
        resulting stream position.
    """
    pyStream, cStream = BytesIO(data), BytesIO(data)
    expected = outcome(PY[name], pyStream, *args)
    actual = outcome(getattr(decoding._cebml, name), cStream, *args)
    assert actual == expected, "%s(%r, %r)" % (name, data, args)
    assert cStream.tell() == pyStream.tell(), "%s(%r, %r)" % (name, data, args)


@contextmanager
def purePython():
    """ Context manager to make `core` use only the pure-Python code. """
    saved = {name: getattr(core, name) for name in PY}
    savedC = core._cebml
    try:
        core._cebml = None
        for name, func in PY.items():
            setattr(core, name, func)
        yield
    finally:
        core._cebml = savedC
        for name, func in saved.items():
            setattr(core, name, func)


def flatten(el, result=None):
    """ Recursively crawl an element, producing a list of tuples describing
        each child. The crawl stops at the first exception, which is
        recorded at the end of the list.
    """
    result = [] if result is None else result
    try:
        for ch in el:
            if isinstance(ch, core.MasterElement):
                result.append((type(ch).__name__, ch.id, ch.offset,
                               ch.payloadOffset, ch.size, len(ch)))
                flatten(ch, result)
            else:
                result.append((type(ch).__name__, ch.id, ch.offset,
                               ch.payloadOffset, ch.size, ch._value))
    except Exception as err:
        result.append(type(err))
        raise
    return result


def crawl(schema, stream):
    """ Load and flatten a document. """
    with warnings.catch_warnings():
        warnings.simplefilter('ignore')
        result = []
        doc = schema.load(stream)
        try:
            flatten(doc, result)
        except Exception:
            pass
        return result


def crawlBoth(schema, data, streamType=BytesIO):
    """ Load and flatten a document using C and then pure Python. """
    actual = crawl(schema, streamType(data))
    with purePython():
        expected = crawl(schema, BytesIO(data))
    return actual, expected


class ShortReadStream:
    """ A minimal file-like stream that returns at most a few bytes per
        `read()` (like a pipe or socket might), as a `bytearray`, and has
        no `readinto()`, `getvalue()`, `fileno()`, or `name`.
    """
    def __init__(self, data, maxRead=3):
        self._data = bytes(data)
        self._pos = 0
        self.maxRead = maxRead

    def read(self, n=-1):
        if n is None or n < 0:
            n = len(self._data) - self._pos
        n = min(n, self.maxRead)
        result = bytearray(self._data[self._pos:self._pos + n])
        self._pos += len(result)
        return result

    def seek(self, pos, whence=0):
        if whence == 1:
            pos += self._pos
        elif whence == 2:
            pos += len(self._data)
        self._pos = pos
        return pos

    def tell(self):
        return self._pos


# ==============================================================================
# Stream decoding functions
# ==============================================================================

def testReadElementID():
    for first in range(256):
        data = bytes([first, 0x42, 0x86, 0x81, 0x01])
        compareStreamFunc('readElementID', data)
        # Truncated
        for n in range(1, 4):
            compareStreamFunc('readElementID', data[:n])
    compareStreamFunc('readElementID', b'')


def testReadElementSize():
    for first in range(256):
        for rest in (b'\x00' * 7, b'\xff' * 7, b'\x12\x34\x56\x78\x9a\xbc\xde'):
            data = bytes([first]) + rest
            compareStreamFunc('readElementSize', data)
            # Truncated
            for n in range(1, 8):
                compareStreamFunc('readElementSize', data[:n])
    compareStreamFunc('readElementSize', b'')


@pytest.mark.parametrize('name', ['readUInt', 'readInt'])
def testReadIntegers(name):
    samples = (b'\x00' * 10, b'\xff' * 10, b'\x80' + b'\x00' * 9,
               b'\x7f' + b'\xff' * 9, bytes(range(1, 11)))
    for data in samples:
        for size in range(0, 11):
            compareStreamFunc(name, data, size)
            # Truncated
            compareStreamFunc(name, data[:size // 2], size)
    compareStreamFunc(name, b'', 4)


def testReadFloat():
    samples = [struct.pack('>f', v) for v in (0.0, -1.5, 3.25e10)]
    samples += [struct.pack('>d', v) for v in (0.0, -1.5, 3.25e100)]
    for data in samples:
        for size in (0, 4, 8, 3, 10):
            compareStreamFunc('readFloat', data, size)
        compareStreamFunc('readFloat', data[:3], len(data))


def testReadFloatNaN():
    data = b'\x7f\xf8' + b'\x00' * 6
    value = decoding._cebml.readFloat(BytesIO(data), 8)
    assert value != value


@pytest.mark.parametrize('name', ['readString', 'readUnicode'])
def testReadStrings(name):
    samples = (b'', b'hello', b'hello\x00world', b'\x00hidden',
               'h\xe9llo'.encode('utf8'), 'h\xe9llo'.encode('latin-1'),
               b'\xff\xfe', b'abc\x00\xff')
    for data in samples:
        for size in (0, 1, len(data), len(data) + 5):
            compareStreamFunc(name, data, size)


def testReadDate():
    samples = [struct.pack('>q', v) for v in
               (0, 1, 999, 1000, -1, -999, -1000, -1001,
                86400 * 10**9 * 365, -86400 * 10**9 * 365 - 1,
                2**63 - 1, -2**63)]
    for data in samples:
        compareStreamFunc('readDate', data, 8)
        compareStreamFunc('readDate', data)
        compareStreamFunc('readDate', data, 4)
        compareStreamFunc('readDate', data[:5], 8)


def testDecodingModuleUsesC():
    """ Test that the C functions replaced the Python ones. """
    for name in PY:
        assert getattr(decoding, name) is getattr(decoding._cebml, name)
        assert getattr(core, name) is getattr(decoding._cebml, name)


# ==============================================================================
# Scanning/iteration
# ==============================================================================

@pytest.mark.parametrize('filename, schemaName', FILES)
def testFiles(filename, schemaName):
    """ Test iterating real files, as both a file and BytesIO. """
    schema = core.loadSchema(schemaName)
    with open(filename, 'rb') as f:
        data = f.read()

    with purePython():
        expected = crawl(schema, BytesIO(data))

    assert crawl(schema, BytesIO(data)) == expected
    with open(filename, 'rb') as f:
        assert crawl(schema, f) == expected

    # Note: not used as a context manager, since `ThreadAwareFile.__enter__()`
    # returns the current thread's underlying stream.
    f = ThreadAwareFile(filename, 'rb')
    try:
        assert crawl(schema, f) == expected
    finally:
        f.closeAll()


@pytest.mark.parametrize('maxRead', [1, 3, 1000])
def testShortReads(maxRead):
    """ Test iterating a stream that returns short reads, and isn't a real
        file.
    """
    filename, schemaName = FILES[0]
    schema = core.loadSchema(schemaName)
    with open(filename, 'rb') as f:
        data = f.read()

    actual, expected = crawlBoth(schema, data,
                                 lambda d: ShortReadStream(d, maxRead))
    assert actual == expected


@pytest.mark.parametrize('filename, schemaName', FILES[:2])
def testTruncated(filename, schemaName):
    """ Test iterating truncated data, including at points within element
        headers and payloads.
    """
    schema = core.loadSchema(schemaName)
    with open(filename, 'rb') as f:
        data = f.read(16384)

    for end in list(range(0, 300)) + list(range(300, len(data), 97)):
        actual, expected = crawlBoth(schema, data[:end])
        assert actual == expected, "truncated at %d" % end


@pytest.mark.parametrize('filename, schemaName', FILES[:2])
def testCorrupted(filename, schemaName):
    """ Test iterating data with bad bytes, e.g. invalid element IDs. """
    schema = core.loadSchema(schemaName)
    with open(filename, 'rb') as f:
        data = f.read(8192)

    for pos in range(0, 400, 3):
        for bad in (0x00, 0xFF, 0x01):
            corrupt = bytearray(data)
            corrupt[pos] = bad
            actual, expected = crawlBoth(schema, bytes(corrupt))
            assert actual == expected, "byte %d set to 0x%02x" % (pos, bad)


def testUnknownSize():
    """ Test elements with an unknown ('infinite') size, both as the last
        thing in the stream and followed by an element that isn't a valid
        child.
    """
    schema = core.loadSchema('matroska.xml')
    cluster = schema['Cluster'].encode({'Timecode': 1234,
                                        'SimpleBlock': b'blockdata'},
                                       infinite=True)
    segment = schema['Segment'].encode({'Cluster': {'Timecode': 1},
                                        'Void': b''}, infinite=True)
    header = schema['EBMLMaxIDLength'].encode(4)
    samples = (cluster,
               cluster + cluster,
               header + cluster + header,
               segment,
               segment[:-1] + cluster,
               cluster[:-3])

    for data in samples:
        actual, expected = crawlBoth(schema, data)
        assert actual == expected
        assert len(actual) > 1

        # Compute size before iterating, the other way around from crawl()
        idx = 1 if data.startswith(header) else 0
        el = schema.load(BytesIO(data))[idx]
        with purePython():
            expectedEl = schema.load(BytesIO(data))[idx]
            expectedSize = expectedEl.size
            expectedLen = len(expectedEl)
        assert el.size == expectedSize
        assert len(el) == expectedLen


def testUnknownSizeCustomValidity():
    """ Test that an overridden `_isValidChild()` is used when determining
        the size of an unknown-size element.
    """
    schema = core.Schema('./ebmlite/schemata/matroska.xml')
    schema['Cluster']._isValidChild = classmethod(
        lambda cls, elId: elId == schema['Timecode'].id)
    data = schema['Cluster'].encode({'Timecode': 1, 'SimpleBlock': b'x'},
                                    infinite=True)
    cluster = schema.load(BytesIO(data))[0]
    assert len(cluster) == 1
    assert cluster.size == len(schema['Timecode'].encode(1))


def testUnknownSizeNonMaster():
    """ Test that a non-master element with an unknown size raises the same
        error with and without the C extension.
    """
    schema = core.loadSchema('matroska.xml')
    data = schema['Timecode'].encode(1) + b'\xe7\xff' + b'\x00' * 4
    actual, expected = crawlBoth(schema, data)
    assert actual == expected
    assert actual[-1] is TypeError


def testUnknownElements():
    """ Test element IDs that aren't in the schema. """
    schema = core.loadSchema('matroska.xml')
    data = (b'\x4f\xff\x82\x01\x02'
            + schema['Timecode'].encode(1)
            + b'\x80\x81\x00')
    actual, expected = crawlBoth(schema, data)
    assert actual == expected
    assert actual[0][0] == 'UnknownElement'


def precachingSchema():
    """ Create a new Matroska schema, with strings precached. """
    schema = core.Schema('./ebmlite/schemata/matroska.xml')
    schema['Title'].precache = True
    schema['CodecID'].precache = True
    return schema


def testLargePayloads():
    """ Test precached values larger than the C extension decodes itself,
        and data spanning multiple reads.
    """
    schema = precachingSchema()
    data = b''.join(schema['Title'].encode('x' * n)
                    for n in (10, 70000, 10, 200000, 10, 5000, 10))
    data += schema['Timecode'].encode(42) * 10000

    actual, expected = crawlBoth(schema, data)
    assert actual == expected
    assert actual[1][-1] == 'x' * 70000


def testValueTypes():
    """ Test that the values decoded by the C extension are correct. """
    schema = precachingSchema()
    data = b''.join((schema['Title'].encode('h\xe9llo'),
                     schema['CodecID'].encode('V_TEST'),
                     schema['DateUTC'].encode(core.datetime(2020, 2, 3, 4, 5, 6, 7)),
                     schema['Duration'].encode(1.5, length=4),
                     schema['Duration'].encode(-2.25, length=8),
                     schema['TrackOffset'].encode(-12345),
                     schema['Timecode'].encode(2**64 - 1),
                     schema['Timecode'].encode(0)))
    actual, expected = crawlBoth(schema, data)
    assert actual == expected
    assert [x[-1] for x in actual] == ['h\xe9llo', 'V_TEST',
                                       core.datetime(2020, 2, 3, 4, 5, 6, 7),
                                       1.5, -2.25, -12345, 2**64 - 1, 0]


def testBadValues():
    """ Test values that the C extension leaves for Python to decode (and
        report errors/warnings about).
    """
    schema = precachingSchema()
    for data in (b'\x44\x89\x83\x00\x00\x00',            # 3 byte float
                 b'\x44\x61\x84\x00\x00\x00\x00',        # 4 byte date
                 b'\x86\x83\xff\xfe\x00',                # Bad ASCII
                 b'\x7b\xa9\x82\xff\xfe',                # Bad UTF-8
                 b'\xe7\x89' + b'\x01' * 9):             # 9 byte uint
        actual, expected = crawlBoth(schema, data)
        assert actual == expected

    with warnings.catch_warnings(record=True) as w:
        warnings.simplefilter('always')
        el = schema.load(BytesIO(b'\x86\x83\xff\xfe\x00'))[0]
        assert el.value == '��'
        assert w and w[0].category is UnicodeWarning


def testNocache():
    """ Test that `nocache` iteration doesn't cache values. """
    schema = core.loadSchema('matroska.xml')
    data = schema['Timecode'].encode(1) * 3
    doc = schema.load(BytesIO(data))
    assert all(el._value is None for el in doc.__iter__(nocache=True))
    assert all(el._value == 1 for el in doc)


def testCustomParse():
    """ Test that elements with overridden `parse()` methods or changed
        `precache` attributes are respected.
    """
    schema = core.Schema('./ebmlite/schemata/matroska.xml')
    schema['Timecode'].parse = lambda self, stream, size: 'custom'
    data = schema['Timecode'].encode(1) + schema['Duration'].encode(1.5)

    doc = schema.load(BytesIO(data))
    assert [el.value for el in doc] == ['custom', 1.5]

    schema['Duration'].precache = False
    assert [el._value for el in doc] == ['custom', None]


def testThreads():
    """ Test simultaneous iteration of the same `ThreadAwareFile` by
        multiple threads.
    """
    filename, schemaName = FILES[2]
    schema = core.loadSchema(schemaName)
    with open(filename, 'rb') as f:
        expected = crawl(schema, f)

    results = []
    f = ThreadAwareFile(filename, 'rb')
    try:
        doc = schema.load(f)

        def run():
            results.append(flatten(doc))

        threads = [threading.Thread(target=run) for _ in range(6)]
        for t in threads:
            t.start()
        for t in threads:
            t.join()
    finally:
        f.closeAll()

    assert len(results) == len(threads)
    for r in results:
        assert r == expected


def testScanElementsArgs():
    """ Test argument checking of the low-level scanning function. """
    scan = decoding._cebml.scanElements
    with pytest.raises(TypeError):
        scan(BytesIO(b''), 0, None, None, None)
    with pytest.raises(TypeError):
        scan(BytesIO(b''), 0, None, [], None, 10)
    with pytest.raises(AttributeError):
        scan(None, 0, None, None, None, 10)
    assert scan(BytesIO(b''), 0, None, None, None, 10) == ([], True)
    assert scan(BytesIO(b'\x80\x81\x00'), 0, None, None, None, 10) == \
        ([(0x80, 0, 1, 2, None)], True)
