#pragma once

/* Exactly one WARTHOG_REGION_<XX> must be set by the per-region build env. */

#if defined(WARTHOG_REGION_US)
#define WARTHOG_REGION_NAME "US"
#define WARTHOG_COUNTRY_CODE "US"
#elif defined(WARTHOG_REGION_EU)
#define WARTHOG_REGION_NAME "EU"
#define WARTHOG_COUNTRY_CODE "EU"
#elif defined(WARTHOG_REGION_JP)
#define WARTHOG_REGION_NAME "JP"
#define WARTHOG_COUNTRY_CODE "JP"
#elif defined(WARTHOG_REGION_KR)
#define WARTHOG_REGION_NAME "KR"
#define WARTHOG_COUNTRY_CODE "KR"
#elif defined(WARTHOG_REGION_AU)
#define WARTHOG_REGION_NAME "AU"
#define WARTHOG_COUNTRY_CODE "AU"
#else
#error "Warthog: define one of WARTHOG_REGION_{US,EU,JP,KR,AU} via build flags"
#endif
