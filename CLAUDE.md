# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project

_ebmlite_ is a lightweight, pure-Python (no runtime dependencies) library for parsing — and doing basic encoding of — EBML (Extensible Binary Markup Language) data. Supports Python 3.9–3.14. Version is defined in `ebmlite/__init__.py` (`__version__`) and read by `setup.py`.

## Commands

```bash
pip install -e .[test]                                  # install with test deps
python -m pytest ./tests/                               # run all tests
python -m pytest ./tests/ --cov=ebmlite -n auto         # as CI runs it (xdist + coverage)
python -m pytest tests/test_core.py::test_name          # single test
flake8 ebmlite                                          # lint (setup.cfg: only pyflakes `F` checks; tests/ excluded)
```

- Run tests from the repo root: tests reference data files via relative paths like `./tests/video-4.ebml` and `./ebmlite/schemata/matroska.xml`.
- `tests/test_tools.py` command-line tests silently return early unless `GITHUB_ACTIONS=true`, since they require the package's console scripts to be installed.
- CI (`.github/workflows/unit-tests.yml`) runs on Windows/Linux/macOS across all supported Python versions. Pushing to `master` publishes to PyPI (`publish-to-pypi.yml`); day-to-day work happens on `development`.

## Code style

`pyproject.toml` configures pylint naming conventions: **camelCase** for functions, methods, arguments, attributes and variables; PascalCase classes; UPPER_CASE constants; snake_case modules. Follow this (not PEP 8 snake_case) when adding code.

## Architecture

- **`core.py`** — the heart of the library.
  - `Element` and its typed subclasses (`IntegerElement`, `UIntegerElement`, `FloatElement`, `StringElement`, `UnicodeElement`, `DateElement`, `BinaryElement`, `MasterElement`), plus the special-case `VoidElement` and `UnknownElement`. Elements use `__slots__` and hold a stream reference + offset; values are lazy-loaded via `parse()` and cached in `_value` unless the class's `precache` flag causes them to be read at parse time.
  - `Schema` parses a schema XML file and **dynamically creates an `Element` subclass for every element defined** (via `addElement`, mapping XML tag names through `Schema.ELEMENT_TYPES`), along with a schema-specific `Document` subclass. Decoding always goes through a schema: `schema.load(file)` → `Document`. Schema structure (parent/child) is used for decoding only and isn't strictly enforced. An element may appear multiple times in a schema XML; later occurrences must not contradict the first definition. Legacy python-ebml schemata (root `<table>`) are also accepted.
  - `Document` is a `MasterElement` subclass representing the file root; multiple root-level elements are allowed (contrary to the official spec).
  - `loadSchema()` caches schemata in the module-level `SCHEMATA` dict and resolves bare filenames through `SCHEMA_PATH` (a list, like `sys.path`), which includes the bundled `ebmlite/schemata/` directory and paths from the `EBMLITE_SCHEMA_PATH` env var. Paths may be module-relative using braces, e.g. `"{idelib}/schemata/mide_ide.xml"`.
- **`decoding.py` / `encoding.py`** — low-level, stream-oriented functions for reading/writing EBML variable-length IDs, sizes and typed payloads. Element classes delegate to these.
- **`_cebml.c`** — optional C extension (built with `optional=True`, so installs fall back to pure Python). Provides drop-in replacements for the `decoding.py` stream readers, plus `scanElements()`, which batch-reads element headers (and simple precached values, per `Schema._getScanTypes()`) and is used by `MasterElement._scanElements()` for `MasterElement`/`Document` iteration and unknown-size computation. It only uses the stream's `seek()`/`read()` and keeps no state between calls. The pure-Python code is the reference: behavior (including exceptions on bad/truncated data) must match it, which `tests/test_cebml.py` checks differentially. `EBMLITE_NO_C=1` forces pure Python; `decoding.PY_IMPLEMENTATIONS` holds the Python readers. After editing the C, rebuild with `pip install -e .` (or `python setup.py build_ext --inplace`) and run the tests both with and without `EBMLITE_NO_C=1`.
- **`util.py`** — higher-level helpers: EBML↔XML conversion (`toXml`, `xml2ebml`, `xmlElement2ebml`, `loadXml`), `pprint`, `flatiter`, ID creation/validation, schema listing.
- **`xml_codecs.py`** — pluggable codecs (`base64`, `hex`, `ignore`; registry `BINARY_CODECS`) for rendering binary payloads in XML output.
- **`threaded_file.py`** — `ThreadAwareFile`, a read-only file replacement giving each thread its own underlying stream so concurrent seeks don't interfere.
- **`tools/`** — CLI entry points registered in `setup.py`: `view-ebml`, `ebml2xml`, `xml2ebml`, `list-schemata`.
- **`schemata/`** — bundled schema XML files (Matroska and Midé formats), shipped as package data.

The README documents the ebmlite schema XML syntax and element attributes (`id`, `global`, `length`, `multiple`, `mandatory`, `precache`, etc.) in detail.
