-- The fades the screens share, and how a level comes in.
require("common.ui.fade").setup()
require("common.ui.level_start").setup()
require("common.ui.dc_restart").setup()

-- The lines of text, the arrows and the ring hints that the game asks for
-- over the play.
require("common.ui.captions").setup()
require("common.ui.ring_hints").setup()

-- The screens the interface draws in place of the engine's.
require("common.ui.ring.screen").setup()
require("common.ui.screens.passport").setup()
require("common.ui.screens.settings").setup()
require("common.ui.screens.stats").setup()

-- These come in only once they first open: the Dreamcast's heap has no room
-- for every screen at once.
for screen, name in pairs({
  [trx.ui.Screen.COMBINE] = "common.ui.screens.combine",
  [trx.ui.Screen.EXAMINE] = "common.ui.screens.examine",
  [trx.ui.Screen.GLOBE_SELECT] = "common.ui.screens.globe",
  [trx.ui.Screen.LEVEL_VIEW] = "common.ui.screens.level_view",
  [trx.ui.Screen.PAUSE] = "common.ui.screens.pause",
  [trx.ui.Screen.PICTURE] = "common.ui.screens.picture",
}) do
  trx.ui.screens.define(screen, function(...)
    return require(name).open(...)
  end)
end
