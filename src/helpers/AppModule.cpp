#include "AppModule.h"

AppModule* AppModules::_mods[APP_MODULE_MAX] = { nullptr };
uint8_t    AppModules::_count = 0;
bool       AppModules::_setup_done = false;
