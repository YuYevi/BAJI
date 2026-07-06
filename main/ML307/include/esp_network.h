#ifndef ESP_NETWORK_H
#define ESP_NETWORK_H

#include "network_interface.h"


class EspNetwork : public NetworkInterface {
public:
    virtual ~EspNetwork() = default;
};

#endif 

