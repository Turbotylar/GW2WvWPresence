// Just enough of the Guild Wars 2 MumbleLink layout for this addon.
// See https://wiki.guildwars2.com/wiki/API:MumbleLink
#pragma once
#include <cstdint>

namespace Mumble
{
    enum class EProfession : unsigned char
    {
        None, Guardian, Warrior, Engineer, Ranger, Thief, Elementalist, Mesmer, Necromancer, Revenant
    };

    /* Nexus's parsed copy of the identity JSON (DataLink "DL_MUMBLE_LINK_IDENTITY") */
    struct Identity
    {
        char        Name[20];
        EProfession Profession;
        unsigned    Specialization;
        unsigned char Race;
        unsigned    MapID;
        unsigned    WorldID;
        unsigned    TeamColorID;
        bool        IsCommander;
        float       FOV;
        unsigned char UISize;
    };

    /* the raw shared memory block (DataLink "DL_MUMBLE_LINK"), up to the identity JSON */
    struct Data
    {
        uint32_t UIVersion;
        uint32_t UITick;
        float    AvatarPosition[3], AvatarFront[3], AvatarTop[3];
        wchar_t  Name[256];
        float    CameraPosition[3], CameraFront[3], CameraTop[3];
        wchar_t  Identity[256];
    };
}
