/*
    The NimBLE host, shared by the BLE scanner and the phone GPS link.

    ble_host_init() sets up the stack (users add GATT services and security
    settings after it); ble_host_start() runs it. Both are idempotent. Sync
    callbacks run in the host task each time the stack is (re)synced; one added
    after sync runs straight away.
*/
#pragma once

#include <stdbool.h>

bool ble_host_init(void);
void ble_host_start(void);
void ble_host_on_sync(void (*cb)(void));
bool ble_host_synced(void);
