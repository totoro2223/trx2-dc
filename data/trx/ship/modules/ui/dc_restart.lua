-- Dreamcast: a row under the graphics mode that starts the level again, so
-- that a mode's other resolution takes effect (it changes as a level loads).
--
--   require("common.ui.dc_restart").setup()  -- once, from a global script

local M = {}

local KEY = "rendering.dc_restart_level"

trx.locale.declare({
  ["settings/rendering.dc_restart_level/title"] = "Restart level",
  ["settings/rendering.dc_restart_level/description"] = "Starts the level again from its beginning. A mode's resolution changes then.",
})

function M.setup()
  local ok, mode = pcall(trx.config.describe, "rendering.dc_mode")
  if not ok or mode == nil then
    return
  end
  trx.config.declare({
    key = KEY,
    kind = "boolean",
    default = false,
    ui = {
      tab = "graphic_rendering",
      after = "rendering.dc_mode",
      is_available = function()
        return trx.game.can_restart_level()
      end,
      format_value = function()
        return ""
      end,
      -- The press restarts the level instead of changing the value.
      request_change_value = function()
        trx.game.restart_level()
        return true
      end,
    },
  })
end

return M
