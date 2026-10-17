# Experimental XFA viewing, filling and saving

This branch adds PDFium-backed XFA support to Okular's PDF generator. Ordinary
PDFs continue to use Poppler. With `ENABLE_XFA=ON`, successfully loaded XFA forms
use PDFium's layout and renderer, including dynamic forms with a different page
tree from their fallback PDF.

The main viewer supports viewing, zooming, tiled rendering and raster printing.
**Form filling is available in a separate editor:** choose **Tools → Fill XFA
Form…**. The original read-only banner has been replaced with instructions for
opening that editor. The editor works on an independent document instance.

## Fill a form

1. Open the XFA PDF, then choose **Tools → Fill XFA Form…**.
2. Click a text field and type. You can also edit its complete value in the
   **Selected text field** box below the page. That box provides normal Qt
   cursor movement, selection, clipboard operations, undo/redo and IME input.
   Click radio buttons, checkboxes and dropdowns on the page itself. Tab and
   Shift+Tab are passed to PDFium for form navigation.
3. Use **Previous Page / Next Page** and the zoom controls as needed.
4. Choose **Save As…** to write a filled PDF under a different filename. Closing
   with unsaved changes offers Save, Discard and Cancel. After closing the
   editor, Okular opens the saved copy, if any. Click a field again to continue
   editing after a save.

Save As preserves XFA rather than flattening it. The editor refuses the original
source filename, checks that the output can be reopened and writes atomically.
Printing to PDF, in contrast, produces a rasterized copy rather than an editable
form. The main viewer's normal form widgets and Save Changes remain disabled
for XFA; use the separate editor to fill and save.

## Dependencies and build

The feature is **off by default**. The full application still requires Okular's
Qt 6, KDE Frameworks 6 and Poppler development dependencies. XFA additionally
requires **qpdf** (headers, library and CMake config) and an XFA-enabled PDFium.
On Arch Linux, install the additional qpdf dependency with:

```sh
sudo pacman -S --needed qpdf
```

The official Arch `qpdf` package includes its headers and
`/usr/lib/cmake/qpdf/qpdfConfig.cmake`; no separate development package is needed.
See the [package file list](https://archlinux.org/packages/extra/x86_64/qpdf/files/).

Use the **self-initializing V8/XFA-enabled shared-library distribution** from
[bblanchon/pdfium-binaries](https://github.com/bblanchon/pdfium-binaries), such as
`pdfium-v8-linux-x64.tgz`. This update was tested with `chromium/8100`
(`157.0.8100.0`) and `chromium/7749` (`148.0.7749.0`). Keep the distribution's
headers, library and `PDFiumConfig.cmake` together. `PDFium_DIR` must point to
the directory containing that config, **not** to `libpdfium.so`. Do not use the
plain, non-V8 asset.

Stock upstream PDFium requires the host to initialize V8, its platform and cppgc
separately. This adapter does not implement that initialization. The supported
binary distribution does it inside `FPDF_InitLibrary()`. PDFium and its bundled
third-party licenses remain separate dependencies; binaries are not vendored.

From the updated Okular source root (the directory containing the top-level
`CMakeLists.txt`), configure and rebuild. For the Arch installation path used
in the earlier build:

```sh
cmake -S . -B build -DENABLE_XFA=ON \
  -DPDFium_DIR=/usr/lib/cmake/PDFium \
  -DCMAKE_INSTALL_PREFIX="$PWD/install"
cmake --build build --parallel 2
cmake --install build
source build/prefix.sh
./install/bin/okular /path/to/form.pdf
```

Adjust `--parallel 2` to the number of build jobs your machine can handle.
After pulling source updates, repeat the build and install commands and restart
Okular. The local `build/` and `install/` directories are ignored by Git.

For an unpacked PDFium distribution, replace `PDFium_DIR` with its config
directory and make its shared library discoverable at runtime if necessary:

```sh
export LD_LIBRARY_PATH="/absolute/path/to/pdfium/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
```

If the XFA compile/link check was cached from an earlier failed configuration,
add `-U PDFIUM_HAS_XFA` to the configure command. A failed check can indicate
missing symbols, headers or linker dependencies; inspect the CMake configure
log rather than assuming the library lacks XFA. If documentation is the only
missing dependency, install `kdoctools` on Arch or configure with
`-DFORCE_NOT_REQUIRED_DEPENDENCIES=KF6DocTools` to omit it.

With `ENABLE_XFA=OFF`, neither PDFium nor qpdf is required.

## Standalone editor and tests

The same editor can be built and run independently of KDE. This is also useful
for trying form filling without rebuilding the entire application. It requires
Qt 6 Core, Gui, Widgets, Xml, PrintSupport and Test development components:

```sh
cmake -S generators/poppler/xfa -B build-xfa -DENABLE_XFA=ON \
  -DPDFium_DIR=/usr/lib/cmake/PDFium
cmake --build build-xfa --parallel
./build-xfa/xfa-editor /path/to/form.pdf
ctest --test-dir build-xfa --output-on-failure
```

The standalone command currently opens unencrypted forms only. The Okular menu
editor reuses the password supplied when opening the document.

For the supplied sample regression, point the test at your own copy (it is not
included in this repository):

```sh
OKULAR_XFA_TEST_PDF=/absolute/path/to/sample.pdf \
  ctest --test-dir build-xfa --output-on-failure
```

Without that variable, only the two sample-specific checks are skipped; the
self-contained rendering, editing, persistence and UI tests still run. Test
fixtures deliberately use a different black fallback PDF page so a fallback
render cannot satisfy XFA pixel checks. The sample test edits only temporary
copies and checks that the source bytes remain unchanged.

The full application also registers `xfaGeneratorTest`. After building and
installing, use the build's `prefix.sh` environment and run
`ctest --test-dir build -R xfa`. It checks backend selection, read-only main
viewer permissions, fallback behavior, ordinary PDF reopening and printing.

## Save implementation and current limits

PDFium's foreground/static XFA editing updates AcroForm values and appearances,
but does not reliably update the separate XFA datasets packet. The save path
therefore uses qpdf to synchronize changed fields into that packet. It follows
hierarchical field names and occurrence-based bindings, including transparent
area containers, repeated field names and radio/checkbox export values. It also
removes the single serialization newline that PDFium inserts at the beginning
of scalar dataset values, while retaining user whitespace.

Some foreground XFA PDFs also contain ordinary PDF text widgets added later
by another editor. Unambiguous top-level text widgets with no matching XFA
name retain their PDF values and appearances without creating XFA bindings.
This includes the added date fields in the supplied sample. Names that could
be XFA paths or collide with template names still require supported bindings.

Saving currently requires an **XFA packet array**. Single-stream XDP documents
can be viewed, but Save As reports that saving them is unsupported. Foreground
synchronization supports the default occurrence binding and transparent layout
containers. Edited explicit/global/reference bindings, ambiguous radio states,
structured data targets, sparse form-state structures and multi-selection values
are rejected with an error.
No output is written when synchronization cannot be established safely.
Complex repeaters and form-specific scripts need
further compatibility work. This is experimental support, not full Adobe
Reader compatibility or a form-design editor.

Text search/selection/export, annotations, PDF outlines/layers/links and digital
signing remain unavailable in the XFA path. Form scripts run in PDFium's V8
runtime for layout and interaction. Host callbacks provide widget timers but
not network access, arbitrary local-file access, email, submission, navigation,
script dialogs or script-triggered printing. This is an in-process engine.

The PDFium runtime is initialized once per process. All XFA operations run on
the application thread; large or script-heavy forms can block the UI. The
ordinary Poppler path restores its normal capabilities when XFA is closed.
Fonts and form compatibility depend on PDFium and the installed fonts.

## Validation

The standalone backend, save bridge and Qt editor were compiled with
`-Wall -Wextra -Werror` using Qt 6.4.2 and qpdf 11.9.0 on Linux. Tests cover
rendering/printing, invalid inputs, thread confinement, mouse and keyboard
editing, Chinese IME commits, cursor/clipboard behavior, save/reopen, repeated
fields, radio export mapping, unsupported bindings, user whitespace, failed
saves, protection of the original file and cancel/discard on close.

The supplied three-page foreground XFA form was edited, saved and reopened.
Independent pypdf/lxml inspection confirmed valid XML and matching passport,
Chinese text, repeated text-field, radio and dropdown values in the saved XFA data.
The editor window was also rendered and visually inspected.

- PDFium 8100 and 7749: all three test executables passed, with 36 QtTest checks
  per configuration including setup/cleanup and the sample checks.
- AddressSanitizer + UndefinedBehaviorSanitizer: all three suites passed with
  PDFium 8100 (`ASAN_OPTIONS=detect_leaks=0`; PDFium itself is not instrumented).
- XFA disabled: 5 checks passed; no PDFium or qpdf dependency is needed.

Subsequent local validation on Arch Linux built the full KDE application and
PDF generator successfully. The build enables exceptions for the qpdf save
bridge under KDE's compiler settings. All three backend, save and editor test
executables passed with 39 QtTest checks, including regression coverage for
the sample's added date widgets and rejection of ambiguous names. The saved
dates were independently checked in the PDF values, appearances and rendered
page. Interactive use confirmed text and checkbox editing and saving.

The full-application `xfaGeneratorTest` subsequently passed all 7 QtTest checks
using an isolated D-Bus session. GUI regression tests also verify that the
Tools menu includes the editor action when a document opens before or after
GUI activation, opens the XFA editor, and retains the action across tab switches
and document reopening. Ordinary PDFs keep the action disabled.
