# graphic 2D display rewrite

Base this implementation on the user's imported graphic project and the
official LS7A2000 manual V1.3. Keep PCI/driver, display, GEM/TTM, and soc layers.
Follow Linux kernel style and the current 7.3 generic DRM/TTM APIs.

Do not inspect or copy existing Loongson hardware-driver implementations or
lg110-open-gpu. Generic DRM/TTM core code and the user's baseline are allowed.
Do not claim undocumented hardware behavior as a manual guarantee. Initial
output support may retain a firmware-configured link/mode while missing PHY
or DDC specifications are resolved and explicitly documented.
