/*
    Copyright 2023 Jesse Talavera-Greenberg

    melonDS DS is free software: you can redistribute it and/or modify it under
    the terms of the GNU General Public License as published by the Free
    Software Foundation, either version 3 of the License, or (at your option)
    any later version.

    melonDS DS is distributed in the hope that it will be useful, but WITHOUT ANY
    WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
    FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.

    You should have received a copy of the GNU General Public License along
    with melonDS DS. If not, see http://www.gnu.org/licenses/.
*/

#include <Platform.h>
#include <net/net_socket.h>

bool Platform::LAN_Init()
{
    // TODO: Implement
    return false;
}

void Platform::LAN_DeInit()
{
    // TODO: Implement
}

int Platform::LAN_SendPacket(u8* data, int len)
{
    // TODO: Implement
    return 0;
}

int Platform::LAN_RecvPacket(u8* data)
{
    // TODO: Implement
    return 0;
}