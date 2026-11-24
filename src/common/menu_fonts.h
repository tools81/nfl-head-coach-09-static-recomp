#pragma once
// hc09: the menus' fonts in the Barlow family (menu_fonts.cpp).

namespace hc09::menu_fonts
{
    // Puts fonts\menus\*.ttf in place of the game's menu fonts in qkl_interface.ast (MenuFonts, HC09_MENU_FONTS: on
    // by default). After the disc is mounted, before the game runs.
    void Apply();
}
