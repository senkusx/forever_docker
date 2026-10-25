/*
 * Companions: ".bot admin" commands used by the web panel through SOAP
 */

#ifndef COMPANION_ADMIN_H
#define COMPANION_ADMIN_H

#include "ChatCommand.h"
#include <span>

namespace Companions
{
    std::span<Trinity::ChatCommands::ChatCommandBuilder const> AdminCommandTable();
}

#endif
