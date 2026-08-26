if(NOT DEFINED KGV_LIBRARY OR NOT DEFINED KGV_NM)
  message(FATAL_ERROR "KGV_LIBRARY and KGV_NM are required")
endif()

execute_process(
  COMMAND "${KGV_NM}" -D --defined-only "${KGV_LIBRARY}"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE symbols
  ERROR_VARIABLE failure)
if(NOT status EQUAL 0)
  message(FATAL_ERROR "could not inspect shared-library exports: ${failure}")
endif()

set(expected
  kgv_abi_version
  kgv_status_name
  kgv_engine_open
  kgv_job_create
  kgv_job_run
  kgv_job_cancel
  kgv_job_destroy
  kgv_engine_close)

foreach(symbol IN LISTS expected)
  if(NOT symbols MATCHES "[ \t]${symbol}(@@KILIX_VOICEGEN_1)?([\r\n]|$)")
    message(FATAL_ERROR "required ABI symbol is not exported: ${symbol}")
  endif()
endforeach()

if(symbols MATCHES "[ \t]_Z")
  message(FATAL_ERROR "shared library exports a C++ symbol outside the C ABI:\n${symbols}")
endif()
