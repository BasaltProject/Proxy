local events = {}

function events.register(visits)
    on("player.join", function(e)
        local count = visits.bump(e.player)
        e.player:send("@welcome", { name = e.player_name, visits = count })
    end)

    on("player.server_change", function(e)
        log.info(e.player_name .. " moved from " .. e.from_server .. " to " .. e.server)
    end)
end

return events
