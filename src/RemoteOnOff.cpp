// Abhängig des Durchschnitts-Saldos der letzten 5 Zählerwerte
// (üblicherweise - die letzten 5 Sekunden) verglichen
// mit einem Aus- / Einschaltwert, wird eine URL aufgerufen.
//
// Das kann z.B. für eine via Netzwerk schaltbare Steckdose verwendet werden.
//
// Um nicht dauernd ein-/auszuschalten, wird ein Interval angegeben, welches
// beim Wechsel zwischen ein/aus nicht unterschritten wird.
//
// Sollte der AmisReader den Sync mit dem Zähler verlieren, wird
// dies einfach ignoriert (diese Werte werden nicht berücksichtigt)


#include "RemoteOnOff.h"

#include <WiFiClient.h>
#include <ESP8266HTTPClient.h>

// TODO: Fragen zu dem Thema:
// * Soll vor einem Reboot noch Ein-/Ausgeschaltet werden
// * Was tun, wenn die Konfiguration während der Laufzeit geändert wird
// * Ev. harmonisches Mittel verwenden?


// TODO: Refactor this external values
extern int valid;
extern uint32_t a_result[10];


#define LOOP_INTERVAL_MS 1000ul

RemoteOnOffClass::RemoteOnOffClass()
{
}

void RemoteOnOffClass::init()
{
}

void RemoteOnOffClass::sendURL(switchState_t stateToSend, unsigned long now)
{
    int httpResultCode;
    HTTPClient http;
    WiFiClient client;
    http.begin(client, (stateToSend == on) ?_urlOn :_urlOff );
    http.setReuse(false);
    httpResultCode = http.GET();
    http.end();
    if ( (_honorHttpResult && httpResultCode == HTTP_CODE_OK) || (!_honorHttpResult) ) {
        _lastSentState = stateToSend;
    }
    _lastSwitchedMs = now;  // TODO: should we also update _lastSwitchedMs just on success?
}

bool RemoteOnOffClass::config(String &urlOn, String &urlOff,
                              unsigned int switchIntervalSec,
                              int switchOnSaldoW, int switchOffSaldoW,
                              bool honorHttpResult)
{
    if (switchOnSaldoW >= switchOffSaldoW) {
        return false;
    }

    _honorHttpResult = honorHttpResult;
    _urlOn = urlOn;
    _urlOff = urlOff;
    if (_urlOn.length() > 0 && _urlOff.length() > 0) {
        _enabled = true;
    } else {
        _enabled = false;
    }
    if (switchIntervalSec < 5) {
        switchIntervalSec = 5;
    }
    _switchIntervalMs = (unsigned long)switchIntervalSec * 1000ul;

    _switchOnSaldoW = switchOnSaldoW;
    _switchOffSaldoW = switchOffSaldoW;

    _saldoHistoryLen = 0;
    memset(_saldoHistory, 0 , sizeof(_saldoHistory));

    _lastLoopMs = millis() - LOOP_INTERVAL_MS - 1; // damit gehts sofort im loop() los
    return true;
}

void RemoteOnOffClass::addCurrentValues(bool dataAreValid, uint32_t v1_7_0, uint32_t v2_7_0)
{
    if (!dataAreValid) {
        return;
    }
    // aktuelles Saldo (Erzeugung - Verbrauch) ganz hinten zur History hinzufügen
    for (size_t i=1; i < std::size(_saldoHistory); i++) {
        _saldoHistory[i-1] = _saldoHistory[i];
    }
    _saldoHistoryLen++;
    if (_saldoHistoryLen > std::size(_saldoHistory)) {
        _saldoHistoryLen = std::size(_saldoHistory);
    }
    _saldoHistory[std::size(_saldoHistory) - 1] = v1_7_0 - v2_7_0;
}

void RemoteOnOffClass::loop()
{
    if (!_enabled) {
        return;
    }

    unsigned long now = millis();

    if(now - _lastLoopMs < LOOP_INTERVAL_MS) {
        return;
    }
    _lastLoopMs = now;

    addCurrentValues(valid==5, a_result[4], a_result[5]); // TODO: Das sollte Event-getriggert sein

    // TODO: Prüfen, ob wir überhaupt mit dem Netzwerk verbunden sind

    // TODO: Nachdem booten (wenn _lastSentState == undefined zuerst mal ausschalten)

    if(_saldoHistoryLen != std::size(_saldoHistory)) {
        // Zuerst sollten wir die History vollständig gefüllt haben
        return;
    }

    if (_lastSentState != undefined && now - _lastSwitchedMs < _switchIntervalMs) {
        // Noch nicht genug Zeit seit dem letzten Schaltvorgang vergangen!
        return;
    }

    // Nun den Arthmetischen-Mittelwert der History berechnen
    long saldo_mw = 0;
    for (size_t i=0; i < _saldoHistoryLen; i++) {
        saldo_mw += _saldoHistory[i];
    }
    saldo_mw /= _saldoHistoryLen;

    // Neuen Status berechnen
    switchState_t newState = undefined;
    if (saldo_mw < _switchOnSaldoW) {
        newState = on;
    }
    if (saldo_mw > _switchOffSaldoW) {
        newState = off;
    }

    if (newState == undefined || newState == _lastSentState) {
        return;
    }
    sendURL(newState, now);
}

RemoteOnOffClass RemoteOnOff;

/* vim:set ts=4 et: */
