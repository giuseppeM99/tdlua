local serpent = require 'serpent'
local tdbot = require 'tdbot'
local function vardump(w)
    print(serpent.block(w, {comment=false}))
end

local botid = nil

local tdbot_function = tdbot.getCallback()

local function saveid(_, result)
  botid = result.id
  print("bot id", botid)
end

local function dl_cb(extra, res)
    print("---EXTRA----")
    vardump(extra)
    print("----RES----")
    vardump(res)
end

local function parseTextMD(text)
   local obj
   tdbot_function({["@type"] = "parseTextEntities", text=text, parse_mode = {["@type"] = "textParseModeMarkdown"}},
    function(_, res) obj = res end)
   return obj
end

local function tdbot_update_callback (data)
    if not botid then
        tdbot_function({_ = "getMe"}, saveid)
    end
    if (data._ == "updateNewMessage") then
      local msg = data.message
      if msg.content._ == "messageText" then
      if msg.content.text.text == "ping" then
        print("PONG!")
        assert (tdbot_function ({_="sendMessage", chat_id=msg.chat_id, reply_to_message_id=msg.id,
        disable_notification=false, from_background=true, reply_markup=nil,
        input_message_content={_="inputMessageText", text=parseTextMD("*PONG!*"),
        disable_web_page_preview=true, clear_draft=false}}, dl_cb, nil))
      end
    end
  end
end

tdbot.run(tdbot_update_callback)
