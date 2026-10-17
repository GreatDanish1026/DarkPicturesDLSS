// Single source of the product name and version number (add-on, version resource).
// One source tree builds two products: QuarryDLSS (default) and, with QDLSS_DARK_PICTURES
// defined (build.ps1 -Product DarkPictures), DarkPicturesDLSS for The Dark Pictures Anthology.
#pragma once

#if defined(QDLSS_DARK_PICTURES)
#define QDLSS_PRODUCT "DarkPicturesDLSS"  // add-on name, ReShade.ini section, log and dump file prefix
#define QDLSS_GAMES "The Dark Pictures Anthology"
#define QDLSS_NGX_PROJECT_ID "6fe9a2b4-ad8f-4788-af08-f181a7c9ee71"
#define QDLSS_VERSION_MAJOR 0
#define QDLSS_VERSION_MINOR 1
#define QDLSS_VERSION_PATCH 13
#define QDLSS_VERSION "0.1.13"
#define QDLSS_RC_FILENAME "DarkPicturesDLSS.addon64"
#define QDLSS_RC_PRODUCT "DarkPicturesDLSS for The Dark Pictures Anthology"
#define QDLSS_RC_DESCRIPTION "DarkPicturesDLSS ReShade add-on"
#else
#define QDLSS_PRODUCT "QuarryDLSS"
#define QDLSS_GAMES "The Quarry"
#define QDLSS_NGX_PROJECT_ID "6c2f3b1e-9d4a-4e57-8b6f-2a1d7c9e0f53"
#define QDLSS_VERSION_MAJOR 1
#define QDLSS_VERSION_MINOR 1
#define QDLSS_VERSION_PATCH 0
#define QDLSS_VERSION "1.1.0"
#define QDLSS_RC_FILENAME "QuarryDLSS.addon64"
#define QDLSS_RC_PRODUCT "QuarryDLSS for The Quarry"
#define QDLSS_RC_DESCRIPTION "QuarryDLSS ReShade add-on"
#endif
