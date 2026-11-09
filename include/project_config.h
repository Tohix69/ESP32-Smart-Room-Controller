#pragma once

// Select your local time zone using a POSIX TZ string.
// Bangladesh:  <+06>-6
// UTC:         UTC0
// India:       IST-5:30
// Nepal:       <+05:45>-5:45
// Pakistan:    PKT-5
// Singapore:   SGT-8
// UK:          GMT0BST,M3.5.0/1,M10.5.0/2
// US Eastern:  EST5EDT,M3.2.0/2,M11.1.0/2
// US Pacific:  PST8PDT,M3.2.0/2,M11.1.0/2
// See README for notes and examples. Use a POSIX TZ string, not an IANA name.
#define TIME_ZONE "<+06>-6"

// Change these before enabling OTA on your network.
#define OTA_USERNAME "admin"
#define OTA_PASSWORD "CHANGE_THIS_OTA_PASSWORD"
