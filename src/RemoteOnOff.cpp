// Anhand des Durchschnitts-Saldos des letzte 5 Sekunden
// vergleichen mit einem Aus- und Einschaltert,
// wird eine URL aufgerufen.
//
// Das kann z.B. für eine via Netzwerk schaltbare Steckdose verwendet werden
//
// Um nicht dauern ein-/auszuschalten, wird ein Interval angegeben, welches
// beim Wechsel zwischen ein/aus nicht unterschritten wird

#include "RemoteOnOff.h"

#include <WiFiClient.h>
#include <ESP8266HTTPClient.h>

// TODO: Fragen zu dem Thema:
// * Was tun wir, solange wir keine Zählerdaten haben
// * Soll vor einem Reboot noch Ein-/Ausgeschaltet werden
// * Was tun, wenn der Sync zum Zähler verlorengeht
// * Was tun, wenn die Konfiguration während der Laufzeit geändert wird
// * Ev. harmonisches Mittel verwenden?


// TODO: Refactor this value
extern int valid;
extern uint32_t a_result[10];


#define LOOP_INTERVAL_MS 1000ul

RemoteOnOffClass::RemoteOnOffClass()
{
}

void RemoteOnOffClass::init()
{
}

int RemoteOnOffClass::sendURL(switchState_t newState)
{
    int httpResultCode;
    HTTPClient http;
    WiFiClient client;
    http.begin(client, (newState == on) ?_urlOn :_urlOff );
    http.setReuse(false);
    httpResultCode = http.GET();
    http.end();

    if (httpResultCode == HTTP_CODE_OK) {
        // writeEvent("INFO", "sys", "Switch XX Url sent", "");
        // TODO: Check if we should update '_lastSentState' just here
    }
    return httpResultCode;
}

bool RemoteOnOffClass::config(String &urlOn, String &urlOff, unsigned int switchIntervalSec, int switchOnSaldoW, int switchOffSaldoW)
{
    if (switchOnSaldoW >= switchOffSaldoW) {
        return false;
    }

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

void RemoteOnOffClass::setCurrentValues(bool dataAreValid, uint32_t v1_7_0, uint32_t v2_7_0)
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

    setCurrentValues(valid==5, a_result[4], a_result[5]); // TODO: Das sollte Event-getriggert sein

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

    sendURL(newState);

    _lastSentState = newState;
    _lastSwitchedMs = now;
}

RemoteOnOffClass RemoteOnOff;

/* vim:set ts=4 et: */
