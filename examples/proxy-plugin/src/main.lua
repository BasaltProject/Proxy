-- Wires the modules together; registers nothing itself.

local visits = require("visits")
local events = require("events")
local commands = require("commands")

events.register(visits)
commands.register()
