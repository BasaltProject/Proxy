on('player.login', function(e) log.info('login ran ' .. tostring(e.cancelled)) if e.player_name == 'Bad' then e.cancelled = true e.reason = 'nope' end end)
on('player.server_pre_connect', {priority = 'high'}, function(e) wait(1) if e.server == 'a' then e.server = 'b' end end)
