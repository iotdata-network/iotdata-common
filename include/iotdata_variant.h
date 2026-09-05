#ifndef IOTDATA_COMMON_VARIANT_H
#define IOTDATA_COMMON_VARIANT_H

// ---------------------------------------------------------------------------------------------------------------------------
//
// iotdata_variant.h - selects the variant-suite definitions the app compiles against.
//
// The build injects the chosen variant FILE as -DIOTDATA_VARIANT='"...iotdata_variant_<name>.h"'
// (default in iotdata-common/make/config.mk); override it per project or per host to swap the
// variant set. There is NO default: if IOTDATA_VARIANT is unset the build breaks explicitly rather
// than silently pulling one in.
//
// ---------------------------------------------------------------------------------------------------------------------------

#ifdef IOTDATA_VARIANT
#include IOTDATA_VARIANT
#else
#error "IOTDATA_VARIANT is not defined: inject -DIOTDATA_VARIANT=\"<variant file>\" (see iotdata-common/make/config.mk). No default variant is included by design."
#endif

#endif /* IOTDATA_COMMON_VARIANT_H */
