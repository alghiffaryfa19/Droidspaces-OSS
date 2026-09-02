#ifndef TRACY_ZONES_H
#define TRACY_ZONES_H

/* Shared Tracy C API shim for the jni sources.
 *
 * With -DANLAND_TRACY=ON the tracy_client target puts <tracy/TracyC.h> on the
 * include path and defines TRACY_ENABLE, so the real macros come in. With OFF
 * the header is absent and we provide matching no-op macros (TracyC.h does the
 * same itself when TRACY_ENABLE is undefined). Zone macros are function-like;
 * each expansion declares a variable named after the ctx argument, so distinct
 * ctx names are required within one scope. */
#if __has_include(<tracy/TracyC.h>)
#include <tracy/TracyC.h>
#else
#define TracyCZoneN(c, x, y)
#define TracyCZoneEnd(c)
#define TracyCFrameMark
#define TracyCFrameMarkNamed(x)
#define TracyCPlot(x, y)
#endif

#endif /* TRACY_ZONES_H */
