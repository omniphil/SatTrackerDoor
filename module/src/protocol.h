// Wire protocol between the Satellite Tracker door (on the BBS) and the TRACE
// module (on the caller's machine). Shared verbatim by both sides.
//
// Division of labour:
//   - The DOOR owns the data: orbit elements from CelesTrak (cached, since
//     CelesTrak asks not to be fetched more than every couple of hours), the
//     caller's home place (searched through Open-Meteo, as the Weather door
//     does) and the Earth pictures (an asset, sent once and cached).
//   - The MODULE owns the picture and the maths that moves it: it runs the same
//     orbit code (orbit.h) every frame, so hundreds of satellites move smoothly
//     without the door streaming positions.
//
// Everything is little-endian. Every message starts with a SatMsgHeader.
#pragma once
#include <stdint.h>

#define SAT_PROTO_VERSION 1
#define SAT_MAX_PAYLOAD   4096       // one packet, under the 8 KB text-command limit once base64'd
#define SAT_MAX           200        // satellites in a group
#define SAT_MAX_PLACES    8
#define SAT_NAME_LEN      48
#define SAT_SEARCH_LEN    40

// The groups, in CelesTrak's names (door) and on-screen names (both).
enum { GRP_STATIONS = 0, GRP_VISUAL, GRP_WEATHER, GRP_AMATEUR, GRP_GPS, GRP_SCIENCE, GRP_COUNT };
static const char *const SAT_GROUP_KEY[GRP_COUNT]  = { "stations", "visual", "weather", "amateur", "gps-ops", "science" };
static const char *const SAT_GROUP_NAME[GRP_COUNT] = { "Space stations", "Brightest", "Weather", "Ham radio", "GPS", "Science" };

#define SAT_ISS 25544u

// Door -> module
enum SatMsgIn {
    SAT_IN_PREFS  = 1,   // SatPrefs
    SAT_IN_ELEMS  = 2,   // flags bit0 = first batch of a group (clear the list); count * SatWire
    SAT_IN_DONE   = 3,   // uint8 group: the group is complete
    SAT_IN_PLACES = 4,   // count * SatPlace: search results
    SAT_IN_STATUS = 5,   // uint8 kind (SAT_STATUS_*) + uint8 len + text
};

// Module -> door
enum SatMsgOut {
    SAT_OUT_READY  = 1,  // no payload: send prefs and the group
    SAT_OUT_GROUP  = 2,  // uint8 group: send this group (and remember it)
    SAT_OUT_SEARCH = 3,  // uint8 len + text typed into the place box
    SAT_OUT_PICK   = 4,  // uint8 index into the last SAT_IN_PLACES
    SAT_OUT_SELECT = 5,  // uint32 norad: the caller picked this satellite (remember it)
};

enum { SAT_STATUS_INFO = 0, SAT_STATUS_BUSY = 1, SAT_STATUS_ERROR = 2 };

typedef struct {
    uint8_t  type;
    uint8_t  flags;
    uint16_t count;
    uint32_t bytes;
} SatMsgHeader;

// One satellite's orbit elements (orbit.h OrbElements, flattened).
typedef struct {
    double   epoch, incl, raan, ecc, argp, mo, nRevDay, bstar;
    uint32_t norad;
    char     name[24];
    uint32_t reserved;
} SatWire;                       // 96 bytes; 42 to a packet

typedef struct {
    int32_t lat1e4, lon1e4;
    char    name[SAT_NAME_LEN];
} SatPlace;

typedef struct {
    uint32_t doorNow;            // the door's clock, so the module keeps time
    int32_t  utcOffset;          // seconds east of UTC at the home place (pass times are shown there)
    int32_t  lat1e4, lon1e4;     // home
    uint32_t selected;           // NORAD number of the satellite to follow
    uint8_t  group;
    uint8_t  firstRun;           // 1 = no home place yet: open the picker
    uint8_t  reserved[2];
    char     place[SAT_NAME_LEN];
} SatPrefs;
