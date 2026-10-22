/*
    Art-Net 4 receiver: ArtDmx for the subscribed universes (15-bit port
    addresses), and ArtPollReply so consoles can discover the node.
*/
#pragma once

#include "esp_err.h"

esp_err_t artnet_start(void);
