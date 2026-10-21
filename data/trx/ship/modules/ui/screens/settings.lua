-- The settings dialogs of the inventory ring: the graphics, the sound and the
-- gameplay settings, each a dialog of tabs, and the controls.

local ring = require("common.ui.ring")
local timer = require("common.timer")

-- The dialogs' modules come in when one is first used rather than at the
-- start: a hundred KiB that the Dreamcast's heap needs for the levels.
local loaded = {}
local function lazy(name)
  local proxy = {}
  return setmetatable(proxy, {
    __index = function(_, key)
      local module = require(name)
      loaded[proxy] = true
      setmetatable(proxy, { __index = module })
      return module[key]
    end,
  })
end

local color_editor = lazy("common.ui.settings.color_editor")
local controls = lazy("common.ui.settings.controls")
local dialog = lazy("common.ui.settings.dialog")
local editor = lazy("common.ui.settings.editor")
local handlers = lazy("common.ui.settings.handlers")
local preset_tab = lazy("common.ui.settings.preset_tab")
local tabs = lazy("common.ui.settings.tabs")

local M = {}

-- What each dialog holds: its title, and its tabs, each the settings tab it
-- shows or the presets. The text is read again whenever a setting changes,
-- so that it follows the player's language.
local DIALOGS = {
  graphics = function()
    -- The Dreamcast shows the rendering tab alone (see ring/entries.lua).
    if pcall(trx.config.describe, "rendering.dc_mode") then
      return trx.locale.get("general/settings/graphic_settings/title"),
        {
          {
            trx.locale.get("general/settings/graphic_settings/tabs/rendering"),
            "graphic_rendering",
          },
        }
    end
    return trx.locale.get("general/settings/graphic_settings/title"),
      {
        {
          trx.locale.get("general/settings/graphic_settings/tabs/visuals"),
          "graphic_visuals",
        },
        {
          trx.locale.get("general/settings/graphic_settings/tabs/ui"),
          "graphic_ui",
        },
        {
          trx.locale.get("general/settings/graphic_settings/tabs/stats"),
          "graphic_ui_stats",
        },
        {
          trx.locale.get("general/settings/graphic_settings/tabs/bars"),
          "graphic_ui_bars",
        },
        {
          trx.locale.get("general/settings/graphic_settings/tabs/rendering"),
          "graphic_rendering",
        },
      }
  end,
  sound = function()
    return trx.locale.get("general/settings/sound/title"),
      {
        {
          trx.locale.get("general/settings/sound/tabs/volume"),
          "sound_volume",
        },
        { trx.locale.get("general/settings/sound/tabs/misc"), "sound_misc" },
      }
  end,
  gameplay = function()
    return trx.locale.get("general/settings/gameplay/title"),
      {
        {
          trx.locale.get("general/settings/gameplay/tabs/general"),
          "gameplay_general",
        },
        {
          trx.locale.get("general/settings/gameplay/tabs/controls"),
          "gameplay_controls",
        },
        {
          trx.locale.get("general/settings/gameplay/tabs/mods"),
          "gameplay_mods",
        },
        {
          trx.locale.get("general/settings/gameplay/tabs/fixes"),
          "gameplay_fixes",
        },
        { trx.locale.get("general/settings/gameplay/tabs/presets") },
      }
  end,
}

-- The rows that scripts declared, by the tab that each sits on.
local function declared_by_tab()
  handlers.add_declared()
  local result = {}
  for _, entry in ipairs(trx.config.declared_rows()) do
    local tab = entry.row.tab
    if not tabs.exists(tab) then
      trx.log.warn(
        ("the setting %s names no settings tab: %s"):format(entry.key, tab)
      )
    else
      result[tab] = result[tab] or {}
      table.insert(result[tab], {
        key = entry.key,
        before = entry.row.before,
        after = entry.row.after,
      })
    end
  end
  return result
end

-- Puts the entry away, playing its animation back to the start as it goes.
local function close(ctx)
  if ring.selection_anim() ~= nil then
    ring.animate_selection(0, -1)
  end
  ctx:cancel()
end

-- Builds one of the dialogs, opening anything it opens over itself through
-- `push`.
local function build_dialog(which, push)
  local _, specs = DIALOGS[which]()
  local declared = declared_by_tab()
  return dialog.new({
    title = function()
      return (DIALOGS[which]())
    end,
    tabs = function(self)
      local names = self.changed:map(function()
        local _, now = DIALOGS[which]()
        return now
      end)
      local result = {}
      for i, spec in ipairs(specs) do
        local name = spec[2]
        local title = names:map(function(now)
          return now[i][1]
        end)
        if name == nil then
          result[i] = preset_tab.new({
            title = title,
            editing = self.editing,
            changed = self.changed,
            push = push,
          })
        else
          result[i] = editor.new({
            title = title,
            keys = tabs.arrange(name, declared[name] or {}),
            editing = self.editing,
            width = self.width,
            dialog_width = self.dialog_width,
            changed = self.changed,
            layout = self.layout,
            push = push,
            edit_color = function(key)
              color_editor.open(push, key)
            end,
          })
        end
      end
      return result
    end,
  })
end

-- Where the player left each dialog, which it opens on again until the level
-- ends, as the original games do.
---@type table<string, common.ui.settings.Dialog.Place>
local places = {}

local function opener(which)
  ---@param ctx trx.ui.EntryContext
  return function(ctx)
    -- The dialog is the largest Lua the game builds: it starts from a
    -- collected heap, and what it leaves goes as soon as it closes.
    collectgarbage("collect")
    local settings_dialog = build_dialog(which, function(settings)
      return ctx:push(settings)
    end)
    if places[which] ~= nil then
      settings_dialog:restore(places[which])
    end
    local resting = ring.resting()
    ctx:on_end(function()
      places[which] = settings_dialog:remember()
      settings_dialog:close()
      resting:stop()
      timer.after(0, function()
        collectgarbage("collect")
      end, { clock = timer.Clock.UI })
    end)
    return ctx:push({
      root = trx.ui.widgets.Stack({
        visible = resting,
        children = { settings_dialog.widget },
      }),
      place = dialog.PLACE,
      on_input = ring.while_resting(function(_, keys)
        if settings_dialog:control(keys) then
          close(ctx)
        end
      end),
    })
  end
end

-- Opens the controls dialog. Confirming out of it leaves the ring, as using
-- an entry does, except on the title screen, where it puts the entry away.
---@param ctx trx.ui.EntryContext
local function open_controls(ctx)
  local resting = ring.resting()
  local layer, close_dialog = controls.open({
    push = function(settings)
      return ctx:push(settings)
    end,
    finish = function(confirmed)
      local title = ring.mode() == trx.ui.MenuMode.TITLE
      if confirmed and not title then
        ctx:confirm()
      else
        close(ctx)
      end
    end,
    visible = resting,
    wrap = ring.while_resting,
  })
  ctx:on_end(function()
    close_dialog()
    resting:stop()
  end)
  return layer
end

function M.setup()
  trx.events.on_level_unload(function()
    places = {}
    if loaded[controls] then
      controls.forget()
    end
  end)
  trx.ui.screens.define(
    trx.ui.Screen.RING_ENTRY,
    open_controls,
    { object = trx.catalog.objects.CONTROLS_OPTION }
  )
  for object, which in pairs({
    [trx.catalog.objects.GRAPHICS_OPTION] = "graphics",
    [trx.catalog.objects.SOUND_OPTION] = "sound",
    [trx.catalog.objects.PDA_OPTION] = "gameplay",
  }) do
    trx.ui.screens.define(
      trx.ui.Screen.RING_ENTRY,
      opener(which),
      { object = object }
    )
  end
end

return M
