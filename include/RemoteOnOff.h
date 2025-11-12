#pragma once

#include <WString.h>

class RemoteOnOffClass {
public:
    RemoteOnOffClass();
    void init();
    bool config(String &urlOn, String &urlOff, unsigned int switchInterval, int switchOnSaldoW, int switchOffSaldoW);
    void loop();
    void setCurrentValues(bool dataAreValid, uint32_t v1_7_0, uint32_t v2_7_0);

private:
    String _urlOn, _urlOff;
    unsigned long _switchIntervalMs;
    long _switchOnSaldoW, _switchOffSaldoW;

    bool _enabled = false;

    unsigned long _lastLoopMs;
    unsigned long _lastSwitchedMs;

    long _saldoHistory[5];
    size_t _saldoHistoryLen;

    typedef enum {
        undefined = -1,
        off,
        on
    } switchState_t;
    switchState_t _lastSentState = undefined;

    int sendURL(switchState_t newState);
};

extern RemoteOnOffClass RemoteOnOff;

/* vim:set ts=4 et: */
