/* Hand-written replacement for libmad's autoconf config.h (MinGW, 32-bit x86). */
#ifndef LIBMAD_CONFIG_H
#define LIBMAD_CONFIG_H

#define HAVE_ASSERT_H    1
#define HAVE_LIMITS_H    1
#define HAVE_ERRNO_H     1
#define HAVE_FCNTL_H     1
#define HAVE_SYS_TYPES_H 1

#define SIZEOF_INT       4
#define SIZEOF_LONG      4
#define SIZEOF_LONG_LONG 8

/* No USE_ASYNC: the async decoder uses fork()/pipes. */
/* No OPT_SPEED / OPT_ACCURACY: libmad's defaults. */

#define PACKAGE         "libmad"
#define PACKAGE_NAME    "MPEG Audio Decoder"
#define PACKAGE_VERSION "0.15.1b"
#define VERSION         "0.15.1b"

#endif
