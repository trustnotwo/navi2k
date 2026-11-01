#ifndef LP_VERSION_H
#define LP_VERSION_H

// The player's version, in one place: the .exe's version info (app.rc), the About box and
// the user agent. scripts/make_build.py rewrites this file for every numbered build.
//
// LP_DEV_BUILD is the internal dev build number (0001, 0002, ...); 0 means a release.

#define LP_VERSION_MAJOR 1
#define LP_VERSION_MINOR 3
#define LP_VERSION_PATCH 0
#define LP_DEV_BUILD 0
#define LP_VERSION_TEXT "1.3"

#define LP_WIDEN_(s) L##s
#define LP_WIDEN(s) LP_WIDEN_(s)
#define LP_VERSION_WTEXT LP_WIDEN(LP_VERSION_TEXT)

#endif
