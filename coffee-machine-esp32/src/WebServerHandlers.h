#ifndef WEBSERVER_HANDLERS_H
#define WEBSERVER_HANDLERS_H

#include "MachineController.h"
#include "SettingsManager.h"
#include <WebServer.h>


void setupWebServer(WebServer &server, MachineController &machine,
                    SettingsManager &settings);

#endif
