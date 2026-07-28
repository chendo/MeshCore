#pragma once

#include "Mesh.h"


class LocationProvider {
protected:
    bool _time_sync_needed = true;

public:
    virtual void syncTime() { _time_sync_needed = true; }
    virtual bool waitingTimeSync() { return _time_sync_needed; }
    virtual long getLatitude() = 0;
    virtual long getLongitude() = 0;
    virtual long getAltitude() = 0;
    virtual long satellitesCount() = 0;
    // Bytes received from the GPS module since boot. Zero satellites with bytes
    // arriving means the module is alive and cannot see sky; zero satellites
    // with ZERO bytes means it is not talking to us at all (power, wiring,
    // baud, or a hardware switch) — two very different faults that look
    // identical from satellite count alone. 0 = this driver does not report it.
    virtual uint32_t rawBytesRx() const { return 0; }
    virtual bool isValid() = 0;
    virtual long getTimestamp() = 0;
    virtual void sendSentence(const char * sentence);
    virtual void reset() = 0;
    virtual void begin() = 0;
    virtual void stop() = 0;
    virtual void loop() = 0;
    virtual bool isEnabled() = 0;
};
