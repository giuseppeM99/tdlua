
local botid = 0

local function saveid(extra, result)
  botid = result.id
end

function tdbot_update_callback()
  if res._ == "updateNewMessage" then
    tdbot_function({_ = "getMe"}, saveid)
  end
end
