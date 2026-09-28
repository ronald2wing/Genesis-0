# Dry-run qmlformat comparison, invoked via `cmake -P` from the qml_format_check
# ctest. For every file in GENESIS_QML_SOURCES it runs `qmlformat -s SETTINGS`
# to a scratch file and compares that byte-for-byte against the on-disk file,
# printing each file that would be reformatted and failing if any differ.
#
# Unlike clang-format, qmlformat has no `--dry-run --Werror`; this script is
# the equivalent. It is a comparison, not an edit, so the tree is never touched.

if(NOT QMLFORMAT OR NOT SETTINGS OR NOT GENESIS_QML_SOURCES OR NOT CHECK_TMPDIR)
    message(FATAL_ERROR
        "qml_format_check: QMLFORMAT, SETTINGS, GENESIS_QML_SOURCES and "
        "CHECK_TMPDIR must be defined")
endif()

set(tmp "${CHECK_TMPDIR}/qml_format_check.qml")
set(drift FALSE)

foreach(file IN LISTS GENESIS_QML_SOURCES)
    execute_process(
        COMMAND "${QMLFORMAT}" -s "${SETTINGS}" "${file}"
        RESULT_VARIABLE rc
        OUTPUT_FILE "${tmp}"
        ERROR_VARIABLE err)
    if(NOT rc EQUAL 0)
        message("qmlformat failed on ${file}: ${err}")
        set(drift TRUE)
        continue()
    endif()
    execute_process(
        COMMAND "${CMAKE_COMMAND}" -E compare_files "${file}" "${tmp}"
        RESULT_VARIABLE cmp)
    if(NOT cmp EQUAL 0)
        message("Would reformat: ${file}")
        set(drift TRUE)
    endif()
endforeach()

file(REMOVE "${tmp}")

if(drift)
    message(FATAL_ERROR
        "QML formatting drift detected. Run the `qml_format` target and commit "
        "the result.")
endif()
