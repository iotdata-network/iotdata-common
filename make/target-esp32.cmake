# target-esp32.cmake — shared compile setup for the esp32 example/device components: optional
# config / variant define injection (IOTDATA_VARIANT + IOTDATA_CONFIG, each applied only if the
# Makefile passed a non-empty value) and the canonical strict warning set (matches the native
# IOTDATA_CFLAGS_COMMON, plus -Os for size). Project-specific defines (e.g. a relay's carrier /
# sim-loss) stay in that project's own CMakeLists.
#
# A component's main/CMakeLists.txt includes this AFTER idf_component_register() (so COMPONENT_LIB
# exists), located via the IOTDATA_SRC_COMMON cache var the Makefile injects (target-esp32.mk):
#   include("${IOTDATA_SRC_COMMON}/make/target-esp32.cmake")

# --- optional config / variant selection (FILE paths the Makefile injects as -D cache vars) ---
if(DEFINED IOTDATA_VARIANT AND NOT "${IOTDATA_VARIANT}" STREQUAL "")
    target_compile_definitions(${COMPONENT_LIB} PRIVATE IOTDATA_VARIANT="${IOTDATA_VARIANT}")
endif()
if(DEFINED IOTDATA_CONFIG AND NOT "${IOTDATA_CONFIG}" STREQUAL "")
    target_compile_definitions(${COMPONENT_LIB} PRIVATE IOTDATA_CONFIG="${IOTDATA_CONFIG}")
    message(STATUS "app: config override = ${IOTDATA_CONFIG}")
endif()

# --- version identity (the Makefile's APP / VERSION / stamp, see target-esp32.mk) ---
# These are the single source of the node's VERSION report (iotdata_node_version.h). The stamp is
# passed IN rather than taken from __DATE__ or the IDF app descriptor, and that is deliberate:
# CONFIG_APP_REPRODUCIBLE_BUILD leaves esp_app_desc_t.date/.time empty by design, because a
# timestamp the compiler invents is precisely what makes a build unreproducible. As an explicit
# build INPUT it is compatible with reproducibility -- pass a fixed stamp and the binary is
# reproducible; pass `date -u` and it records when it was built.
foreach(_v IOTDATA_VERSION_APP IOTDATA_VERSION_SEMVER IOTDATA_VERSION_STAMP)
    if(DEFINED ${_v} AND NOT "${${_v}}" STREQUAL "")
        target_compile_definitions(${COMPONENT_LIB} PRIVATE ${_v}="${${_v}}")
    endif()
endforeach()
if(DEFINED IOTDATA_VERSION_APP AND NOT "${IOTDATA_VERSION_APP}" STREQUAL "")
    message(STATUS "app: version = ${IOTDATA_VERSION_APP}/${IOTDATA_VERSION_SEMVER}/${IOTDATA_VERSION_STAMP}")
endif()

# --- strict warnings ---
target_compile_options(${COMPONENT_LIB} PRIVATE
    -Wdouble-promotion
    -Werror=double-promotion
    -Wfloat-conversion
    -Werror=float-conversion
    -Wall -Wextra
    -Werror
    -Wstrict-prototypes
    -Wold-style-definition
    -Wcast-align -Wcast-qual -Wconversion
    -Wfloat-equal -Wformat=2 -Wformat-security
    -Winit-self -Wjump-misses-init
    -Wlogical-op -Wmissing-include-dirs
    -Wnested-externs -Wpointer-arith
    -Wredundant-decls -Wshadow
    -Wstrict-overflow=2 -Wswitch-default
    -Wunreachable-code -Wunused
    -Wwrite-strings
    -Wnull-dereference
    -Wduplicated-cond
    -Wduplicated-branches
    -Wrestrict
    -Wstringop-overflow
    -Wno-duplicated-branches
    -Wvla
    -Wstack-usage=1024
    -Os
)
