#ifndef STALENESS_H
#define STALENESS_H

#include "types.h"

void invalidateOldReadings();
void invalidateInsideAirQuality();
void invalidateStaleApiData();

#endif // STALENESS_H
