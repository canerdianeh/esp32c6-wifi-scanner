/*
    E1.31 (sACN) receiver: joins the multicast group of every subscribed
    universe and also accepts unicast.
*/
#pragma once

#include "esp_err.h"

esp_err_t sacn_start(void);
