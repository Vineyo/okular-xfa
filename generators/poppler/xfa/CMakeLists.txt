# SPDX-FileCopyrightText: 2026 OpenAI
# SPDX-License-Identifier: BSD-3-Clause

# This component can be tested without building the KDE application:
# cmake -S generators/poppler/xfa -B build-xfa -DENABLE_XFA=ON -DPDFium_DIR=...
cmake_minimum_required(VERSION 3.22)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(OkularXfaTests LANGUAGES CXX)
    set(CMAKE_CXX_STANDARD 20)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
    include(CTest)
    find_package(Qt6 6.4 REQUIRED COMPONENTS Core Gui Widgets Xml PrintSupport Test)
endif()

option(ENABLE_XFA "Enable experimental XFA viewing and filling (requires self-initializing PDFium and qpdf; see README.xfa.md)" OFF)

add_library(okular_xfa STATIC ../xfadocument.cpp)
set_target_properties(okular_xfa PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_include_directories(okular_xfa PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/..")
target_link_libraries(okular_xfa PUBLIC Qt6::Core Qt6::Gui Qt6::Xml Qt6::PrintSupport)
target_compile_definitions(okular_xfa PRIVATE HAVE_PDFIUM_XFA=$<BOOL:${ENABLE_XFA}>)

if(ENABLE_XFA)
    find_package(qpdf CONFIG REQUIRED)
    target_sources(okular_xfa PRIVATE ../xfasave.cpp)
    target_link_libraries(okular_xfa PRIVATE qpdf::libqpdf)
    target_compile_definitions(okular_xfa PRIVATE POINTERHOLDER_TRANSITION=4)
    # KDE disables exceptions by default, but the qpdf save bridge uses them
    # to report unsupported forms and save failures. Standalone builds retain
    # the compiler's default exception support without KDECompilerSettings.
    if(COMMAND kde_target_enable_exceptions)
        kde_target_enable_exceptions(okular_xfa PRIVATE)
    endif()
    find_package(PDFium CONFIG REQUIRED)
    if(NOT TARGET pdfium)
        message(FATAL_ERROR "PDFium must provide the 'pdfium' CMake target. See README.xfa.md for the supported distribution.")
    endif()
    include(CheckCXXSourceCompiles)
    set(CMAKE_REQUIRED_LIBRARIES pdfium)
    check_cxx_source_compiles("
        #define PDF_ENABLE_XFA
        #include <fpdfview.h>
        #include <fpdf_formfill.h>
        int main() { FPDF_FORMFILLINFO info{}; info.version = 2; (void)info;
            FPDF_BSTR value{};
            FPDF_BStr_Init(&value);
            FPDF_BStr_Clear(&value);
            return FPDF_LoadXFA(nullptr); }
        " PDFIUM_HAS_XFA)
    unset(CMAKE_REQUIRED_LIBRARIES)
    if(NOT PDFIUM_HAS_XFA)
        message(FATAL_ERROR "The PDFium XFA compile/link check failed. This can mean missing XFA symbols, headers, or a linker problem. Inspect CMakeFiles/CMakeConfigureLog.yaml and clear the cached check with -U PDFIUM_HAS_XFA before retrying.")
    endif()
    target_compile_definitions(okular_xfa PRIVATE PDF_ENABLE_XFA)
    target_link_libraries(okular_xfa PRIVATE pdfium)
endif()

add_library(okular_xfa_ui STATIC ../xfaeditor.cpp ../xfaeditor.h)
set_target_properties(okular_xfa_ui PROPERTIES AUTOMOC ON POSITION_INDEPENDENT_CODE ON)
target_link_libraries(okular_xfa_ui PUBLIC okular_xfa Qt6::Widgets)

if(ENABLE_XFA AND CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    add_executable(xfa-editor editor_main.cpp)
    target_link_libraries(xfa-editor PRIVATE okular_xfa_ui)
endif()

if(BUILD_TESTING)
    add_executable(xfaDocumentTest ../autotests/testxfadocument.cpp)
    set_target_properties(xfaDocumentTest PROPERTIES AUTOMOC ON)
    target_compile_definitions(xfaDocumentTest PRIVATE HAVE_PDFIUM_XFA=$<BOOL:${ENABLE_XFA}>)
    target_link_libraries(xfaDocumentTest PRIVATE okular_xfa Qt6::Test)
    if(ENABLE_XFA)
        target_link_libraries(xfaDocumentTest PRIVATE pdfium)
    endif()
    add_test(NAME xfaDocumentTest COMMAND xfaDocumentTest)
    set_tests_properties(xfaDocumentTest PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    if(ENABLE_XFA)
        add_executable(xfaSaveTest ../autotests/testxfasave.cpp)
        set_target_properties(xfaSaveTest PROPERTIES AUTOMOC ON)
        target_compile_definitions(xfaSaveTest PRIVATE POINTERHOLDER_TRANSITION=4)
        target_link_libraries(xfaSaveTest PRIVATE okular_xfa qpdf::libqpdf Qt6::Test)
        if(COMMAND kde_target_enable_exceptions)
            kde_target_enable_exceptions(xfaSaveTest PRIVATE)
        endif()
        add_test(NAME xfaSaveTest COMMAND xfaSaveTest)
        add_executable(xfaEditorTest ../autotests/testxfaeditor.cpp)
        set_target_properties(xfaEditorTest PROPERTIES AUTOMOC ON)
        target_link_libraries(xfaEditorTest PRIVATE okular_xfa_ui Qt6::Test)
        add_test(NAME xfaEditorTest COMMAND xfaEditorTest)
        set_tests_properties(xfaEditorTest PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 60)
    endif()
endif()
