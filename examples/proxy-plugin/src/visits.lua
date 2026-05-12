local visits = {}

function visits.bump(player)
    local count = (player.data.visits or 0) + 1
    player.data.visits = count
    return count
end

return visits
