FinalBurn Neo CPS3 for the PS Vita
==================================

A FinalBurn Neo libretro core with only the CPS3 driver (Street Fighter III,
2nd Impact, 3rd Strike, JoJo's Venture, JoJo's Bizarre Adventure, Red Earth),
made to run them at full speed on the PS Vita: SH-2 dynamic recompiler,
faster renderer, and the games' own wide screen mode.

Built with RetroArch @RETROARCH@.
Source: @SOURCE@


Install
-------
You need RetroArch installed on the Vita (an up to date version is best).
Copy the app folder of this zip to the root of ux0: (it adds two files to
ux0:app/RETROARCH, the stock FinalBurn Neo core is left alone). In RetroArch
the core is "Arcade (FinalBurn Neo CPS3)".


Games
-----
FinalBurn Neo romsets, as .zip: the .7z sets from MAME don't load on the
Vita. A clone (sfiii3n, jojon...) needs its parent set (sfiii3, jojo...) in the
same folder. No CD images are needed.

The game settings are saved as the cabinet's EEPROM in
ux0:data/retroarch/savefiles/fbneo/<game>.fs.


Wide screen
-----------
Core option "Screen mode", Wide: the games draw a 16:9 picture.

  sfiii, sfiii2   their own wide mode. It is a game setting, so it takes
                  effect from the second time the game starts.
  sfiii3          the game has the mode but left it unfinished; the core
                  patches it, based on the sfiii3ws hack, from the first start.

Normal plays the original 4:3 games, unchanged.


Credits and licenses (see the licenses folder)
--------------------
FinalBurn Neo, Team FBNeo: non-commercial license.
SH-2 dynamic recompiler from PicoDrive, by notaz and irixxxx: non-commercial,
the source of modified versions must be available (see Source above).
RetroArch, the libretro team: GPLv3.
sfiii3 wide screen: based on the sfiii3ws hack (20220908).

Not for sale.
