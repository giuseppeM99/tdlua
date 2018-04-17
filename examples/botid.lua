
local botid = 0

local function saveid(extra, result)
  botid = result.id
  print("bot id", botid)
end

function tdbot_update_callback(res)
  if res._ == "updateNewMessage" then
    tdbot_function({_ = "getMe"}, saveid)
  end
end

require 'tdbot'
