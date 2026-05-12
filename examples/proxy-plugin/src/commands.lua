local commands = {}

function commands.register()
    cmd("where", function(src)
        src:send("@where", { server = src.server })
    end)

    cmd("lobby", function(src)
        if not servers.exists(config.lobby) then
            src:send("@no_lobby", { lobby = config.lobby })
            return
        end
        src:connect(config.lobby)
    end)
end

return commands
